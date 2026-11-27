mod mgmt;
mod rfkill;

use std::fs::{self, File};
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use bluer::monitor::{data_type, Monitor, Pattern, Type};
use bluer::{Adapter, DiscoveryFilter, DiscoveryTransport, Session};
use futures::StreamExt;
use tokio::sync::Notify;
use tokio::task::JoinHandle;

pub use mgmt::Advertisement;

const PROXY_FILE: &str = "/data/wallview/bluetooth-proxy";
const SUBSCRIBER_QUEUE: usize = 256;
const HOLD_WAIT: Duration = Duration::from_secs(20);
const HOLD_POLL: Duration = Duration::from_millis(200);
const TICK: Duration = Duration::from_secs(5);
const POWER_ATTEMPTS: u32 = 10;
const POWER_RETRY: Duration = Duration::from_millis(500);
const MONITOR_TIMEOUT: Duration = Duration::from_secs(10);
const CONTROL_READ_TIMEOUT: Duration = Duration::from_secs(1);
const CONTROL_RETRY: Duration = Duration::from_secs(5);
const DUPLICATE_WINDOW: Duration = Duration::from_millis(5);
const PACKET: usize = 1024;

#[derive(Default)]
struct State {
    proxy: bool,
    holds: usize,
    powered: bool,
    address: Option<String>,
    subscribers: Vec<(u64, SyncSender<Advertisement>)>,
}

struct Shared {
    state: Mutex<State>,
    store: Mutex<()>,
    wake: Notify,
    changes: AtomicU64,
    next_subscriber: AtomicU64,
}

#[derive(Clone)]
pub struct Bluetooth {
    shared: Arc<Shared>,
}

pub struct Hold {
    shared: Arc<Shared>,
}

impl Drop for Hold {
    fn drop(&mut self) {
        self.shared.state.lock().unwrap().holds -= 1;
        self.shared.wake.notify_one();
    }
}

pub struct Subscription {
    id: u64,
    shared: Arc<Shared>,
    receiver: Receiver<Advertisement>,
}

impl Subscription {
    pub fn pending(&self) -> Vec<Advertisement> {
        self.receiver.try_iter().collect()
    }
}

impl Drop for Subscription {
    fn drop(&mut self) {
        self.shared.state.lock().unwrap().subscribers.retain(|(id, _)| *id != self.id);
        self.shared.wake.notify_one();
    }
}

impl Bluetooth {
    pub fn load() -> Self {
        let proxy = fs::read_to_string(PROXY_FILE).is_ok_and(|text| text.trim() == "on");
        Bluetooth {
            shared: Arc::new(Shared {
                state: Mutex::new(State { proxy, ..Default::default() }),
                store: Mutex::new(()),
                wake: Notify::new(),
                changes: AtomicU64::new(0),
                next_subscriber: AtomicU64::new(0),
            }),
        }
    }

    pub fn proxy_enabled(&self) -> bool {
        self.shared.state.lock().unwrap().proxy
    }

    pub fn address(&self) -> Option<String> {
        self.shared.state.lock().unwrap().address.clone()
    }

    pub fn changes(&self) -> u64 {
        self.shared.changes.load(Ordering::SeqCst)
    }

    pub fn set_proxy(&self, enabled: bool) {
        let _store = self.shared.store.lock().unwrap();
        if self.proxy_enabled() == enabled {
            return;
        }
        let temporary = format!("{PROXY_FILE}.new");
        let stored = File::create(&temporary)
            .and_then(|mut file| {
                file.write_all(if enabled { b"on\n" } else { b"off\n" })?;
                file.sync_all()
            })
            .and_then(|_| fs::rename(&temporary, PROXY_FILE));
        match stored {
            Ok(()) => self.shared.state.lock().unwrap().proxy = enabled,
            Err(error) => eprintln!("wallview-agent: bluetooth: cannot store the proxy setting: {error}"),
        }
        self.shared.changes.fetch_add(1, Ordering::SeqCst);
        self.shared.wake.notify_one();
    }

    pub fn hold(&self) -> Hold {
        self.shared.state.lock().unwrap().holds += 1;
        self.shared.wake.notify_one();
        let started = Instant::now();
        while !self.shared.state.lock().unwrap().powered && started.elapsed() < HOLD_WAIT {
            thread::sleep(HOLD_POLL);
        }
        Hold { shared: self.shared.clone() }
    }

    pub fn subscribe(&self) -> Subscription {
        let id = self.shared.next_subscriber.fetch_add(1, Ordering::SeqCst);
        let (sender, receiver) = mpsc::sync_channel(SUBSCRIBER_QUEUE);
        self.shared.state.lock().unwrap().subscribers.push((id, sender));
        self.shared.wake.notify_one();
        Subscription { id, shared: self.shared.clone(), receiver }
    }

