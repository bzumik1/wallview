mod entities;
mod frame;
mod mdns;

use std::fs;
use std::io;
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use base64::Engine;
use prost::Message;

use crate::api::{
    DeviceInfoResponse, DisconnectResponse, HelloResponse, HomeassistantActionRequest, ListEntitiesDoneResponse,
    NoiseEncryptionSetKeyRequest, NoiseEncryptionSetKeyResponse, PingResponse,
};
use crate::device::Device;

use frame::{Connection, Sender};

const PORT: u16 = 6053;
const ESPHOME_VERSION: &str = "2025.9.0";
const SENSOR_INTERVAL: Duration = Duration::from_secs(15);
const POLL_INTERVAL: Duration = Duration::from_millis(500);
const SIGN_IN_REQUESTED_EVENT: &str = "esphome.wallview_sign_in_requested";

pub mod message_type {
    pub const HELLO_REQUEST: u16 = 1;
    pub const HELLO_RESPONSE: u16 = 2;
    pub const DISCONNECT_REQUEST: u16 = 5;
    pub const DISCONNECT_RESPONSE: u16 = 6;
    pub const PING_REQUEST: u16 = 7;
    pub const PING_RESPONSE: u16 = 8;
    pub const DEVICE_INFO_REQUEST: u16 = 9;
    pub const DEVICE_INFO_RESPONSE: u16 = 10;
    pub const LIST_ENTITIES_REQUEST: u16 = 11;
    pub const LIST_ENTITIES_BINARY_SENSOR: u16 = 12;
    pub const LIST_ENTITIES_LIGHT: u16 = 15;
    pub const LIST_ENTITIES_SENSOR: u16 = 16;
    pub const LIST_ENTITIES_TEXT_SENSOR: u16 = 18;
    pub const LIST_ENTITIES_DONE: u16 = 19;
    pub const SUBSCRIBE_STATES_REQUEST: u16 = 20;
    pub const BINARY_SENSOR_STATE: u16 = 21;
    pub const LIGHT_STATE: u16 = 24;
    pub const SENSOR_STATE: u16 = 25;
    pub const TEXT_SENSOR_STATE: u16 = 27;
    pub const LIST_ENTITIES_SERVICES: u16 = 41;
    pub const EXECUTE_SERVICE_REQUEST: u16 = 42;
    pub const LIGHT_COMMAND_REQUEST: u16 = 32;
    pub const SUBSCRIBE_HOMEASSISTANT_SERVICES_REQUEST: u16 = 34;
    pub const HOMEASSISTANT_ACTION_REQUEST: u16 = 35;
    pub const LIST_ENTITIES_BUTTON: u16 = 61;
    pub const BUTTON_COMMAND_REQUEST: u16 = 62;
    pub const LIST_ENTITIES_TEXT: u16 = 97;
    pub const TEXT_STATE: u16 = 98;
    pub const TEXT_COMMAND_REQUEST: u16 = 99;
    pub const NOISE_ENCRYPTION_SET_KEY_REQUEST: u16 = 124;
    pub const NOISE_ENCRYPTION_SET_KEY_RESPONSE: u16 = 125;
}

#[derive(Clone)]
pub struct KeyStore {
    path: String,
    key: Arc<Mutex<Option<[u8; 32]>>>,
    generation: Arc<AtomicU64>,
}

impl KeyStore {
    pub fn load(path: &str) -> Self {
        let key = fs::read_to_string(path).ok().and_then(|text| {
            base64::engine::general_purpose::STANDARD.decode(text.trim()).ok()?.try_into().ok()
        });
        KeyStore { path: path.to_string(), key: Arc::new(Mutex::new(key)), generation: Arc::new(AtomicU64::new(0)) }
    }

    pub fn current(&self) -> Option<[u8; 32]> {
        *self.key.lock().unwrap()
    }

    fn store(&self, key: [u8; 32]) -> io::Result<()> {
        let encoded = base64::engine::general_purpose::STANDARD.encode(key);
        let temporary = format!("{}.new", self.path);
        fs::write(&temporary, format!("{encoded}\n"))?;
        fs::rename(&temporary, &self.path)?;
        *self.key.lock().unwrap() = Some(key);
        self.generation.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }

    fn generation(&self) -> u64 {
        self.generation.load(Ordering::SeqCst)
    }
}

pub fn serve(device: Device, keys: KeyStore) {
    let advertisement = mdns::Advertisement::start(&device, keys.current().is_some());
    let listener = match TcpListener::bind(("0.0.0.0", PORT)) {
        Ok(listener) => listener,
        Err(error) => {
            eprintln!("wallview-agent: cannot listen on port {PORT}: {error}");
            std::process::exit(1);
        }
    };
    let advertisement = Arc::new(Mutex::new(advertisement));
    for stream in listener.incoming().flatten() {
        let device = device.clone();
        let keys = keys.clone();
        let advertisement = advertisement.clone();
        thread::spawn(move || {
            let peer = stream.peer_addr().map(|address| address.to_string()).unwrap_or_default();
            if let Err(error) = handle_client(stream, &device, &keys, &advertisement) {
                eprintln!("wallview-agent: {peer}: {error}");
            }
        });
    }
}

#[derive(Default)]
struct Subscriptions {
    states: bool,
    actions: bool,
}

enum Incoming {
    Message(u16, Vec<u8>),
    Closed(io::Error),
}

