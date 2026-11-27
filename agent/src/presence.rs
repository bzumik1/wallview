use std::fs::{self, File};
use std::io::Write;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::device::Device;

const SETTINGS_FILE: &str = "/data/wallview/presence";
const TICK: Duration = Duration::from_millis(100);
const MOST_SENSITIVE_RISE: f64 = 2.0;
const LEAST_SENSITIVE_RISE: f64 = 1000.0;
const RELEASE_FRACTION: f64 = 0.5;
const BASELINE_FOLLOW: f64 = 0.01;
pub const HOLD: Duration = Duration::from_secs(1);
pub const MINIMUM_SENSITIVITY: u32 = 1;
pub const MAXIMUM_SENSITIVITY: u32 = 100;
pub const DEFAULT_SENSITIVITY: u32 = 50;

pub fn rise_for(sensitivity: u32) -> f64 {
    let sensitivity = sensitivity.clamp(MINIMUM_SENSITIVITY, MAXIMUM_SENSITIVITY);
    let fraction = (MAXIMUM_SENSITIVITY - sensitivity) as f64 / (MAXIMUM_SENSITIVITY - MINIMUM_SENSITIVITY) as f64;
    MOST_SENSITIVE_RISE * (LEAST_SENSITIVE_RISE / MOST_SENSITIVE_RISE).powf(fraction)
}

fn parse_sensitivity(text: &str) -> u32 {
    text.lines()
        .filter_map(|line| line.split_once('='))
        .find(|(name, _)| name.trim() == "sensitivity")
        .and_then(|(_, value)| value.trim().parse::<u32>().ok())
        .unwrap_or(DEFAULT_SENSITIVITY)
        .clamp(MINIMUM_SENSITIVITY, MAXIMUM_SENSITIVITY)
}

#[derive(Default)]
pub struct Detector {
    baseline: Option<f64>,
    near: bool,
    last_near: Option<Instant>,
}

impl Detector {
    pub fn update(&mut self, raw: f64, rise: f64, now: Instant) -> bool {
        let baseline = self.baseline.map_or(raw, |baseline| baseline.min(raw));
        let signal = raw - baseline;
        if signal >= rise || (self.near && signal >= rise * RELEASE_FRACTION) {
            self.near = true;
            self.last_near = Some(now);
        } else if self.near && self.last_near.is_none_or(|last| now.duration_since(last) >= HOLD) {
            self.near = false;
        }
        self.baseline = Some(if self.near { baseline } else { baseline + (raw - baseline) * BASELINE_FOLLOW });
        self.near
    }
}

struct State {
    sensitivity: u32,
    near: Option<bool>,
}

struct Shared {
    state: Mutex<State>,
    store: Mutex<()>,
}

#[derive(Clone)]
pub struct Presence {
    shared: Arc<Shared>,
}

impl Presence {
    pub fn load() -> Self {
        let sensitivity = fs::read_to_string(SETTINGS_FILE).map_or(DEFAULT_SENSITIVITY, |text| parse_sensitivity(&text));
        Presence {
            shared: Arc::new(Shared {
                state: Mutex::new(State { sensitivity, near: None }),
                store: Mutex::new(()),
            }),
        }
    }

    pub fn sensitivity(&self) -> u32 {
        self.shared.state.lock().unwrap().sensitivity
    }

    pub fn near(&self) -> Option<bool> {
        self.shared.state.lock().unwrap().near
    }

    pub fn set_sensitivity(&self, sensitivity: u32) {
        let sensitivity = sensitivity.clamp(MINIMUM_SENSITIVITY, MAXIMUM_SENSITIVITY);
        let _store = self.shared.store.lock().unwrap();
        if self.sensitivity() == sensitivity {
            return;
        }
        let temporary = format!("{SETTINGS_FILE}.new");
        let stored = File::create(&temporary)
            .and_then(|mut file| {
                file.write_all(format!("sensitivity={sensitivity}\n").as_bytes())?;
                file.sync_all()
            })
            .and_then(|_| fs::rename(&temporary, SETTINGS_FILE));
        if let Err(error) = stored {
            eprintln!("wallview-agent: presence: cannot store the sensitivity: {error}");
        }
        self.shared.state.lock().unwrap().sensitivity = sensitivity;
    }