    fn wanted(&self) -> (bool, bool) {
        let state = self.shared.state.lock().unwrap();
        (state.holds > 0 || state.proxy, state.proxy && !state.subscribers.is_empty())
    }

    fn set_powered_state(&self, powered: bool, address: Option<String>) {
        let mut state = self.shared.state.lock().unwrap();
        state.powered = powered;
        if address.is_some() {
            state.address = address;
        }
    }

    fn dispatch(&self, advertisement: &Advertisement) {
        let mut state = self.shared.state.lock().unwrap();
        if !state.proxy {
            return;
        }
        state.subscribers.retain(|(_, sender)| !matches!(sender.try_send(advertisement.clone()), Err(TrySendError::Disconnected(_))));
    }
}

fn report(last: &mut String, message: String) {
    if *last != message {
        eprintln!("wallview-agent: bluetooth: {message}");
        *last = message;
    }
}

fn read_control(bluetooth: Bluetooth) {
    let mut last_error = String::new();
    let mut buffer = [0u8; PACKET];
    loop {
        let control = match mgmt::Control::open(CONTROL_READ_TIMEOUT) {
            Ok(control) => control,
            Err(error) => {
                report(&mut last_error, format!("cannot open the management socket: {error}"));
                thread::sleep(CONTROL_RETRY);
                continue;
            }
        };
        let mut previous: Option<(Advertisement, Instant)> = None;
        loop {
            match control.receive(&mut buffer) {
                Ok(Some(length)) => {
                    let Some(advertisement) = mgmt::parse(&buffer[..length]) else {
                        continue;
                    };
                    let duplicate = previous
                        .as_ref()
                        .is_some_and(|(last, at)| *last == advertisement && at.elapsed() < DUPLICATE_WINDOW);
                    if !duplicate {
                        bluetooth.dispatch(&advertisement);
                    }
                    previous = Some((advertisement, Instant::now()));
                }
                Ok(None) => {}
                Err(error) => {
                    report(&mut last_error, format!("management socket: {error}"));
                    thread::sleep(CONTROL_RETRY);
                    break;
                }
            }
        }
    }
}

fn passive_monitor() -> Monitor {
    Monitor {
        monitor_type: Type::OrPatterns,
        patterns: Some(
            [0x02u8, 0x06, 0x1a].iter().map(|flags| Pattern::new(data_type::FLAGS, 0, &[*flags])).collect(),
        ),
        ..Default::default()
    }
}

async fn start_monitor(adapter: &Adapter) -> bluer::Result<JoinHandle<()>> {
    let manager = adapter.monitor().await?;
    let registered = tokio::time::timeout(MONITOR_TIMEOUT, manager.register(passive_monitor())).await;
    let mut handle = match registered {
        Ok(handle) => handle?,
        Err(_) => return Err(bluer::Error { kind: bluer::ErrorKind::Failed, message: "monitor was not activated".into() }),
    };
    Ok(tokio::spawn(async move {
        let _manager = manager;
        while handle.next().await.is_some() {}
    }))
}

async fn start_discovery(adapter: &Adapter) -> bluer::Result<JoinHandle<()>> {
    adapter
        .set_discovery_filter(DiscoveryFilter {
            transport: DiscoveryTransport::Le,
            duplicate_data: true,
            ..Default::default()
        })
        .await?;
    let mut events = Box::pin(adapter.discover_devices().await?);
    Ok(tokio::spawn(async move { while events.next().await.is_some() {} }))
}

async fn start_scanner(adapter: &Adapter, last: &mut String) -> bluer::Result<JoinHandle<()>> {
    match start_monitor(adapter).await {
        Ok(task) => {
            report(last, "scanning passively with an advertisement monitor".into());
            Ok(task)
        }
        Err(monitor_error) => {
            let task = start_discovery(adapter).await?;
            report(last, format!("advertisement monitor unavailable ({monitor_error}), scanning with discovery"));
            Ok(task)
        }
    }
}

async fn power_on(session: &Session) -> bluer::Result<Adapter> {
    if let Err(error) = rfkill::set_blocked(false) {
        eprintln!("wallview-agent: bluetooth: cannot unblock: {error}");
    }
    let mut last = None;
    for _ in 0..POWER_ATTEMPTS {
        match session.default_adapter().await {
            Ok(adapter) => match adapter.set_powered(true).await {
                Ok(()) => return Ok(adapter),
                Err(error) => last = Some(error),
            },
            Err(error) => last = Some(error),
        }
        tokio::time::sleep(POWER_RETRY).await;
    }
    Err(last.unwrap_or_else(|| bluer::Error { kind: bluer::ErrorKind::NotReady, message: "no adapter".into() }))
}

