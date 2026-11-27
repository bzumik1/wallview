use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

use bluer::adv::{Advertisement, AdvertisementHandle, Type};
use bluer::gatt::local::{
    Application, ApplicationHandle, Characteristic, CharacteristicNotifier, CharacteristicNotify,
    CharacteristicNotifyMethod, CharacteristicRead, CharacteristicWrite, CharacteristicWriteMethod, Service,
};
use bluer::Uuid;
use tokio::sync::Mutex;

use crate::device::Device;
use crate::wifi;

const SERVICE: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268000);
const CURRENT_STATE: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268001);
const ERROR_STATE: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268002);
const RPC_COMMAND: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268003);
const RPC_RESULT: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268004);
const CAPABILITIES: Uuid = Uuid::from_u128(0x00467768_6228_2272_4663_277478268005);
const SERVICE_DATA: Uuid = Uuid::from_u128(0x00004677_0000_1000_8000_00805f9b34fb);

const STATE_AUTHORIZED: u8 = 0x02;
const STATE_PROVISIONING: u8 = 0x03;
const STATE_PROVISIONED: u8 = 0x04;
const ERROR_NONE: u8 = 0x00;
const ERROR_INVALID_RPC: u8 = 0x01;
const ERROR_UNKNOWN_RPC: u8 = 0x02;
const ERROR_UNABLE_TO_CONNECT: u8 = 0x03;
const COMMAND_WIFI_SETTINGS: u8 = 0x01;
const COMMAND_IDENTIFY: u8 = 0x02;
const CAPABILITY_IDENTIFY: u8 = 0x01;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Default)]
struct State {
    current: u8,
    error: u8,
    result: Vec<u8>,
    state_notifier: Option<CharacteristicNotifier>,
    error_notifier: Option<CharacteristicNotifier>,
    result_notifier: Option<CharacteristicNotifier>,
}

type Shared = Arc<Mutex<State>>;

fn checksum(bytes: &[u8]) -> u8 {
    bytes.iter().fold(0u8, |sum, byte| sum.wrapping_add(*byte))
}

fn parse_rpc(packet: &[u8]) -> Option<(u8, &[u8])> {
    if packet.len() < 3 {
        return None;
    }
    let length = packet[1] as usize;
    if packet.len() < length + 3 || checksum(&packet[..length + 2]) != packet[length + 2] {
        return None;
    }
    Some((packet[0], &packet[2..length + 2]))
}

fn parse_wifi(data: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    let ssid_length = *data.first()? as usize;
    let ssid = data.get(1..1 + ssid_length)?.to_vec();
    let password_length = *data.get(1 + ssid_length)? as usize;
    let password = data.get(2 + ssid_length..2 + ssid_length + password_length)?.to_vec();
    Some((ssid, password))
}

fn rpc_result(command: u8, strings: &[&str]) -> Vec<u8> {
    let mut data = Vec::new();
    for string in strings {
        data.push(string.len() as u8);
        data.extend_from_slice(string.as_bytes());
    }
    let mut packet = vec![command, data.len() as u8];
    packet.extend_from_slice(&data);
    packet.push(checksum(&packet));
    packet
}

async fn notify(notifier: &mut Option<CharacteristicNotifier>, value: Vec<u8>) {
    if let Some(active) = notifier {
        if active.notify(value).await.is_err() {
            *notifier = None;
        }
    }
}

async fn set_state(shared: &Shared, current: u8, error: u8) {
    let mut state = shared.lock().await;
    state.current = current;
    state.error = error;
    let mut state_notifier = state.state_notifier.take();
    let mut error_notifier = state.error_notifier.take();
    notify(&mut state_notifier, vec![current]).await;
    notify(&mut error_notifier, vec![error]).await;
    state.state_notifier = state_notifier;
    state.error_notifier = error_notifier;
}

async fn set_result(shared: &Shared, result: Vec<u8>) {
    let mut state = shared.lock().await;
    state.result = result.clone();
    let mut notifier = state.result_notifier.take();
    notify(&mut notifier, result).await;
    state.result_notifier = notifier;
}

async fn handle_command(shared: Shared, device: Device, packet: Vec<u8>) {
    let Some((command, data)) = parse_rpc(&packet) else {
        set_state(&shared, STATE_AUTHORIZED, ERROR_INVALID_RPC).await;
        return;
    };
    match command {
        COMMAND_WIFI_SETTINGS => {
            let Some((ssid, password)) = parse_wifi(data) else {
                set_state(&shared, STATE_AUTHORIZED, ERROR_INVALID_RPC).await;
                return;
            };
            set_state(&shared, STATE_PROVISIONING, ERROR_NONE).await;
            let connected = tokio::task::spawn_blocking(move || wifi::apply(&ssid, &password, CONNECT_TIMEOUT))
                .await
                .ok()
                .flatten();
            match connected {
                Some(address) => {
                    set_result(&shared, rpc_result(COMMAND_WIFI_SETTINGS, &[&format!("http://{address}")])).await;
                    set_state(&shared, STATE_PROVISIONED, ERROR_NONE).await;
                }
                None => set_state(&shared, STATE_AUTHORIZED, ERROR_UNABLE_TO_CONNECT).await,
            }
        }
        COMMAND_IDENTIFY => {
            tokio::task::spawn_blocking(move || device.identify());
        }
        _ => set_state(&shared, STATE_AUTHORIZED, ERROR_UNKNOWN_RPC).await,
    }
}

