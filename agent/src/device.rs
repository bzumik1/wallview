use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const BACKLIGHT: &str = "/sys/class/backlight/backlight";
const URL_FILE: &str = "/data/wallview/url";
const DEFAULT_URL: &str = "http://homeassistant.local:8123";
const BL_POWER_ON: &str = "0";
const BL_POWER_OFF: &str = "4";
const PROXIMITY_NEAR: f64 = 100.0;

#[derive(Clone)]
pub struct Device {
    pub id: String,
    pub name: String,
    pub mac: String,
    light_sensor: Option<PathBuf>,
}

fn read(path: impl AsRef<Path>) -> Option<String> {
    fs::read_to_string(path).ok().map(|text| text.trim().to_string())
}

fn read_number(path: impl AsRef<Path>) -> Option<f64> {
    read(path)?.parse().ok()
}

impl Device {
    pub fn detect() -> Self {
        let mac = read("/sys/class/net/wlan0/address").unwrap_or_default().to_uppercase();
        let compact = mac.replace(':', "").to_lowercase();
        let id = if compact.is_empty() { "unknown".to_string() } else { compact };
        let suffix = id[id.len().saturating_sub(4)..].to_uppercase();
        let light_sensor = fs::read_dir("/sys/bus/iio/devices")
            .ok()
            .into_iter()
            .flatten()
            .flatten()
            .map(|entry| entry.path())
            .find(|path| read(path.join("name")).as_deref() == Some("stk3310"));
        Device {
            name: format!("WallView {suffix}"),
            id,
            mac,
            light_sensor,
        }
    }

    pub fn node_name(&self) -> String {
        format!("wallview-{}", &self.id[self.id.len().saturating_sub(6)..])
    }

    pub fn proximity_near(&self) -> Option<bool> {
        Some(self.proximity()? >= PROXIMITY_NEAR)
    }

    pub fn free_memory_mb(&self) -> Option<f64> {
        let meminfo = read("/proc/meminfo")?;
        let line = meminfo.lines().find(|line| line.starts_with("MemAvailable:"))?;
        let kilobytes: f64 = line.split_whitespace().nth(1)?.parse().ok()?;
        Some(kilobytes / 1024.0)
    }

    pub fn uptime_s(&self) -> Option<f64> {
        read("/proc/uptime")?.split_whitespace().next()?.parse().ok()
    }

    pub fn wifi_signal_dbm(&self) -> Option<f64> {
        let output = Command::new("iw").args(["dev", "wlan0", "link"]).output().ok()?;
        let text = String::from_utf8_lossy(&output.stdout);
        let line = text.lines().find(|line| line.trim_start().starts_with("signal:"))?;
        line.split_whitespace().nth(1)?.parse().ok()
    }

    pub fn ip_address(&self) -> String {
        let output = Command::new("ip").args(["-4", "-o", "addr", "show", "wlan0"]).output();
        output
            .ok()
            .and_then(|output| {
                String::from_utf8_lossy(&output.stdout)
                    .split_whitespace()
                    .skip_while(|word| *word != "inet")
                    .nth(1)
                    .map(|address| address.split('/').next().unwrap_or("").to_string())
            })
            .unwrap_or_default()
    }

    pub fn restart_kiosk(&self) {
        let _ = Command::new("killall").arg("cog").status();
    }

    pub fn reboot(&self) {
        let _ = Command::new("reboot").status();
    }

    pub fn max_brightness(&self) -> u32 {
        read_number(Path::new(BACKLIGHT).join("max_brightness")).unwrap_or(255.0) as u32
    }

    pub fn brightness(&self) -> u32 {
        read_number(Path::new(BACKLIGHT).join("brightness")).unwrap_or(0.0) as u32
    }

    pub fn backlight_on(&self) -> bool {
        read(Path::new(BACKLIGHT).join("bl_power")).as_deref() != Some(BL_POWER_OFF)
    }

    pub fn set_brightness(&self, value: u32) {
        let value = value.clamp(1, self.max_brightness());
        let _ = fs::write(Path::new(BACKLIGHT).join("brightness"), value.to_string());
    }

    pub fn set_backlight(&self, on: bool) {
        let value = if on { BL_POWER_ON } else { BL_POWER_OFF };
        let _ = fs::write(Path::new(BACKLIGHT).join("bl_power"), value);
    }

    pub fn illuminance(&self) -> Option<f64> {
        let sensor = self.light_sensor.as_ref()?;
        Some(read_number(sensor.join("in_illuminance_raw"))? * read_number(sensor.join("in_illuminance_scale"))?)
    }

    pub fn proximity(&self) -> Option<f64> {
        read_number(self.light_sensor.as_ref()?.join("in_proximity_raw"))
    }

    pub fn cpu_temperature(&self) -> Option<f64> {
        Some(read_number("/sys/class/thermal/thermal_zone0/temp")? / 1000.0)
    }

    pub fn url(&self) -> String {
        read(URL_FILE).filter(|url| !url.is_empty()).unwrap_or_else(|| DEFAULT_URL.to_string())
    }

    pub fn set_url(&self, url: &str) {
        if !(url.starts_with("http://") || url.starts_with("https://")) {
            return;
        }
        if fs::write(URL_FILE, format!("{url}\n")).is_ok() {
            let _ = Command::new("killall").arg("cog").status();
        }
    }
}
