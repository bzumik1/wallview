use std::fs;

pub struct Config {
    pub host: String,
    pub port: u16,
    pub username: Option<String>,
    pub password: Option<String>,
}

impl Config {
    pub fn load(path: &str) -> Result<Self, String> {
        let text = fs::read_to_string(path).map_err(|error| format!("cannot read {path}: {error}"))?;
        let mut config = Config {
            host: String::new(),
            port: 1883,
            username: None,
            password: None,
        };
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                return Err(format!("invalid line in {path}: {line}"));
            };
            let value = value.trim().to_string();
            match key.trim() {
                "host" => config.host = value,
                "port" => config.port = value.parse().map_err(|_| format!("invalid port {value}"))?,
                "username" => config.username = Some(value),
                "password" => config.password = Some(value),
                other => return Err(format!("unknown key {other} in {path}")),
            }
        }
        if config.host.is_empty() {
            return Err(format!("host missing in {path}"));
        }
        Ok(config)
    }
}