fn read_value(shared: &Shared, select: fn(&State) -> Vec<u8>) -> CharacteristicRead {
    let shared = shared.clone();
    CharacteristicRead {
        read: true,
        fun: Box::new(move |_| {
            let shared = shared.clone();
            Box::pin(async move { Ok(select(&*shared.lock().await)) })
        }),
        ..Default::default()
    }
}

fn notify_into(shared: &Shared, select: fn(&mut State) -> &mut Option<CharacteristicNotifier>) -> CharacteristicNotify {
    let shared = shared.clone();
    CharacteristicNotify {
        notify: true,
        method: CharacteristicNotifyMethod::Fun(Box::new(move |notifier| {
            let shared = shared.clone();
            Box::pin(async move {
                *select(&mut *shared.lock().await) = Some(notifier);
            })
        })),
        ..Default::default()
    }
}

fn application(shared: &Shared, device: &Device) -> Application {
    let command_shared = shared.clone();
    let command_device = device.clone();
    Application {
        services: vec![Service {
            uuid: SERVICE,
            primary: true,
            characteristics: vec![
                Characteristic {
                    uuid: CURRENT_STATE,
                    read: Some(read_value(shared, |state| vec![state.current])),
                    notify: Some(notify_into(shared, |state| &mut state.state_notifier)),
                    ..Default::default()
                },
                Characteristic {
                    uuid: ERROR_STATE,
                    read: Some(read_value(shared, |state| vec![state.error])),
                    notify: Some(notify_into(shared, |state| &mut state.error_notifier)),
                    ..Default::default()
                },
                Characteristic {
                    uuid: RPC_COMMAND,
                    write: Some(CharacteristicWrite {
                        write: true,
                        write_without_response: true,
                        method: CharacteristicWriteMethod::Fun(Box::new(move |value, _| {
                            let shared = command_shared.clone();
                            let device = command_device.clone();
                            Box::pin(async move {
                                tokio::spawn(handle_command(shared, device, value));
                                Ok(())
                            })
                        })),
                        ..Default::default()
                    }),
                    ..Default::default()
                },
                Characteristic {
                    uuid: RPC_RESULT,
                    read: Some(read_value(shared, |state| state.result.clone())),
                    notify: Some(notify_into(shared, |state| &mut state.result_notifier)),
                    ..Default::default()
                },
                Characteristic {
                    uuid: CAPABILITIES,
                    read: Some(read_value(shared, |_| vec![CAPABILITY_IDENTIFY])),
                    ..Default::default()
                },
            ],
            ..Default::default()
        }],
        ..Default::default()
    }
}

fn advertisement(device: &Device, current: u8) -> Advertisement {
    Advertisement {
        advertisement_type: Type::Peripheral,
        service_uuids: [SERVICE].into_iter().collect(),
        service_data: BTreeMap::from([(SERVICE_DATA, vec![current, CAPABILITY_IDENTIFY, 0, 0, 0, 0])]),
        discoverable: Some(true),
        local_name: Some(device.name.clone()),
        ..Default::default()
    }
}

async fn serve(device: Device) -> bluer::Result<()> {
    let session = bluer::Session::new().await?;
    let adapter = session.default_adapter().await?;
    adapter.set_powered(true).await?;
    let shared: Shared = Arc::new(Mutex::new(State { current: STATE_AUTHORIZED, ..Default::default() }));
    let _application: ApplicationHandle = adapter.serve_gatt_application(application(&shared, &device)).await?;
    let mut advertised = STATE_AUTHORIZED;
    let mut handle: Option<AdvertisementHandle> = Some(adapter.advertise(advertisement(&device, advertised)).await?);
    loop {
        tokio::time::sleep(Duration::from_secs(1)).await;
        let current = shared.lock().await.current;
        if current != advertised {
            drop(handle.take());
            advertised = current;
            handle = Some(adapter.advertise(advertisement(&device, advertised)).await?);
        }
        if current == STATE_PROVISIONED {
            tokio::time::sleep(Duration::from_secs(30)).await;
            drop(handle);
            return Ok(());
        }
    }
}

pub fn run(device: Device) {
    let _bluetooth = device.bluetooth.hold();
    let runtime = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("wallview-agent: improv: {error}");
            return;
        }
    };
    if let Err(error) = runtime.block_on(serve(device)) {
        eprintln!("wallview-agent: improv: {error}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_wifi_rpc() {
        let mut packet = vec![COMMAND_WIFI_SETTINGS, 9, 3, b'n', b'e', b't', 4, b'p', b'a', b's', b's'];
        packet.push(checksum(&packet));
        let (command, data) = parse_rpc(&packet).unwrap();
        assert_eq!(command, COMMAND_WIFI_SETTINGS);
        assert_eq!(parse_wifi(data).unwrap(), (b"net".to_vec(), b"pass".to_vec()));
    }

    #[test]
    fn rejects_bad_checksum() {
        assert!(parse_rpc(&[COMMAND_WIFI_SETTINGS, 0, 0]).is_none());
    }
}
