use mdns_sd::{IfKind, ServiceDaemon, ServiceInfo};

use crate::device::Device;

use super::{ESPHOME_VERSION, PORT};

const SERVICE_TYPE: &str = "_esphomelib._tcp.local.";
const NOISE: &str = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

pub struct Advertisement {
    daemon: ServiceDaemon,
    fullname: String,
}

impl Advertisement {
    pub fn start(device: &Device, has_key: bool) -> Option<Self> {
        let daemon = ServiceDaemon::new()
            .map_err(|error| eprintln!("wallview-agent: mdns: {error}"))
            .ok()?;
        let _ = daemon.disable_interface(IfKind::Name("usb0".into()));
        let node = device.node_name();
        let mut properties = vec![
            ("version", ESPHOME_VERSION.to_string()),
            ("mac", device.id.clone()),
            ("friendly_name", device.name.clone()),
            ("platform", "HOST".to_string()),
            ("board", "nspanel-pro".to_string()),
            ("network", "wifi".to_string()),
            ("project_name", "WallView.WallView OS".to_string()),
            ("project_version", env!("CARGO_PKG_VERSION").to_string()),
        ];
        if has_key {
            properties.push(("api_encryption", NOISE.to_string()));
        } else {
            properties.push(("api_encryption_supported", NOISE.to_string()));
            properties.push(("api_provisioning", "zero-psk".to_string()));
        }
        let service = ServiceInfo::new(SERVICE_TYPE, &node, &format!("{node}.local."), "", PORT, &properties[..])
            .map_err(|error| eprintln!("wallview-agent: mdns: {error}"))
            .ok()?
            .enable_addr_auto();
        let fullname = service.get_fullname().to_string();
        daemon
            .register(service)
            .map_err(|error| eprintln!("wallview-agent: mdns: {error}"))
            .ok()?;
        Some(Advertisement { daemon, fullname })
    }
}

impl Drop for Advertisement {
    fn drop(&mut self) {
        let _ = self.daemon.unregister(&self.fullname);
        let _ = self.daemon.shutdown();
    }
}