fn handle_client(
    stream: TcpStream,
    device: &Device,
    keys: &KeyStore,
    advertisement: &Arc<Mutex<Option<mdns::Advertisement>>>,
) -> io::Result<()> {
    stream.set_nodelay(true)?;
    let mut connection = Connection::accept(stream, keys.current(), &device.node_name(), &device.id)?;
    let sender = connection.sender();

    let (incoming, messages) = mpsc::channel();
    thread::spawn(move || loop {
        match connection.receive() {
            Ok((message_type, data)) => {
                if incoming.send(Incoming::Message(message_type, data)).is_err() {
                    return;
                }
            }
            Err(error) => {
                let _ = incoming.send(Incoming::Closed(error));
                return;
            }
        }
    });

    let generation = keys.generation();
    let mut subscriptions = Subscriptions::default();
    let mut sign_in_requests = device.sign_in_requests();
    let mut last_presence = None;
    let mut last_sensors = Instant::now();
    loop {
        if keys.generation() != generation {
            sender.close();
            return Ok(());
        }
        match messages.recv_timeout(POLL_INTERVAL) {
            Ok(Incoming::Message(message_type, data)) => {
                if !handle_message(&sender, device, keys, advertisement, message_type, &data, &mut subscriptions)? {
                    return Ok(());
                }
            }
            Ok(Incoming::Closed(error)) => {
                return if error.kind() == io::ErrorKind::UnexpectedEof { Ok(()) } else { Err(error) };
            }
            Err(RecvTimeoutError::Disconnected) => return Ok(()),
            Err(RecvTimeoutError::Timeout) => {}
        }
        if device.sign_in_requests() != sign_in_requests {
            sign_in_requests = device.sign_in_requests();
            if subscriptions.actions {
                send_message(
                    &sender,
                    message_type::HOMEASSISTANT_ACTION_REQUEST,
                    HomeassistantActionRequest {
                        service: SIGN_IN_REQUESTED_EVENT.into(),
                        is_event: true,
                        ..Default::default()
                    },
                )?;
            }
        }
        if !subscriptions.states {
            continue;
        }
        if let Some((near, state)) = entities::presence_state(device) {
            if last_presence != Some(near) {
                last_presence = Some(near);
                sender.send(state.0, &state.1)?;
            }
        }
        if last_sensors.elapsed() >= SENSOR_INTERVAL {
            last_sensors = Instant::now();
            for (message_type, data) in entities::sensor_states(device) {
                sender.send(message_type, &data)?;
            }
        }
    }
}

fn send_message(sender: &Sender, message_type: u16, message: impl Message) -> io::Result<()> {
    sender.send(message_type, &message.encode_to_vec())
}

fn handle_message(
    sender: &Sender,
    device: &Device,
    keys: &KeyStore,
    advertisement: &Arc<Mutex<Option<mdns::Advertisement>>>,
    message_type: u16,
    data: &[u8],
    subscriptions: &mut Subscriptions,
) -> io::Result<bool> {
    use message_type::*;
    match message_type {
        HELLO_REQUEST => send_message(
            sender,
            HELLO_RESPONSE,
            HelloResponse {
                api_version_major: 1,
                api_version_minor: 10,
                server_info: format!("WallView {}", env!("CARGO_PKG_VERSION")),
                name: device.node_name(),
                ..Default::default()
            },
        )?,
        DEVICE_INFO_REQUEST => send_message(
            sender,
            DEVICE_INFO_RESPONSE,
            DeviceInfoResponse {
                name: device.node_name(),
                friendly_name: device.name.clone(),
                mac_address: device.mac.clone(),
                esphome_version: ESPHOME_VERSION.into(),
                model: "Sonoff NSPanel Pro".into(),
                manufacturer: "WallView".into(),
                project_name: "WallView.WallView OS".into(),
                project_version: env!("CARGO_PKG_VERSION").into(),
                api_encryption_supported: true,
                api_encryption_provisionable: keys.current().is_none(),
                ..Default::default()
            },
        )?,
        LIST_ENTITIES_REQUEST => {
            for (entity_type, entity) in entities::list() {
                sender.send(entity_type, &entity)?;
            }
            send_message(sender, LIST_ENTITIES_DONE, ListEntitiesDoneResponse::default())?;
        }
        SUBSCRIBE_HOMEASSISTANT_SERVICES_REQUEST => subscriptions.actions = true,
        SUBSCRIBE_STATES_REQUEST => {
            subscriptions.states = true;
            for (state_type, state) in entities::all_states(device) {
                sender.send(state_type, &state)?;
            }
        }
        PING_REQUEST => send_message(sender, PING_RESPONSE, PingResponse::default())?,
        DISCONNECT_REQUEST => {
            send_message(sender, DISCONNECT_RESPONSE, DisconnectResponse::default())?;
            return Ok(false);
        }
        LIGHT_COMMAND_REQUEST => {
            if let Some((state_type, state)) = entities::handle_light(device, data) {
                sender.send(state_type, &state)?;
            }
        }
        BUTTON_COMMAND_REQUEST => entities::handle_button(device, data),
        EXECUTE_SERVICE_REQUEST => entities::handle_service(device, data),
        TEXT_COMMAND_REQUEST => {
            if let Some((state_type, state)) = entities::handle_text(device, data) {
                sender.send(state_type, &state)?;
            }
        }
        NOISE_ENCRYPTION_SET_KEY_REQUEST => {
            let success = NoiseEncryptionSetKeyRequest::decode(data)
                .ok()
                .and_then(|request| base64::engine::general_purpose::STANDARD.decode(&request.key).ok())
                .and_then(|key| <[u8; 32]>::try_from(key.as_slice()).ok())
                .filter(|key| *key != [0u8; 32])
                .map(|key| keys.store(key).is_ok())
                .unwrap_or(false);
            send_message(sender, NOISE_ENCRYPTION_SET_KEY_RESPONSE, NoiseEncryptionSetKeyResponse { success })?;
            if success {
                let mut advertisement = advertisement.lock().unwrap();
                *advertisement = None;
                *advertisement = mdns::Advertisement::start(device, true);
            }
        }
        _ => {}
    }
    Ok(true)
}
