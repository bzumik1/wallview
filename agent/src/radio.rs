//! Onboard Zigbee/Thread radio (EFR32MG21 on UART5).
//!
//! The radio runs in one mode at a time, chosen with the "Radio mode" select in Home Assistant
//! and stored in `/data/wallview/radio-mode`. Today the only active mode is the Zigbee
//! coordinator: the factory EZSP firmware is shared over TCP (port 6638, like ser2net) and
//! advertised as `_zigbee-coordinator._tcp`, so ZHA offers it automatically and Zigbee2MQTT can
//! use `tcp://<panel>:6638`. Router and Thread modes need other firmware and come later.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use mdns_sd::{ServiceDaemon, ServiceInfo};

const SERIAL: &str = "/dev/ttyS5";
/// Overrides the UART path, for testing the bridge against a pseudo terminal.
const SERIAL_ENV: &str = "WALLVIEW_RADIO_SERIAL";
const MODE_FILE: &str = "/data/wallview/radio-mode";
pub const BRIDGE_PORT: u16 = 6638;
const SERVICE_TYPE: &str = "_zigbee-coordinator._tcp.local.";
const IDLE: Duration = Duration::from_millis(500);
const BUFFER: usize = 1024;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    Off,
    ZigbeeCoordinator,
}

impl Mode {
    pub const ALL: [Mode; 2] = [Mode::Off, Mode::ZigbeeCoordinator];

    pub fn label(self) -> &'static str {
        match self {
            Mode::Off => "Off",
            Mode::ZigbeeCoordinator => "Zigbee coordinator",
        }
    }

    fn id(self) -> &'static str {
        match self {
            Mode::Off => "off",
            Mode::ZigbeeCoordinator => "zigbee-coordinator",
        }
    }

    pub fn from_label(label: &str) -> Option<Mode> {
        Mode::ALL.into_iter().find(|mode| mode.label() == label)
    }

    fn from_id(id: &str) -> Option<Mode> {
        Mode::ALL.into_iter().find(|mode| mode.id() == id)
    }
}

#[derive(Clone)]
pub struct Radio {
    mode: Arc<Mutex<Mode>>,
    client: Arc<AtomicBool>,
    changes: Arc<AtomicU64>,
}

impl Radio {
    pub fn load() -> Self {
        let mode = fs::read_to_string(MODE_FILE)
            .ok()
            .and_then(|text| Mode::from_id(text.trim()))
            .unwrap_or(Mode::Off);
        Radio { mode: Arc::new(Mutex::new(mode)), client: Arc::new(AtomicBool::new(false)), changes: Arc::new(AtomicU64::new(0)) }
    }

    pub fn mode(&self) -> Mode {
        *self.mode.lock().unwrap()
    }

    pub fn set_mode(&self, mode: Mode) {
        let mut current = self.mode.lock().unwrap();
        if *current == mode {
            return;
        }
        if let Err(error) = fs::write(MODE_FILE, format!("{}\n", mode.id())) {
            eprintln!("wallview-agent: radio: cannot store mode: {error}");
        }
        *current = mode;
        self.changes.fetch_add(1, Ordering::SeqCst);
    }

    /// True while a Zigbee host (ZHA, Zigbee2MQTT) is connected to the bridge.
    pub fn client_connected(&self) -> bool {
        self.client.load(Ordering::SeqCst)
    }

    /// Increases on every change of mode or client, so API connections know when to send states.
    pub fn changes(&self) -> u64 {
        self.changes.load(Ordering::SeqCst)
    }

    fn set_client(&self, connected: bool) {
        if self.client.swap(connected, Ordering::SeqCst) != connected {
            self.changes.fetch_add(1, Ordering::SeqCst);
        }
    }
}

/// Runs the radio for the lifetime of the agent, following the selected mode.
pub fn run(radio: Radio, node_name: String, friendly_name: String, serial_number: String) {
    loop {
        match radio.mode() {
            Mode::Off => thread::sleep(IDLE),
            Mode::ZigbeeCoordinator => {
                if let Err(error) = bridge(&radio, &node_name, &friendly_name, &serial_number) {
                    eprintln!("wallview-agent: radio: {error}");
                    thread::sleep(Duration::from_secs(5));
                }
            }
        }
    }
}

struct Advertisement {
    daemon: &'static ServiceDaemon,
    fullname: String,
}

impl Advertisement {
    fn start(node_name: &str, friendly_name: &str, serial_number: &str) -> Option<Self> {
        let daemon = crate::mdns::daemon()?;
        let properties = [("radio_type", "ezsp"), ("serial_number", serial_number), ("name", friendly_name)];
        let service =
            ServiceInfo::new(SERVICE_TYPE, friendly_name, &format!("{node_name}.local."), "", BRIDGE_PORT, &properties[..])
                .map_err(|error| eprintln!("wallview-agent: radio mdns: {error}"))
                .ok()?
                .enable_addr_auto();
        let fullname = service.get_fullname().to_string();
        daemon.register(service).map_err(|error| eprintln!("wallview-agent: radio mdns: {error}")).ok()?;
        Some(Advertisement { daemon, fullname })
    }
}

impl Drop for Advertisement {
    fn drop(&mut self) {
        let _ = self.daemon.unregister(&self.fullname);
    }
}

