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
const USER_TIMEOUT_MS: i32 = 25_000;
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
    store: Arc<Mutex<()>>,
    client: Arc<AtomicBool>,
    changes: Arc<AtomicU64>,
}

impl Radio {
    pub fn load() -> Self {
        let mode = fs::read_to_string(MODE_FILE)
            .ok()
            .and_then(|text| Mode::from_id(text.trim()))
            .unwrap_or(Mode::Off);
        Radio { mode: Arc::new(Mutex::new(mode)), store: Arc::new(Mutex::new(())), client: Arc::new(AtomicBool::new(false)), changes: Arc::new(AtomicU64::new(0)) }
    }

    pub fn mode(&self) -> Mode {
        *self.mode.lock().unwrap()
    }

    pub fn set_mode(&self, mode: Mode) {
        let _store = self.store.lock().unwrap();
        if self.mode() == mode {
            return;
        }
        let temporary = format!("{MODE_FILE}.new");
        let stored = File::create(&temporary)
            .and_then(|mut file| {
                file.write_all(format!("{}\n", mode.id()).as_bytes())?;
                file.sync_all()
            })
            .and_then(|_| fs::rename(&temporary, MODE_FILE));
        if let Err(error) = stored {
            eprintln!("wallview-agent: radio: cannot store mode, keeping {}: {error}", self.mode().label());
            self.changes.fetch_add(1, Ordering::SeqCst);
            return;
        }
        *self.mode.lock().unwrap() = mode;
        self.changes.fetch_add(1, Ordering::SeqCst);
    }

    pub fn client_connected(&self) -> bool {
        self.client.load(Ordering::SeqCst)
    }

    pub fn changes(&self) -> u64 {
        self.changes.load(Ordering::SeqCst)
    }

    fn set_client(&self, connected: bool) {
        if self.client.swap(connected, Ordering::SeqCst) != connected {
            self.changes.fetch_add(1, Ordering::SeqCst);
        }
    }
}

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
        termios.c_iflag &= !(libc::IXON | libc::IXOFF | libc::IXANY);
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

fn flush_serial(serial: &File) {
    unsafe {
        libc::tcflush(serial.as_raw_fd(), libc::TCIOFLUSH);
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
    set(libc::IPPROTO_TCP, libc::TCP_KEEPCNT, KEEPALIVE_COUNT)?;
    set(libc::IPPROTO_TCP, libc::TCP_USER_TIMEOUT, USER_TIMEOUT_MS)
}

fn bridge(radio: &Radio, node_name: &str, friendly_name: &str, serial_number: &str) -> io::Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", BRIDGE_PORT))?;
    listener.set_nonblocking(true)?;
    let serial = open_serial()?;
    let _advertisement = Advertisement::start(node_name, friendly_name, serial_number);
    eprintln!("wallview-agent: radio: Zigbee bridge on port {BRIDGE_PORT}");
    let mut session: Option<Session> = None;
    let mut refused_logged: Vec<std::net::IpAddr> = Vec::new();
    let reap = |session: &mut Option<Session>, refused_logged: &mut Vec<std::net::IpAddr>| {
        if session.as_ref().is_some_and(Session::finished) {
            session.take().unwrap().stop(&serial);
            refused_logged.clear();
            eprintln!("wallview-agent: radio: Zigbee host disconnected");
        }
    };

    while radio.mode() == Mode::ZigbeeCoordinator {
        reap(&mut session, &mut refused_logged);
        match listener.accept() {
            Ok((stream, peer)) => {
                reap(&mut session, &mut refused_logged);
                if session.is_some() {
                    if !refused_logged.contains(&peer.ip()) && refused_logged.len() < 16 {
                        eprintln!("wallview-agent: radio: refusing {}, another Zigbee host is connected", peer.ip());
                        refused_logged.push(peer.ip());
                    }
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
        session.stop(&serial);
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
        let mut from_serial = serial.try_clone()?;
        let mut to_serial = serial.try_clone()?;
        let mut to_client = stream.try_clone()?;
        let mut from_client = stream.try_clone()?;
        flush_serial(serial);
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

    fn stop(self, serial: &File) {
        self.stop.store(true, Ordering::SeqCst);
        let _ = self.stream.shutdown(Shutdown::Both);
        unsafe {
            libc::tcflush(serial.as_raw_fd(), libc::TCOFLUSH);
        }
        for worker in self.workers {
            let _ = worker.join();
        }
        self.radio.set_client(false);
    }
}
