use std::fs::{self, File};
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread;
use std::time::Duration;

use crate::device::Device;

const SETTINGS_FILE: &str = "/data/wallview/brightness";
const TICK: Duration = Duration::from_millis(50);
const SAMPLE_TICKS: u32 = 10;
const SMOOTHING: f64 = 0.12;
const HYSTERESIS: f64 = 0.03;
const RAMP_STEP: f64 = 0.02;
const DARK_LUX: f64 = 1.0;
const BRIGHT_LUX: f64 = 300.0;
pub const DEFAULT_MINIMUM: u32 = 5;
pub const DEFAULT_MAXIMUM: u32 = 100;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Settings {
    pub auto: bool,
    pub minimum: u32,
    pub maximum: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { auto: false, minimum: DEFAULT_MINIMUM, maximum: DEFAULT_MAXIMUM }
    }
}

impl Settings {
    fn parse(text: &str) -> Settings {
        let mut settings = Settings::default();
        for line in text.lines() {
            let Some((name, value)) = line.split_once('=') else { continue };
            match (name.trim(), value.trim()) {
                ("auto", value) => settings.auto = value == "on",
                ("minimum", value) => settings.minimum = value.parse().unwrap_or(settings.minimum),
                ("maximum", value) => settings.maximum = value.parse().unwrap_or(settings.maximum),
                _ => {}
            }
        }
        settings.minimum = settings.minimum.clamp(1, 100);
        settings.maximum = settings.maximum.clamp(1, 100);
        settings
    }

    fn render(&self) -> String {
        let auto = if self.auto { "on" } else { "off" };
        format!("auto={auto}\nminimum={}\nmaximum={}\n", self.minimum, self.maximum)
    }
}

pub fn perceived_level(lux: f64) -> f64 {
    let lux = lux.max(DARK_LUX);
    ((lux.ln() - DARK_LUX.ln()) / (BRIGHT_LUX.ln() - DARK_LUX.ln())).clamp(0.0, 1.0)
}

pub fn perceived_brightness(level: f64, settings: &Settings) -> f64 {
    let low = (settings.minimum.min(settings.maximum) as f64 / 100.0).sqrt();
    let high = (settings.maximum.max(settings.minimum) as f64 / 100.0).sqrt();
    low + (high - low) * level.clamp(0.0, 1.0)
}

pub fn duty(perceived: f64, max_brightness: u32) -> u32 {
    let perceived = perceived.clamp(0.0, 1.0);
    ((perceived * perceived * max_brightness as f64).round() as u32).clamp(1, max_brightness.max(1))
}

pub fn perceived_of_duty(duty: u32, max_brightness: u32) -> f64 {
    (duty as f64 / max_brightness.max(1) as f64).clamp(0.0, 1.0).sqrt()
}

pub fn step_toward(current: f64, target: f64) -> f64 {
    if (target - current).abs() <= RAMP_STEP {
        target
    } else if target > current {
        current + RAMP_STEP
    } else {
        current - RAMP_STEP
    }
}

#[derive(Default)]
struct Shared {
    settings: Mutex<Settings>,
    changes: AtomicU64,
}

#[derive(Clone)]
pub struct Brightness {
    shared: Arc<Shared>,
}

impl Brightness {
    pub fn load() -> Self {
        let settings = fs::read_to_string(SETTINGS_FILE).map(|text| Settings::parse(&text)).unwrap_or_default();
        Brightness { shared: Arc::new(Shared { settings: Mutex::new(settings), changes: AtomicU64::new(0) }) }
    }

    pub fn settings(&self) -> Settings {
        *self.shared.settings.lock().unwrap()
    }

    pub fn changes(&self) -> u64 {
        self.shared.changes.load(Ordering::SeqCst)
    }

    fn changed(&self) {
        self.shared.changes.fetch_add(1, Ordering::SeqCst);
    }

    fn lock(&self) -> MutexGuard<'_, Settings> {
        self.shared.settings.lock().unwrap()
    }

    fn update(&self, change: impl FnOnce(&mut Settings)) {
        let mut settings = self.lock();
        let mut updated = *settings;
        change(&mut updated);
        if updated != *settings {
            match store(&updated) {
                Ok(()) => *settings = updated,
                Err(error) => eprintln!("wallview-agent: brightness: cannot store settings: {error}"),
            }
        }
        drop(settings);
        self.changed();
    }

    pub fn set_auto(&self, auto: bool) {
        self.update(|settings| settings.auto = auto);
    }

    pub fn set_minimum(&self, minimum: u32) {
        self.update(|settings| settings.minimum = minimum.clamp(1, 100));
    }

    pub fn set_maximum(&self, maximum: u32) {
        self.update(|settings| settings.maximum = maximum.clamp(1, 100));
    }

    pub fn set_manual(&self, write: impl FnOnce()) {
        let mut settings = self.lock();
        if settings.auto {
            let manual = Settings { auto: false, ..*settings };
            match store(&manual) {
                Ok(()) => *settings = manual,
                Err(error) => eprintln!("wallview-agent: brightness: cannot store settings: {error}"),
            }
        }
        write();
        drop(settings);
        self.changed();
    }
}

