//! Onboard Zigbee/Thread radio (EFR32MG21 on UART5).
//!
//! The radio runs in one mode at a time, chosen with the "Radio mode" select in Home Assistant
//! and stored in `/data/wallview/radio-mode`. Today the only active mode is the Zigbee
//! coordinator: the factory EZSP firmware is shared over TCP (port 6638, like ser2net) and
//! advertised as `_zigbee-coordinator._tcp`, so ZHA offers it automatically and Zigbee2MQTT can
//! use `tcp://<panel>:6638`. Router and Thread modes need other firmware and come later.
//!
//! The bridge serves one Zigbee host at a time and refuses others while it is connected, so a
//! port scan or a second host cannot cut off the running one (a host that takes over with a
//! different network configuration would make the radio leave the Zigbee network). Dead hosts
//! are detected with TCP keepalive. Option labels are wire values: only ever append new ones.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use mdns_sd::{ServiceDaemon, ServiceInfo};

const SERIAL: &str = "/dev/ttyS5";
/// Overrides the UART path, for testing the bridge against a pseudo terminal.
const SERIAL_ENV: &str = "WALLVIEW_RADIO_SERIAL";
const MODE_FILE: &str = "/data/wallview/radio-mode";
pub const BRIDGE_PORT: u16 = 6638;
const SERVICE_TYPE: &str = "_zigbee-coordinator._tcp.local.";
const IDLE: Duration = Duration::from_millis(500);
const BUFFER: usize = 1024;
const RETRY_FIRST: Duration = Duration::from_secs(5);
const RETRY_MAX: Duration = Duration::from_secs(60);
const KEEPALIVE_IDLE_S: i32 = 10;
const KEEPALIVE_INTERVAL_S: i32 = 5;
const KEEPALIVE_COUNT: i32 = 3;
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

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

    /// Stores the mode (temporary file + rename) and applies it only if that worked, so the
    /// panel never runs a mode it would forget on the next boot.
    pub fn set_mode(&self, mode: Mode) {
        if self.mode() == mode {
            return;
        }
        let temporary = format!("{MODE_FILE}.new");
        let stored = fs::write(&temporary, format!("{}\n", mode.id())).and_then(|_| fs::rename(&temporary, MODE_FILE));
        if let Err(error) = stored {
            eprintln!("wallview-agent: radio: cannot store mode, keeping {}: {error}", self.mode().label());
            self.changes.fetch_add(1, Ordering::SeqCst);
            return;
        }
        *self.mode.lock().unwrap() = mode;
        self.changes.fetch_add(1, Ordering::SeqCst);
    }

    /// True while a host (ZHA, Zigbee2MQTT) is connected to the radio.
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

