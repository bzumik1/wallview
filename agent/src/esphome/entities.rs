use prost::Message;

use crate::api::{
    BinarySensorStateResponse, ButtonCommandRequest, ColorMode, EntityCategory, ExecuteServiceRequest,
    LightCommandRequest, LightStateResponse, ListEntitiesBinarySensorResponse, ListEntitiesButtonResponse,
    ListEntitiesLightResponse, ListEntitiesNumberResponse, ListEntitiesSelectResponse, ListEntitiesSensorResponse, ListEntitiesServicesArgument, ListEntitiesSwitchResponse,
    ListEntitiesServicesResponse, ListEntitiesTextResponse, ListEntitiesTextSensorResponse, NumberCommandRequest, NumberMode, NumberStateResponse, SensorStateClass,
    SelectCommandRequest, SelectStateResponse, SensorStateResponse, SwitchCommandRequest, SwitchStateResponse, ServiceArgType, TextCommandRequest, TextMode, TextSensorStateResponse,
    TextStateResponse,
};
use crate::device::Device;
use crate::presence::{MAXIMUM_SENSITIVITY, MINIMUM_SENSITIVITY};
use crate::radio::Mode;

use super::message_type;

pub const DISPLAY: u32 = 1;
pub const ILLUMINANCE: u32 = 2;
pub const PRESENCE: u32 = 3;
pub const PROXIMITY_RAW: u32 = 4;
pub const CPU_TEMPERATURE: u32 = 5;
pub const FREE_MEMORY: u32 = 6;
pub const UPTIME: u32 = 7;
pub const WIFI_SIGNAL: u32 = 8;
pub const RESTART_PANEL: u32 = 9;
pub const RESTART_KIOSK: u32 = 10;
pub const DASHBOARD_URL: u32 = 11;
pub const IP_ADDRESS: u32 = 12;
pub const VERSION: u32 = 13;
pub const SIGN_IN_AGAIN: u32 = 14;
pub const PAIR: u32 = 15;
pub const RADIO_MODE: u32 = 16;
pub const ZIGBEE_CLIENT: u32 = 17;
pub const BLUETOOTH_PROXY: u32 = 18;
pub const PRESENCE_SENSITIVITY: u32 = 22;

pub type Encoded = (u16, Vec<u8>);

fn encode(message_type: u16, message: impl Message) -> Encoded {
    (message_type, message.encode_to_vec())
}

struct Sensor {
    key: u32,
    object_id: &'static str,
    name: &'static str,
    unit: &'static str,
    device_class: &'static str,
    decimals: i32,
    diagnostic: bool,
    icon: &'static str,
}

const SENSORS: [Sensor; 6] = [
    Sensor { key: ILLUMINANCE, object_id: "illuminance", name: "Illuminance", unit: "lx", device_class: "illuminance", decimals: 1, diagnostic: false, icon: "" },
    Sensor { key: PROXIMITY_RAW, object_id: "proximity", name: "Proximity", unit: "", device_class: "", decimals: 0, diagnostic: true, icon: "mdi:hand-wave" },
    Sensor { key: CPU_TEMPERATURE, object_id: "cpu_temperature", name: "CPU temperature", unit: "°C", device_class: "temperature", decimals: 1, diagnostic: true, icon: "" },
    Sensor { key: FREE_MEMORY, object_id: "free_memory", name: "Free memory", unit: "MB", device_class: "", decimals: 0, diagnostic: true, icon: "mdi:memory" },
    Sensor { key: UPTIME, object_id: "uptime", name: "Uptime", unit: "s", device_class: "duration", decimals: 0, diagnostic: true, icon: "" },
    Sensor { key: WIFI_SIGNAL, object_id: "wifi_signal", name: "Wi-Fi signal", unit: "dBm", device_class: "signal_strength", decimals: 0, diagnostic: true, icon: "" },
];

fn category(diagnostic: bool) -> i32 {
    if diagnostic {
        EntityCategory::Diagnostic as i32
    } else {
        EntityCategory::None as i32
    }
}

