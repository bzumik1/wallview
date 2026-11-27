use std::fs::{self, File};
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread;
use std::time::Duration;

use crate::device::Device;

const SETTINGS_FILE: &str = "/data/wallview/brightness";
const TICK: Duration = Duration::from_millis(50);
const SAMPLE_TICKS: u32 = 10;
const IDLE: Duration = Duration::from_millis(500);
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

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Curve {
    Linear,
    Perceptual,
}

impl Curve {
    pub fn from_scale(scale: Option<&str>) -> Curve {
        if scale == Some("non-linear") {
            Curve::Perceptual
        } else {
            Curve::Linear
        }
    }

    fn perceived(self, fraction: f64) -> f64 {
        let fraction = fraction.clamp(0.0, 1.0);
        match self {
            Curve::Linear => fraction.sqrt(),
            Curve::Perceptual => fraction,
        }
    }

    fn fraction(self, perceived: f64) -> f64 {
        let perceived = perceived.clamp(0.0, 1.0);
        match self {
            Curve::Linear => perceived * perceived,
            Curve::Perceptual => perceived,
        }
    }
}

pub fn perceived_brightness(level: f64, settings: &Settings, curve: Curve) -> f64 {
    let low = curve.perceived(settings.minimum.min(settings.maximum) as f64 / 100.0);
    let high = curve.perceived(settings.maximum.max(settings.minimum) as f64 / 100.0);
    low + (high - low) * level.clamp(0.0, 1.0)
}

pub fn duty(perceived: f64, max_brightness: u32, curve: Curve) -> u32 {
    let max_brightness = max_brightness.max(1);
    ((curve.fraction(perceived) * max_brightness as f64).round() as u32).clamp(1, max_brightness)
}

