use std::thread;
use std::time::{Duration, Instant};

use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};

use crate::device::Device;

const SERVICE_TYPE: &str = "_home-assistant._tcp.local.";
const BROWSE_TIME: Duration = Duration::from_secs(10);
const RETRY_INTERVAL: Duration = Duration::from_secs(60);

fn url_of(info: &ServiceInfo) -> Option<String> {
    let internal = info
        .get_property_val_str("internal_url")
        .filter(|url| url.starts_with("http://") || url.starts_with("https://"))
        .map(str::to_string);
    internal.or_else(|| {
        let address = info.get_addresses_v4().into_iter().next()?;
        Some(format!("http://{address}:{}", info.get_port()))
    })
}

fn find_home_assistant() -> Option<String> {
    let daemon = ServiceDaemon::new().ok()?;
    let receiver = daemon.browse(SERVICE_TYPE).ok()?;
    let deadline = Instant::now() + BROWSE_TIME;
    let mut found = None;
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        match receiver.recv_timeout(remaining) {
            Ok(ServiceEvent::ServiceResolved(info)) => {
                found = url_of(&info);
                if found.is_some() {
                    break;
                }
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }
    let _ = daemon.shutdown();
    found
}

pub fn run(device: Device) {
    while !device.url_configured() {
        if let Some(url) = find_home_assistant() {
            device.set_url(&url);
            return;
        }
        thread::sleep(RETRY_INTERVAL);
    }
}