async fn power_off(session: &Session) {
    if let Ok(adapter) = session.default_adapter().await {
        let _ = adapter.set_powered(false).await;
    }
    if let Err(error) = rfkill::set_blocked(true) {
        eprintln!("wallview-agent: bluetooth: cannot block: {error}");
    }
}

fn stop(scanner: &mut Option<JoinHandle<()>>) {
    if let Some(task) = scanner.take() {
        task.abort();
    }
}

async fn manage(bluetooth: Bluetooth) {
    let mut last = String::new();
    let session = loop {
        match Session::new().await {
            Ok(session) => break session,
            Err(error) => report(&mut last, format!("no bluetoothd: {error}")),
        }
        tokio::time::sleep(TICK).await;
    };
    let mut adapter: Option<Adapter> = None;
    let mut scanner: Option<JoinHandle<()>> = None;
    let mut first = true;
    loop {
        let (want_power, want_scan) = bluetooth.wanted();
        if !want_power && (adapter.is_some() || first) {
            stop(&mut scanner);
            adapter = None;
            power_off(&session).await;
            bluetooth.set_powered_state(false, None);
            report(&mut last, "off".into());
        }
        first = false;
        if want_power && adapter.is_none() {
            match power_on(&session).await {
                Ok(powered) => {
                    let address = powered.address().await.ok().map(|address| address.to_string());
                    bluetooth.set_powered_state(true, address);
                    adapter = Some(powered);
                    report(&mut last, "on".into());
                }
                Err(error) => report(&mut last, format!("cannot power on: {error}")),
            }
        }
        if scanner.as_ref().is_some_and(|task| task.is_finished()) || !want_scan {
            stop(&mut scanner);
        }
        if want_scan && scanner.is_none() {
            if let Some(adapter) = &adapter {
                match start_scanner(adapter, &mut last).await {
                    Ok(task) => scanner = Some(task),
                    Err(error) => report(&mut last, format!("cannot scan: {error}")),
                }
            }
        }
        let _ = tokio::time::timeout(TICK, bluetooth.shared.wake.notified()).await;
    }
}

pub fn run(bluetooth: Bluetooth) {
    let reader = bluetooth.clone();
    thread::spawn(move || read_control(reader));
    let runtime = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("wallview-agent: bluetooth: {error}");
            return;
        }
    };
    runtime.block_on(manage(bluetooth));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bluetooth(proxy: bool) -> Bluetooth {
        let bluetooth = Bluetooth::load();
        bluetooth.shared.state.lock().unwrap().proxy = proxy;
        bluetooth
    }

    fn advertisement() -> Advertisement {
        Advertisement { address: 1, address_type: 0, rssi: -50, data: vec![0x02, 0x01, 0x06] }
    }

    #[test]
    fn is_off_unless_held_or_proxied() {
        let bluetooth = bluetooth(false);
        assert_eq!(bluetooth.wanted(), (false, false));
        bluetooth.shared.state.lock().unwrap().powered = true;
        let hold = bluetooth.hold();
        assert_eq!(bluetooth.wanted(), (true, false));
        drop(hold);
        assert_eq!(bluetooth.wanted(), (false, false));
    }

    #[test]
    fn scans_only_with_proxy_and_a_subscriber() {
        let bluetooth = bluetooth(true);
        assert_eq!(bluetooth.wanted(), (true, false));
        let subscription = bluetooth.subscribe();
        assert_eq!(bluetooth.wanted(), (true, true));
        drop(subscription);
        assert_eq!(bluetooth.wanted(), (true, false));
    }

    #[test]
    fn forwards_advertisements_only_with_proxy() {
        let bluetooth = bluetooth(false);
        let subscription = bluetooth.subscribe();
        bluetooth.dispatch(&advertisement());
        assert!(subscription.pending().is_empty());
        bluetooth.shared.state.lock().unwrap().proxy = true;
        bluetooth.dispatch(&advertisement());
        assert_eq!(subscription.pending(), vec![advertisement()]);
    }

    #[test]
    fn drops_advertisements_when_a_subscriber_falls_behind() {
        let bluetooth = bluetooth(true);
        let subscription = bluetooth.subscribe();
        for _ in 0..SUBSCRIBER_QUEUE + 10 {
            bluetooth.dispatch(&advertisement());
        }
        assert_eq!(subscription.pending().len(), SUBSCRIBER_QUEUE);
        assert_eq!(bluetooth.shared.state.lock().unwrap().subscribers.len(), 1);
    }
}
