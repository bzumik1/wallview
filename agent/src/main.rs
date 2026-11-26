mod api;
mod config;
mod device;
mod esphome;
mod mqtt;
mod mqtt_discovery;

use std::path::Path;
use std::thread;

use config::Config;
use device::Device;

const MQTT_CONFIG: &str = "/data/wallview/mqtt.conf";
const API_KEY: &str = "/data/wallview/api.key";

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

    esphome::serve(device, esphome::KeyStore::load(API_KEY));
}