/// Runs the radio for the lifetime of the agent, following the selected mode. A failing start
/// (UART missing or busy, port taken) is retried with a growing delay and logged once.
pub fn run(radio: Radio, node_name: String, friendly_name: String, serial_number: String) {
    let mut retry = RETRY_FIRST;
    let mut last_error = String::new();
    loop {
        match radio.mode() {
            Mode::Off => {
                retry = RETRY_FIRST;
                last_error.clear();
                thread::sleep(IDLE);
            }
            Mode::ZigbeeCoordinator => match bridge(&radio, &node_name, &friendly_name, &serial_number) {
                Ok(()) => {
                    retry = RETRY_FIRST;
                    last_error.clear();
                }
                Err(error) => {
                    let message = error.to_string();
                    if message != last_error {
                        eprintln!("wallview-agent: radio: {message} (retrying)");
                        last_error = message;
                    }
                    let deadline = Instant::now() + retry;
                    while Instant::now() < deadline && radio.mode() == Mode::ZigbeeCoordinator {
                        thread::sleep(IDLE);
                    }
                    retry = (retry * 2).min(RETRY_MAX);
                }
            },
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

fn check(result: libc::c_int) -> io::Result<()> {
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// Opens the UART exclusively (`TIOCEXCL`, so nothing else can open it while the bridge runs)
/// and raw at 115200 8N1 without hardware flow control. The factory EZSP firmware uses
/// XON/XOFF: `IXON` lets the kernel pause sending when the radio asks for it (ASH escapes 0x11
/// and 0x13 inside frames, so only real flow-control bytes are consumed). Reads return after
/// 0.2 s without data, so the copy loops can notice a mode change.
fn open_serial() -> io::Result<File> {
    let path = std::env::var(SERIAL_ENV).unwrap_or_else(|_| SERIAL.to_string());
    let file = OpenOptions::new().read(true).write(true).custom_flags(libc::O_NOCTTY | libc::O_NONBLOCK).open(&path)?;
    let fd = file.as_raw_fd();
    unsafe {
        check(libc::ioctl(fd, libc::TIOCEXCL))?;
        let mut termios: libc::termios = std::mem::zeroed();
        check(libc::tcgetattr(fd, &mut termios))?;
        libc::cfmakeraw(&mut termios);
        termios.c_cflag &= !(libc::CRTSCTS | libc::CSTOPB | libc::PARENB);
        termios.c_cflag |= libc::CLOCAL | libc::CREAD;
        termios.c_iflag &= !(libc::IXOFF | libc::IXANY);
        termios.c_iflag |= libc::IXON;
        termios.c_cc[libc::VMIN] = 0;
        termios.c_cc[libc::VTIME] = 2;
        check(libc::cfsetispeed(&mut termios, libc::B115200))?;
        check(libc::cfsetospeed(&mut termios, libc::B115200))?;
        check(libc::tcsetattr(fd, libc::TCSANOW, &termios))?;
        let flags = libc::fcntl(fd, libc::F_GETFL);
        check(flags)?;
        check(libc::fcntl(fd, libc::F_SETFL, flags & !libc::O_NONBLOCK))?;
    }
    Ok(file)
}

/// Discards bytes the radio sent while no host was connected and resumes output in case an
/// XOFF from an earlier session was never followed by XON.
fn reset_serial(serial: &File) {
    unsafe {
        libc::tcflush(serial.as_raw_fd(), libc::TCIFLUSH);
        libc::tcflow(serial.as_raw_fd(), libc::TCOON);
    }
}

fn set_keepalive(stream: &TcpStream) -> io::Result<()> {
    let fd = stream.as_raw_fd();
    let set = |level, name, value: libc::c_int| unsafe {
        check(libc::setsockopt(
            fd,
            level,
            name,
            &value as *const libc::c_int as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        ))
    };
    set(libc::SOL_SOCKET, libc::SO_KEEPALIVE, 1)?;
    set(libc::IPPROTO_TCP, libc::TCP_KEEPIDLE, KEEPALIVE_IDLE_S)?;
    set(libc::IPPROTO_TCP, libc::TCP_KEEPINTVL, KEEPALIVE_INTERVAL_S)?;
    set(libc::IPPROTO_TCP, libc::TCP_KEEPCNT, KEEPALIVE_COUNT)
}

/// Serves the UART to one TCP host at a time while the mode is Zigbee coordinator. Errors of a
/// single connection are logged and do not stop the bridge.
fn bridge(radio: &Radio, node_name: &str, friendly_name: &str, serial_number: &str) -> io::Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", BRIDGE_PORT))?;
    listener.set_nonblocking(true)?;
    let serial = open_serial()?;
    let _advertisement = Advertisement::start(node_name, friendly_name, serial_number);
    eprintln!("wallview-agent: radio: Zigbee bridge on port {BRIDGE_PORT}");
    let mut session: Option<Session> = None;

    while radio.mode() == Mode::ZigbeeCoordinator {
        if session.as_ref().is_some_and(Session::finished) {
            session.take().unwrap().stop();
            eprintln!("wallview-agent: radio: Zigbee host disconnected");
        }
        match listener.accept() {
            Ok((stream, peer)) => {
                if session.is_some() {
                    eprintln!("wallview-agent: radio: refused {peer}, another Zigbee host is connected");
                    let _ = stream.shutdown(Shutdown::Both);
                    continue;
                }
                match Session::start(stream, &serial, radio.clone()) {
                    Ok(started) => {
                        eprintln!("wallview-agent: radio: Zigbee host {peer} connected");
                        session = Some(started);
                    }
                    Err(error) => eprintln!("wallview-agent: radio: {peer}: {error}"),
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => thread::sleep(IDLE),
            Err(error) => {
                eprintln!("wallview-agent: radio: accept: {error}");
                thread::sleep(IDLE);
            }
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
        stream.set_nonblocking(false)?;
        stream.set_nodelay(true)?;
        stream.set_read_timeout(Some(Duration::from_millis(200)))?;
        stream.set_write_timeout(Some(WRITE_TIMEOUT))?;
        set_keepalive(&stream)?;
        // Every clone before any thread starts, so a failure leaves no worker behind.
        let mut from_serial = serial.try_clone()?;
        let mut to_serial = serial.try_clone()?;
        let mut to_client = stream.try_clone()?;
        let mut from_client = stream.try_clone()?;
        reset_serial(serial);
        let stop = Arc::new(AtomicBool::new(false));

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
