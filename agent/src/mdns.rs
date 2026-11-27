use std::sync::OnceLock;

use mdns_sd::{IfKind, ServiceDaemon};

const SERVICE_NAME_LEN_MAX: u8 = 20;

/// One mDNS responder for every service the agent advertises (ESPHome API, Zigbee bridge).
pub fn daemon() -> Option<&'static ServiceDaemon> {
    static DAEMON: OnceLock<Option<ServiceDaemon>> = OnceLock::new();
    DAEMON
        .get_or_init(|| {
            let daemon = ServiceDaemon::new().map_err(|error| eprintln!("wallview-agent: mdns: {error}")).ok()?;
            let _ = daemon.disable_interface(IfKind::Name("usb0".into()));
            // `_zigbee-coordinator` (ZHA) is longer than the RFC 6763 limit of 15 characters.
            if let Err(error) = daemon.set_service_name_len_max(SERVICE_NAME_LEN_MAX) {
                eprintln!("wallview-agent: mdns: {error}");
            }
            Some(daemon)
        })
        .as_ref()
}
