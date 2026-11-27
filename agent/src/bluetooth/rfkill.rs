use std::fs;
use std::io;
use std::path::{Path, PathBuf};

const RFKILL: &str = "/sys/class/rfkill";

fn bluetooth_switches(root: &Path) -> Vec<PathBuf> {
    fs::read_dir(root)
        .into_iter()
        .flatten()
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| fs::read_to_string(path.join("type")).is_ok_and(|kind| kind.trim() == "bluetooth"))
        .collect()
}

fn set_blocked_in(root: &Path, blocked: bool) -> io::Result<usize> {
    let switches = bluetooth_switches(root);
    for switch in &switches {
        fs::write(switch.join("soft"), if blocked { "1" } else { "0" })?;
    }
    Ok(switches.len())
}

fn all_blocked_in(root: &Path) -> bool {
    let switches = bluetooth_switches(root);
    !switches.is_empty()
        && switches.iter().all(|switch| fs::read_to_string(switch.join("soft")).is_ok_and(|soft| soft.trim() == "1"))
}

pub fn all_blocked() -> bool {
    all_blocked_in(Path::new(RFKILL))
}

pub fn set_blocked(blocked: bool) -> io::Result<usize> {
    set_blocked_in(Path::new(RFKILL), blocked)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_only_bluetooth_switches() {
        let root = std::env::temp_dir().join(format!("wallview-rfkill-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for (name, kind) in [("rfkill0", "wlan"), ("rfkill1", "bluetooth")] {
            fs::create_dir_all(root.join(name)).unwrap();
            fs::write(root.join(name).join("type"), format!("{kind}\n")).unwrap();
            fs::write(root.join(name).join("soft"), "0\n").unwrap();
        }
        assert!(!all_blocked_in(&root));
        assert_eq!(set_blocked_in(&root, true).unwrap(), 1);
        assert!(all_blocked_in(&root));
        assert_eq!(fs::read_to_string(root.join("rfkill1/soft")).unwrap(), "1");
        assert_eq!(fs::read_to_string(root.join("rfkill0/soft")).unwrap(), "0\n");
        set_blocked_in(&root, false).unwrap();
        assert_eq!(fs::read_to_string(root.join("rfkill1/soft")).unwrap(), "0");
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn is_not_blocked_without_a_bluetooth_switch() {
        let root = std::env::temp_dir().join(format!("wallview-rfkill-empty-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        assert!(!all_blocked_in(&root));
        assert_eq!(set_blocked_in(&root, true).unwrap(), 0);
        fs::remove_dir_all(&root).unwrap();
    }
}