pub fn list() -> Vec<Encoded> {
    let mut messages = vec![encode(
        message_type::LIST_ENTITIES_LIGHT,
        ListEntitiesLightResponse {
            object_id: "display".into(),
            key: DISPLAY,
            name: "Display".into(),
            supported_color_modes: vec![ColorMode::Brightness as i32],
            icon: "mdi:tablet-dashboard".into(),
            ..Default::default()
        },
    )];
    messages.push(encode(
        message_type::LIST_ENTITIES_BINARY_SENSOR,
        ListEntitiesBinarySensorResponse {
            object_id: "presence".into(),
            key: PRESENCE,
            name: "Presence".into(),
            device_class: "occupancy".into(),
            ..Default::default()
        },
    ));
    messages.push(encode(
        message_type::LIST_ENTITIES_BINARY_SENSOR,
        ListEntitiesBinarySensorResponse {
            object_id: "radio_client_connected".into(),
            key: ZIGBEE_CLIENT,
            name: "Radio client connected".into(),
            device_class: "connectivity".into(),
            icon: "mdi:lan-connect".into(),
            entity_category: EntityCategory::Diagnostic as i32,
            ..Default::default()
        },
    ));
    messages.push(encode(
        message_type::LIST_ENTITIES_SELECT,
        ListEntitiesSelectResponse {
            object_id: "radio_mode".into(),
            key: RADIO_MODE,
            name: "Radio mode".into(),
            icon: "mdi:radio-tower".into(),
            options: Mode::ALL.iter().map(|mode| mode.label().to_string()).collect(),
            entity_category: EntityCategory::Config as i32,
            ..Default::default()
        },
    ));
    messages.push(encode(
        message_type::LIST_ENTITIES_SWITCH,
        ListEntitiesSwitchResponse {
            object_id: "bluetooth_proxy".into(),
            key: BLUETOOTH_PROXY,
            name: "Bluetooth proxy".into(),
            icon: "mdi:bluetooth".into(),
            entity_category: EntityCategory::Config as i32,
            ..Default::default()
        },
    ));
    messages.push(encode(
        message_type::LIST_ENTITIES_NUMBER,
        ListEntitiesNumberResponse {
            object_id: "presence_sensitivity".into(),
            key: PRESENCE_SENSITIVITY,
            name: "Presence sensitivity".into(),
            icon: "mdi:motion-sensor".into(),
            min_value: MINIMUM_SENSITIVITY as f32,
            max_value: MAXIMUM_SENSITIVITY as f32,
            step: 1.0,
            unit_of_measurement: "%".into(),
            mode: NumberMode::Slider as i32,
            entity_category: EntityCategory::Config as i32,
            ..Default::default()
        },
    ));
    for sensor in &SENSORS {
        messages.push(encode(
            message_type::LIST_ENTITIES_SENSOR,
            ListEntitiesSensorResponse {
                object_id: sensor.object_id.into(),
                key: sensor.key,
                name: sensor.name.into(),
                icon: sensor.icon.into(),
                unit_of_measurement: sensor.unit.into(),
                accuracy_decimals: sensor.decimals,
                device_class: sensor.device_class.into(),
                state_class: SensorStateClass::StateClassMeasurement as i32,
                entity_category: category(sensor.diagnostic),
                ..Default::default()
            },
        ));
    }
    for (key, object_id, name, icon) in [
        (RESTART_PANEL, "restart", "Restart", ""),
        (RESTART_KIOSK, "restart_kiosk", "Restart kiosk", "mdi:web-refresh"),
        (SIGN_IN_AGAIN, "sign_in_again", "Sign in again", "mdi:login"),
    ] {
        messages.push(encode(
            message_type::LIST_ENTITIES_BUTTON,
            ListEntitiesButtonResponse {
                object_id: object_id.into(),
                key,
                name: name.into(),
                icon: icon.into(),
                device_class: if key == RESTART_PANEL { "restart".into() } else { String::new() },
                entity_category: EntityCategory::Config as i32,
                ..Default::default()
            },
        ));
    }
    messages.push(encode(
        message_type::LIST_ENTITIES_TEXT,
        ListEntitiesTextResponse {
            object_id: "dashboard_url".into(),
            key: DASHBOARD_URL,
            name: "Dashboard URL".into(),
            icon: "mdi:link-variant".into(),
            entity_category: EntityCategory::Config as i32,
            min_length: 0,
            max_length: 255,
            mode: TextMode::Text as i32,
            ..Default::default()
        },
    ));
    for (key, object_id, name, icon) in [
        (IP_ADDRESS, "ip_address", "IP address", "mdi:ip-network"),
        (VERSION, "version", "WallView version", "mdi:package-variant"),
    ] {
        messages.push(encode(
            message_type::LIST_ENTITIES_TEXT_SENSOR,
            ListEntitiesTextSensorResponse {
                object_id: object_id.into(),
                key,
                name: name.into(),
                icon: icon.into(),
                entity_category: EntityCategory::Diagnostic as i32,
                ..Default::default()
            },
        ));
    }
    messages.push(encode(
        message_type::LIST_ENTITIES_SERVICES,
        ListEntitiesServicesResponse {
            name: "pair".into(),
            key: PAIR,
            args: vec![ListEntitiesServicesArgument { name: "secret".into(), r#type: ServiceArgType::String as i32, ..Default::default() }],
            ..Default::default()
        },
    ));
    messages
}

pub fn display_state(device: &Device) -> Encoded {
    let max = device.max_brightness().max(1) as f32;
    encode(
        message_type::LIGHT_STATE,
        LightStateResponse {
            key: DISPLAY,
            state: device.backlight_on(),
            brightness: device.brightness() as f32 / max,
            color_mode: ColorMode::Brightness as i32,
            ..Default::default()
        },
    )
}

pub fn presence_state(device: &Device) -> Option<(bool, Encoded)> {
    let near = device.presence.near()?;
    Some((
        near,
        encode(
            message_type::BINARY_SENSOR_STATE,
            BinarySensorStateResponse { key: PRESENCE, state: near, missing_state: false, ..Default::default() },
        ),
    ))
}

pub fn presence_sensitivity_state(device: &Device) -> Encoded {
    encode(
        message_type::NUMBER_STATE,
        NumberStateResponse { key: PRESENCE_SENSITIVITY, state: device.presence.sensitivity() as f32, missing_state: false, ..Default::default() },
    )
}

pub fn url_state(device: &Device) -> Encoded {
    encode(
        message_type::TEXT_STATE,
        TextStateResponse { key: DASHBOARD_URL, state: device.url(), missing_state: false, ..Default::default() },
    )
}

fn sensor_value(device: &Device, key: u32) -> Option<f64> {
    match key {
        ILLUMINANCE => device.illuminance(),
        PROXIMITY_RAW => device.proximity(),
        CPU_TEMPERATURE => device.cpu_temperature(),
        FREE_MEMORY => device.free_memory_mb(),
        UPTIME => device.uptime_s(),
        WIFI_SIGNAL => device.wifi_signal_dbm(),
        _ => None,
    }
}

pub fn sensor_states(device: &Device) -> Vec<Encoded> {
    SENSORS
        .iter()
        .map(|sensor| {
            let value = sensor_value(device, sensor.key);
            encode(
                message_type::SENSOR_STATE,
                SensorStateResponse {
                    key: sensor.key,
                    state: value.unwrap_or(f64::NAN) as f32,
                    missing_state: value.is_none(),
                    ..Default::default()
                },
            )
        })
        .collect()
}

pub fn text_sensor_states(device: &Device) -> Vec<Encoded> {
    [(IP_ADDRESS, device.ip_address()), (VERSION, env!("CARGO_PKG_VERSION").to_string())]
        .into_iter()
        .map(|(key, state)| {
            encode(
                message_type::TEXT_SENSOR_STATE,
                TextSensorStateResponse { key, state, missing_state: false, ..Default::default() },
            )
        })
        .collect()
}

pub fn radio_states(device: &Device) -> Vec<Encoded> {
    vec![
        encode(
            message_type::SELECT_STATE,
            SelectStateResponse { key: RADIO_MODE, state: device.radio.mode().label().into(), missing_state: false, ..Default::default() },
        ),
        encode(
            message_type::BINARY_SENSOR_STATE,
            BinarySensorStateResponse {
                key: ZIGBEE_CLIENT,
                state: device.radio.client_connected(),
                missing_state: false,
                ..Default::default()
            },
        ),
    ]
}

pub fn bluetooth_proxy_state(device: &Device) -> Encoded {
    encode(
        message_type::SWITCH_STATE,
        SwitchStateResponse { key: BLUETOOTH_PROXY, state: device.bluetooth.proxy_enabled(), ..Default::default() },
    )
}

pub fn all_states(device: &Device) -> Vec<Encoded> {
    let mut states = vec![display_state(device), url_state(device), bluetooth_proxy_state(device), presence_sensitivity_state(device)];
    states.extend(radio_states(device));
    if let Some((_, presence)) = presence_state(device) {
        states.push(presence);
    }
    states.extend(sensor_states(device));
    states.extend(text_sensor_states(device));
    states
}

pub fn handle_light(device: &Device, data: &[u8]) -> Option<Encoded> {
    let command = LightCommandRequest::decode(data).ok()?;
    if command.key != DISPLAY {
        return None;
    }
    if command.has_brightness {
        let max = device.max_brightness() as f32;
        device.set_brightness((command.brightness.clamp(0.0, 1.0) * max).round() as u32);
    }
    if command.has_state {
        device.set_backlight(command.state);
    }
    Some(display_state(device))
}

pub fn handle_button(device: &Device, data: &[u8]) {
    let Ok(command) = ButtonCommandRequest::decode(data) else {
        return;
    };
    match command.key {
        RESTART_PANEL => device.reboot(),
        RESTART_KIOSK => device.restart_kiosk(),
        SIGN_IN_AGAIN => device.request_sign_in(),
        _ => {}
    }
}

pub fn handle_select(device: &Device, data: &[u8]) {
    let Ok(command) = SelectCommandRequest::decode(data) else {
        return;
    };
    if command.key == RADIO_MODE {
        if let Some(mode) = Mode::from_label(&command.state) {
            device.radio.set_mode(mode);
        }
    }
}

pub fn handle_switch(device: &Device, data: &[u8]) {
    let Ok(command) = SwitchCommandRequest::decode(data) else {
        return;
    };
    if command.key == BLUETOOTH_PROXY {
        device.bluetooth.set_proxy(command.state);
    }
}

pub fn handle_number(device: &Device, data: &[u8]) -> Option<Encoded> {
    let command = NumberCommandRequest::decode(data).ok()?;
    if command.key != PRESENCE_SENSITIVITY || !command.state.is_finite() {
        return None;
    }
    let value = command.state.round().clamp(MINIMUM_SENSITIVITY as f32, MAXIMUM_SENSITIVITY as f32) as u32;
    device.presence.set_sensitivity(value);
    Some(presence_sensitivity_state(device))
}

pub fn handle_service(device: &Device, data: &[u8]) {
    let Ok(request) = ExecuteServiceRequest::decode(data) else {
        return;
    };
    if request.key == PAIR {
        if let Some(argument) = request.args.first() {
            device.start_pairing(&argument.string);
        }
    }
}

pub fn handle_text(device: &Device, data: &[u8]) -> Option<Encoded> {
    let command = TextCommandRequest::decode(data).ok()?;
    if command.key != DASHBOARD_URL {
        return None;
    }
    device.set_url(command.state.trim());
    Some(url_state(device))
}
