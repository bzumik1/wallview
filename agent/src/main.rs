mod config;
mod device;
mod discovery;

use std::process::Command;
use std::thread;
use std::time::Duration;

use rumqttc::{Client, Event, LastWill, MqttOptions, Packet, QoS};
use serde_json::{json, Value};

use config::Config;
use device::Device;
use discovery::Topics;

const SENSOR_INTERVAL: Duration = Duration::from_secs(15);
const CONFIG_PATH: &str = "/data/wallview/mqtt.conf";

fn main() {
    let config = match Config::load(CONFIG_PATH) {
        Ok(config) => config,
        Err(error) => {
            eprintln!("wallview-agent: {error}");
            std::process::exit(1);
        }
    };
    let device = Device::detect();
    let topics = Topics::new(&device.id);

    let mut options = MqttOptions::new(format!("wallview-{}", device.id), &config.host, config.port);
    options.set_keep_alive(Duration::from_secs(30));
    if let (Some(username), Some(password)) = (&config.username, &config.password) {
        options.set_credentials(username, password);
    }
    options.set_last_will(LastWill::new(&topics.availability, "offline", QoS::AtLeastOnce, true));

    let (client, mut connection) = Client::new(options, 64);

    let publisher = client.clone();
    let sensor_device = device.clone();
    let sensor_topics = topics.clone();
    thread::spawn(move || loop {
        publish_sensors(&publisher, &sensor_device, &sensor_topics);
        thread::sleep(SENSOR_INTERVAL);
    });

    for notification in connection.iter() {
        match notification {
            Ok(Event::Incoming(Packet::ConnAck(_))) => announce(&client, &device, &topics),
            Ok(Event::Incoming(Packet::Publish(message))) => {
                let payload = String::from_utf8_lossy(&message.payload).to_string();
                handle_command(&client, &device, &topics, &message.topic, &payload);
            }
            Ok(_) => {}
            Err(error) => {
                eprintln!("wallview-agent: connection: {error}");
                thread::sleep(Duration::from_secs(5));
            }
        }
    }
}

fn publish(client: &Client, topic: &str, payload: String, retain: bool) {
    if let Err(error) = client.try_publish(topic, QoS::AtLeastOnce, retain, payload) {
        eprintln!("wallview-agent: publish {topic}: {error}");
    }
}

fn announce(client: &Client, device: &Device, topics: &Topics) {
    for (topic, payload) in discovery::configs(device, topics) {
        publish(client, &topic, payload.to_string(), true);
    }
    for topic in [&topics.backlight_set, &topics.restart_press, &topics.url_set] {
        if let Err(error) = client.try_subscribe(topic.as_str(), QoS::AtLeastOnce) {
            eprintln!("wallview-agent: subscribe {topic}: {error}");
        }
    }
    publish(client, &topics.availability, "online".into(), true);
    publish_backlight(client, device, topics);
    publish(client, &topics.url_state, device.url(), true);
    publish_sensors(client, device, topics);
}

fn publish_backlight(client: &Client, device: &Device, topics: &Topics) {
    let brightness = device.brightness();
    let state = json!({
        "state": if device.backlight_on() { "ON" } else { "OFF" },
        "brightness": brightness,
    });
    publish(client, &topics.backlight_state, state.to_string(), true);
}

fn publish_sensors(client: &Client, device: &Device, topics: &Topics) {
    let mut state = serde_json::Map::new();
    if let Some(lux) = device.illuminance() {
        state.insert("illuminance".into(), json!((lux * 10.0).round() / 10.0));
    }
    if let Some(proximity) = device.proximity() {
        state.insert("proximity".into(), json!(proximity));
    }
    if let Some(temperature) = device.cpu_temperature() {
        state.insert("cpu_temperature".into(), json!((temperature * 10.0).round() / 10.0));
    }
    publish(client, &topics.sensors_state, Value::Object(state).to_string(), false);
}

fn handle_command(client: &Client, device: &Device, topics: &Topics, topic: &str, payload: &str) {
    if topic == topics.backlight_set {
        if let Ok(command) = serde_json::from_str::<Value>(payload) {
            if let Some(brightness) = command.get("brightness").and_then(Value::as_u64) {
                device.set_brightness(brightness as u32);
            }
            match command.get("state").and_then(Value::as_str) {
                Some("ON") => device.set_backlight(true),
                Some("OFF") => device.set_backlight(false),
                _ => {}
            }
        }
        publish_backlight(client, device, topics);
    } else if topic == topics.restart_press {
        publish(client, &topics.availability, "offline".into(), true);
        let _ = Command::new("reboot").status();
    } else if topic == topics.url_set {
        device.set_url(payload.trim());
        publish(client, &topics.url_state, device.url(), true);
    }
}