pub fn perceived_of_duty(duty: u32, max_brightness: u32, curve: Curve) -> f64 {
    curve.perceived(duty as f64 / max_brightness.max(1) as f64)
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

pub fn next_duty(present: u32, goal: f64, max_brightness: u32, curve: Curve) -> u32 {
    let wanted = duty(goal, max_brightness, curve);
    if wanted == present {
        return present;
    }
    let stepped = duty(step_toward(perceived_of_duty(present, max_brightness, curve), goal), max_brightness, curve);
    if stepped != present {
        stepped
    } else if wanted > present {
        present + 1
    } else {
        present - 1
    }
}

#[derive(Default)]
struct Shared {
    settings: Mutex<Settings>,
    store: Mutex<()>,
    changes: AtomicU64,
}

#[derive(Clone)]
pub struct Brightness {
    shared: Arc<Shared>,
}

impl Brightness {
    pub fn load() -> Self {
        let settings = fs::read_to_string(SETTINGS_FILE).map(|text| Settings::parse(&text)).unwrap_or_default();
        Brightness { shared: Arc::new(Shared { settings: Mutex::new(settings), ..Default::default() }) }
    }

    pub fn settings(&self) -> Settings {
        *self.lock()
    }

    pub fn changes(&self) -> u64 {
        self.shared.changes.load(Ordering::SeqCst)
    }

    fn changed(&self) {
        self.shared.changes.fetch_add(1, Ordering::SeqCst);
    }

    fn lock(&self) -> MutexGuard<'_, Settings> {
        self.shared.settings.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn persist(&self) {
        let _store = self.shared.store.lock().unwrap_or_else(PoisonError::into_inner);
        if let Err(error) = store(&self.settings()) {
            eprintln!("wallview-agent: brightness: cannot store settings: {error}");
        }
    }

    fn update(&self, change: impl FnOnce(&mut Settings)) {
        let mut settings = self.lock();
        let before = *settings;
        change(&mut settings);
        let changed = *settings != before;
        drop(settings);
        if changed {
            self.persist();
        }
        self.changed();
    }

    pub fn set_auto(&self, auto: bool) {
        self.update(|settings| settings.auto = auto);
    }

    pub fn set_minimum(&self, minimum: u32) {
        self.update(|settings| {
            settings.minimum = minimum.clamp(1, 100);
            settings.maximum = settings.maximum.max(settings.minimum);
        });
    }

    pub fn set_maximum(&self, maximum: u32) {
        self.update(|settings| {
            settings.maximum = maximum.clamp(1, 100);
            settings.minimum = settings.minimum.min(settings.maximum);
        });
    }

    pub fn set_manual(&self, write: impl FnOnce()) {
        let mut settings = self.lock();
        let was_auto = settings.auto;
        settings.auto = false;
        write();
        drop(settings);
        if was_auto {
            self.persist();
        }
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
    let curve = device.backlight_curve();
    let mut smoothed: Option<f64> = None;
    let mut target: Option<f64> = None;
    let mut applied: Option<Settings> = None;
    let mut ticks = 0;
    let mut ramping = false;
    loop {
        let settings = brightness.settings();
        if !settings.auto {
            smoothed = None;
            target = None;
            applied = None;
            ramping = false;
            thread::sleep(IDLE);
            continue;
        }
        if applied != Some(settings) {
            applied = Some(settings);
            target = None;
        }
        if ticks == 0 || target.is_none() {
            if let Some(lux) = device.illuminance() {
                let level = perceived_level(lux);
                let next = smoothed.map_or(level, |previous| previous + SMOOTHING * (level - previous));
                smoothed = Some(next);
                let wanted = perceived_brightness(next, &settings, curve);
                if target.is_none_or(|current| (wanted - current).abs() > HYSTERESIS) {
                    target = Some(wanted);
                }
            }
        }
        ticks = (ticks + 1) % SAMPLE_TICKS;
        let Some(goal) = target else {
            thread::sleep(IDLE);
            continue;
        };
        let guard = brightness.lock();
        if !guard.auto {
            continue;
        }
        let present = device.brightness();
        let next = next_duty(present, goal, device.max_brightness(), curve);
        let moving = next != present;
        if moving {
            device.write_brightness(next);
        }
        drop(guard);
        if ramping && !moving {
            brightness.changed();
        }
        ramping = moving;
        if moving {
            thread::sleep(TICK);
        } else {
            ticks = 0;
            thread::sleep(IDLE);
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
    fn keeps_auto_brightness_between_minimum_and_maximum_on_a_linear_backlight() {
        let settings = Settings { auto: true, minimum: 4, maximum: 64 };
        assert_eq!(duty(perceived_brightness(0.0, &settings, Curve::Linear), 255, Curve::Linear), 10);
        assert_eq!(duty(perceived_brightness(1.0, &settings, Curve::Linear), 255, Curve::Linear), 163);
        assert_eq!(duty(perceived_brightness(0.5, &settings, Curve::Linear), 255, Curve::Linear), 64);
    }

    #[test]
    fn does_not_correct_a_backlight_that_is_already_perceptual() {
        let settings = Settings { auto: true, minimum: 10, maximum: 90 };
        assert_eq!(Curve::from_scale(Some("non-linear")), Curve::Perceptual);
        assert_eq!(Curve::from_scale(Some("linear")), Curve::Linear);
        assert_eq!(Curve::from_scale(None), Curve::Linear);
        assert_eq!(duty(perceived_brightness(0.5, &settings, Curve::Perceptual), 1000, Curve::Perceptual), 500);
        assert!((perceived_of_duty(250, 1000, Curve::Perceptual) - 0.25).abs() < 1e-9);
    }

    #[test]
    fn never_turns_the_backlight_fully_dark() {
        assert_eq!(duty(0.0, 255, Curve::Linear), 1);
        assert_eq!(duty(1.0, 255, Curve::Linear), 255);
        assert_eq!(duty(0.5, 0, Curve::Linear), 1);
        assert!((perceived_of_duty(64, 256, Curve::Linear) - 0.5).abs() < 1e-9);
    }

    #[test]
    fn ramps_in_small_steps_and_lands_on_target() {
        assert!((step_toward(0.2, 0.8) - 0.22).abs() < 1e-9);
        assert!((step_toward(0.8, 0.2) - 0.78).abs() < 1e-9);
        assert_eq!(step_toward(0.5, 0.51), 0.51);
        assert_eq!(next_duty(100, 1.0, 255, Curve::Linear), 106);
        assert_eq!(next_duty(255, 1.0, 255, Curve::Linear), 255);
        assert_eq!(next_duty(1, 0.1, 255, Curve::Linear), 2);
        assert_eq!(next_duty(2, 0.0, 255, Curve::Linear), 1);
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