fn store(settings: &Settings) -> std::io::Result<()> {
    let temporary = format!("{SETTINGS_FILE}.new");
    let mut file = File::create(&temporary)?;
    file.write_all(settings.render().as_bytes())?;
    file.sync_all()?;
    fs::rename(&temporary, SETTINGS_FILE)
}

pub fn run(device: Device) {
    let brightness = device.brightness_control.clone();
    let mut smoothed: Option<f64> = None;
    let mut target: Option<f64> = None;
    let mut ticks = 0;
    let mut ramping = false;
    loop {
        let settings = brightness.settings();
        if !settings.auto {
            smoothed = None;
            target = None;
            ramping = false;
            thread::sleep(TICK * SAMPLE_TICKS);
            continue;
        }
        if ticks == 0 || smoothed.is_none() {
            if let Some(lux) = device.illuminance() {
                let level = perceived_level(lux);
                let next = smoothed.map_or(level, |previous| previous + SMOOTHING * (level - previous));
                smoothed = Some(next);
                let wanted = perceived_brightness(next, &settings);
                if target.is_none_or(|current| (wanted - current).abs() > HYSTERESIS) {
                    target = Some(wanted);
                }
            }
        }
        ticks = (ticks + 1) % SAMPLE_TICKS;
        let Some(goal) = target else {
            thread::sleep(TICK);
            continue;
        };
        let guard = brightness.lock();
        if !guard.auto {
            continue;
        }
        let max = device.max_brightness();
        let current = perceived_of_duty(device.brightness(), max);
        let wanted = duty(goal, max);
        let moving = device.brightness() != wanted;
        if moving {
            let present = device.brightness();
            let next = duty(step_toward(current, goal), max);
            let next = if next != present { next } else if wanted > present { present + 1 } else { present - 1 };
            device.write_brightness(next);
        }
        drop(guard);
        if ramping && !moving {
            brightness.changed();
        }
        ramping = moving;
        thread::sleep(if moving { TICK } else { TICK * SAMPLE_TICKS });
        if !moving {
            ticks = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_light_logarithmically_between_dark_and_bright() {
        assert_eq!(perceived_level(0.0), 0.0);
        assert_eq!(perceived_level(DARK_LUX), 0.0);
        assert_eq!(perceived_level(BRIGHT_LUX), 1.0);
        assert_eq!(perceived_level(10_000.0), 1.0);
        let middle = perceived_level((DARK_LUX * BRIGHT_LUX).sqrt());
        assert!((middle - 0.5).abs() < 1e-9);
    }

    #[test]
    fn keeps_auto_brightness_between_minimum_and_maximum() {
        let settings = Settings { auto: true, minimum: 4, maximum: 64 };
        assert_eq!(duty(perceived_brightness(0.0, &settings), 255), 10);
        assert_eq!(duty(perceived_brightness(1.0, &settings), 255), 163);
        let swapped = Settings { auto: true, minimum: 64, maximum: 4 };
        assert_eq!(perceived_brightness(0.0, &swapped), perceived_brightness(0.0, &settings));
    }

    #[test]
    fn never_turns_the_backlight_fully_dark() {
        assert_eq!(duty(0.0, 255), 1);
        assert_eq!(duty(1.0, 255), 255);
        assert!((perceived_of_duty(64, 256) - 0.5).abs() < 1e-9);
    }

    #[test]
    fn ramps_in_small_steps_and_lands_on_target() {
        assert!((step_toward(0.2, 0.8) - 0.22).abs() < 1e-9);
        assert!((step_toward(0.8, 0.2) - 0.78).abs() < 1e-9);
        assert_eq!(step_toward(0.5, 0.51), 0.51);
    }

    #[test]
    fn reads_stored_settings_and_falls_back_to_defaults() {
        assert_eq!(Settings::parse(""), Settings::default());
        let stored = Settings { auto: true, minimum: 12, maximum: 80 };
        assert_eq!(Settings::parse(&stored.render()), stored);
        assert_eq!(Settings::parse("auto=on\nminimum=0\nmaximum=500\n"), Settings { auto: true, minimum: 1, maximum: 100 });
        assert_eq!(Settings::parse("minimum=x\n").minimum, DEFAULT_MINIMUM);
    }
}