/// Opens the UART raw at 115200 8N1 without flow control. Reads return after 0.2 s without data,
/// so the copy loops can notice a mode change. The EZSP firmware uses XON/XOFF, which ASH
/// escapes inside frames; the bytes are passed through untouched and the host ignores them.
fn open_serial() -> io::Result<File> {
    let file = OpenOptions::new().read(true).write(true).custom_flags(libc::O_NOCTTY).open(std::env::var(SERIAL_ENV).unwrap_or_else(|_| SERIAL.to_string()))?;
    let fd = file.as_raw_fd();
    unsafe {
        let mut termios: libc::termios = std::mem::zeroed();
        if libc::tcgetattr(fd, &mut termios) != 0 {
            return Err(io::Error::last_os_error());
        }
        libc::cfmakeraw(&mut termios);
        termios.c_cflag &= !(libc::CRTSCTS | libc::CSTOPB | libc::PARENB);
        termios.c_cflag |= libc::CLOCAL | libc::CREAD;
        termios.c_iflag &= !(libc::IXON | libc::IXOFF | libc::IXANY);
        termios.c_cc[libc::VMIN] = 0;
        termios.c_cc[libc::VTIME] = 2;
        libc::cfsetispeed(&mut termios, libc::B115200);
        libc::cfsetospeed(&mut termios, libc::B115200);
        if libc::tcsetattr(fd, libc::TCSANOW, &termios) != 0 {
            return Err(io::Error::last_os_error());
        }
        libc::tcflush(fd, libc::TCIOFLUSH);
    }
    Ok(file)
}

/// Serves the UART to one TCP client at a time while the mode is Zigbee coordinator. A new
/// client replaces the old one, so a restarted Home Assistant does not find the port busy.
fn bridge(radio: &Radio, node_name: &str, friendly_name: &str, serial_number: &str) -> io::Result<()> {
    let serial = open_serial()?;
    let listener = TcpListener::bind(("0.0.0.0", BRIDGE_PORT))?;
    listener.set_nonblocking(true)?;
    let _advertisement = Advertisement::start(node_name, friendly_name, serial_number);
    let mut session: Option<Session> = None;

    while radio.mode() == Mode::ZigbeeCoordinator {
        match listener.accept() {
            Ok((stream, peer)) => {
                eprintln!("wallview-agent: radio: Zigbee host {peer} connected");
                if let Some(old) = session.take() {
                    old.stop();
                }
                session = Some(Session::start(stream, &serial, radio.clone())?);
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => thread::sleep(IDLE),
            Err(error) => return Err(error),
        }
        if session.as_ref().is_some_and(Session::finished) {
            session.take().unwrap().stop();
            eprintln!("wallview-agent: radio: Zigbee host disconnected");
        }
    }
    if let Some(session) = session {
        session.stop();
    }
    Ok(())
}

struct Session {
    stream: TcpStream,
    stop: Arc<AtomicBool>,
    workers: Vec<thread::JoinHandle<()>>,
    radio: Radio,
}

impl Session {
    fn start(stream: TcpStream, serial: &File, radio: Radio) -> io::Result<Self> {
        stream.set_nodelay(true)?;
        stream.set_nonblocking(false)?;
        stream.set_read_timeout(Some(Duration::from_millis(200)))?;
        let stop = Arc::new(AtomicBool::new(false));

        let mut from_serial = serial.try_clone()?;
        let mut to_client = stream.try_clone()?;
        let uplink_stop = stop.clone();
        let uplink = thread::spawn(move || {
            let mut buffer = [0u8; BUFFER];
            while !uplink_stop.load(Ordering::SeqCst) {
                match from_serial.read(&mut buffer) {
                    Ok(0) => {}
                    Ok(count) => {
                        if to_client.write_all(&buffer[..count]).is_err() {
                            break;
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                    Err(error) => {
                        eprintln!("wallview-agent: radio: serial read: {error}");
                        break;
                    }
                }
            }
            uplink_stop.store(true, Ordering::SeqCst);
        });

        let mut to_serial = serial.try_clone()?;
        let mut from_client = stream.try_clone()?;
        let downlink_stop = stop.clone();
        let downlink = thread::spawn(move || {
            let mut buffer = [0u8; BUFFER];
            while !downlink_stop.load(Ordering::SeqCst) {
                match from_client.read(&mut buffer) {
                    Ok(0) => break,
                    Ok(count) => {
                        if let Err(error) = to_serial.write_all(&buffer[..count]) {
                            eprintln!("wallview-agent: radio: serial write: {error}");
                            break;
                        }
                    }
                    Err(error)
                        if matches!(
                            error.kind(),
                            io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
                        ) => {}
                    Err(_) => break,
                }
            }
            downlink_stop.store(true, Ordering::SeqCst);
        });

        radio.set_client(true);
        Ok(Session { stream, stop, workers: vec![uplink, downlink], radio })
    }

    fn finished(&self) -> bool {
        self.stop.load(Ordering::SeqCst)
    }

    fn stop(self) {
        self.stop.store(true, Ordering::SeqCst);
        let _ = self.stream.shutdown(Shutdown::Both);
        for worker in self.workers {
            let _ = worker.join();
        }
        self.radio.set_client(false);
    }
}