    fn set_near(&self, near: Option<bool>) {
        self.shared.state.lock().unwrap().near = near;
    }
}

pub fn run(device: Device) {
    let presence = device.presence.clone();
    let mut detector = Detector::default();
    loop {
        let near = device.proximity().map(|raw| detector.update(raw, rise_for(presence.sensitivity()), Instant::now()));
        if near.is_none() {
            detector = Detector::default();
        }
        presence.set_near(near);
        thread::sleep(TICK);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feed(detector: &mut Detector, start: Instant, samples: &[(u64, f64)], rise: f64) -> Vec<bool> {
        samples.iter().map(|(millis, raw)| detector.update(*raw, rise, start + Duration::from_millis(*millis))).collect()
    }

    #[test]
    fn sensitivity_maps_to_a_rise_that_falls_as_sensitivity_grows() {
        assert_eq!(rise_for(MAXIMUM_SENSITIVITY), MOST_SENSITIVE_RISE);
        assert!((rise_for(MINIMUM_SENSITIVITY) - LEAST_SENSITIVE_RISE).abs() < 1e-9);
        assert!(rise_for(DEFAULT_SENSITIVITY) < 97.0);
        assert!(rise_for(60) < rise_for(40));
        assert_eq!(rise_for(0), rise_for(MINIMUM_SENSITIVITY));
        assert_eq!(rise_for(500), rise_for(MAXIMUM_SENSITIVITY));
    }

    #[test]
    fn settings_parse_with_default_and_clamp() {
        assert_eq!(parse_sensitivity("sensitivity=70\n"), 70);
        assert_eq!(parse_sensitivity(""), DEFAULT_SENSITIVITY);
        assert_eq!(parse_sensitivity("sensitivity=abc"), DEFAULT_SENSITIVITY);
        assert_eq!(parse_sensitivity("sensitivity=0"), MINIMUM_SENSITIVITY);
        assert_eq!(parse_sensitivity("sensitivity=250"), MAXIMUM_SENSITIVITY);
    }

    #[test]
    fn rise_above_the_baseline_is_near_not_the_absolute_value() {
        let mut detector = Detector::default();
        let start = Instant::now();
        assert_eq!(feed(&mut detector, start, &[(0, 300.0), (100, 300.0), (200, 330.0)], 20.0), [false, false, true]);
    }

    #[test]
    fn baseline_follows_down_at_once() {
        let mut detector = Detector::default();
        let start = Instant::now();
        assert_eq!(feed(&mut detector, start, &[(0, 300.0), (100, 3.0), (200, 30.0)], 20.0), [false, false, true]);
    }

    #[test]
    fn slow_drift_is_absorbed_into_the_baseline() {
        let mut detector = Detector::default();
        let start = Instant::now();
        let drift: Vec<(u64, f64)> = (0..3000).map(|tick| (tick * 100, 3.0 + tick as f64 * 0.01)).collect();
        assert!(feed(&mut detector, start, &drift, 20.0).iter().all(|near| !near));
    }

    #[test]
    fn a_short_wave_stays_near_for_the_hold_time() {
        let mut detector = Detector::default();
        let start = Instant::now();
        let near = feed(&mut detector, start, &[(0, 3.0), (100, 60.0), (200, 3.0), (900, 3.0), (1100, 3.0), (1200, 3.0)], 20.0);
        assert_eq!(near, [false, true, true, true, false, false]);
    }

    #[test]
    fn hysteresis_keeps_near_until_the_signal_falls_below_half() {
        let mut detector = Detector::default();
        let start = Instant::now();
        let near = feed(&mut detector, start, &[(0, 3.0), (100, 30.0), (1500, 15.0), (3000, 14.0), (4500, 12.0)], 20.0);
        assert_eq!(near, [false, true, true, true, false]);
    }

    #[test]
    fn baseline_does_not_rise_while_near() {
        let mut detector = Detector::default();
        let start = Instant::now();
        let hand: Vec<(u64, f64)> = std::iter::once((0, 3.0)).chain((1..600).map(|tick| (tick * 100, 60.0))).collect();
        assert!(feed(&mut detector, start, &hand, 20.0)[1..].iter().all(|near| *near));
    }
}
