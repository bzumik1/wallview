mod api;
mod config;
mod device;
mod discovery;
mod esphome;
mod improv;
mod mqtt;
mod mqtt_discovery;
mod wifi;

use std::path::Path;
use std::thread;
use std::time::Duration;

use config::Config;
use device::Device;

const MQTT_CONFIG: &str = "/data/wallview/mqtt.conf";
const API_KEY: &str = "/data/wallview/api.key";
const IMPROV_GRACE: Duration = Duration::from_secs(120);

fn start_improv_when_needed(device: Device) {
    thread::spawn(move || {
        if wifi::configured() {
            thread::sleep(IMPROV_GRACE);
            if wifi::address().is_some() {
                return;
            }
        }
        improv::run(device);
    });
}

fn main() {
    let device = Device::detect();

    if Path::new(MQTT_CONFIG).exists() {
        match Config::load(MQTT_CONFIG) {
            Ok(config) => {
                let mqtt_device = device.clone();
                thread::spawn(move || mqtt::run(config, mqtt_device));
            }
            Err(error) => eprintln!("wallview-agent: {error}"),
        }
    }

    start_improv_when_needed(device.clone());
    let discovery_device = device.clone();
    thread::spawn(move || discovery::run(discovery_device));
    esphome::serve(device, esphome::KeyStore::load(API_KEY));
}
