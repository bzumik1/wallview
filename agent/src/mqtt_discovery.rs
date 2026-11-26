use serde_json::{json, Value};

use crate::device::Device;

const DISCOVERY_PREFIX: &str = "homeassistant";

#[derive(Clone)]
pub struct Topics {
    pub availability: String,
    pub backlight_state: String,
    pub backlight_set: String,
    pub sensors_state: String,
    pub restart_press: String,
    pub url_state: String,
    pub url_set: String,
}

impl Topics {
    pub fn new(id: &str) -> Self {
        let base = format!("wallview/{id}");
        Topics {
            availability: format!("{base}/availability"),
            backlight_state: format!("{base}/backlight/state"),
            backlight_set: format!("{base}/backlight/set"),
            sensors_state: format!("{base}/sensors"),
            restart_press: format!("{base}/restart/press"),
            url_state: format!("{base}/url/state"),
            url_set: format!("{base}/url/set"),
        }
    }
}

pub fn configs(device: &Device, topics: &Topics) -> Vec<(String, Value)> {
    let device_info = json!({
        "identifiers": [format!("wallview_{}", device.id)],
        "name": device.name,
        "manufacturer": "WallView",
        "model": "Sonoff NSPanel Pro",
        "sw_version": env!("CARGO_PKG_VERSION"),
    });
    let entity = |component: &str, key: &str, mut config: Value| {
        config["unique_id"] = json!(format!("wallview_{}_{}", device.id, key));
        config["object_id"] = json!(format!("wallview_{}_{}", device.id, key));
        config["availability_topic"] = json!(topics.availability);
        config["device"] = device_info.clone();
        (format!("{DISCOVERY_PREFIX}/{component}/wallview_{}/{key}/config", device.id), config)
    };
    let sensor = |key: &str, name: &str, extra: Value| {
        let mut config = json!({
            "name": name,
            "state_topic": topics.sensors_state,
            "value_template": format!("{{{{ value_json.{key} }}}}"),
            "state_class": "measurement",
        });
        if let (Some(target), Some(source)) = (config.as_object_mut(), extra.as_object()) {
            target.extend(source.clone());
        }
        entity("sensor", key, config)
    };
    vec![
        entity(
            "light",
            "backlight",
            json!({
                "name": "Display",
                "schema": "json",
                "state_topic": topics.backlight_state,
                "command_topic": topics.backlight_set,
                "brightness": true,
                "brightness_scale": device.max_brightness(),
                "supported_color_modes": ["brightness"],
            }),
        ),
        sensor("illuminance", "Illuminance", json!({"device_class": "illuminance", "unit_of_measurement": "lx"})),
        sensor("proximity", "Proximity", json!({"icon": "mdi:hand-wave"})),
        sensor(
            "cpu_temperature",
            "CPU temperature",
            json!({"device_class": "temperature", "unit_of_measurement": "°C", "entity_category": "diagnostic"}),
        ),
        entity(
            "button",
            "restart",
            json!({
                "name": "Restart",
                "device_class": "restart",
                "command_topic": topics.restart_press,
                "entity_category": "config",
            }),
        ),
        entity(
            "text",
            "url",
            json!({
                "name": "Dashboard URL",
                "state_topic": topics.url_state,
                "command_topic": topics.url_set,
                "entity_category": "config",
                "max": 255,
            }),
        ),
    ]
}
