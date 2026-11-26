use std::fs;
use std::path::Path;
use std::process::Command;
use std::thread;
use std::time::{Duration, Instant};

pub const DATA_CONFIG: &str = "/data/wallview/wpa_supplicant.conf";
const BUILTIN_CONFIG: &str = "/etc/wpa_supplicant.conf";
const NETWORK_SCRIPT: &str = "/etc/init.d/S40wallview-network";

pub fn configured() -> bool {
    Path::new(DATA_CONFIG).exists() || Path::new(BUILTIN_CONFIG).exists()
}

pub fn address() -> Option<String> {
    let output = Command::new("ip").args(["-4", "-o", "addr", "show", "wlan0"]).output().ok()?;
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .skip_while(|word| *word != "inet")
        .nth(1)
        .and_then(|address| address.split('/').next())
        .map(str::to_string)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn network_block(ssid: &[u8], password: &[u8]) -> String {
    if password.is_empty() {
        format!("network={{\n\tssid={}\n\tkey_mgmt=NONE\n}}\n", hex(ssid))
    } else {
        let mut psk = [0u8; 32];
        pbkdf2::pbkdf2_hmac::<sha1::Sha1>(password, ssid, 4096, &mut psk);
        format!("network={{\n\tssid={}\n\tpsk={}\n}}\n", hex(ssid), hex(&psk))
    }
}

pub fn apply(ssid: &[u8], password: &[u8], timeout: Duration) -> Option<String> {
    let config = format!("ctrl_interface=/run/wpa_supplicant\n{}", network_block(ssid, password));
    let temporary = format!("{DATA_CONFIG}.new");
    fs::write(&temporary, config).ok()?;
    fs::rename(&temporary, DATA_CONFIG).ok()?;
    let _ = Command::new(NETWORK_SCRIPT).arg("restart").status();
    let start = Instant::now();
    while start.elapsed() < timeout {
        if let Some(address) = address() {
            return Some(address);
        }
        thread::sleep(Duration::from_millis(500));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn psk_matches_wpa_passphrase() {
        let block = network_block(b"IEEE", b"password");
        assert!(block.contains("psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    }
}
