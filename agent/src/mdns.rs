use std::sync::OnceLock;

use mdns_sd::{IfKind, ServiceDaemon};

const SERVICE_NAME_LEN_MAX: u8 = 20;

pub fn daemon() -> Option<&'static ServiceDaemon> {
    static DAEMON: OnceLock<Option<ServiceDaemon>> = OnceLock::new();
    DAEMON
        .get_or_init(|| {
            let daemon = ServiceDaemon::new().map_err(|error| eprintln!("wallview-agent: mdns: {error}")).ok()?;
            let _ = daemon.disable_interface(IfKind::Name("usb0".into()));
            if let Err(error) = daemon.set_service_name_len_max(SERVICE_NAME_LEN_MAX) {
                eprintln!("wallview-agent: mdns: {error}");
            }
            Some(daemon)
        })
        .as_ref()
}
