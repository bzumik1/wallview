use std::io;
use std::mem;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::time::Duration;

const AF_BLUETOOTH: libc::c_int = 31;
const BTPROTO_HCI: libc::c_int = 1;
const HCI_DEV_NONE: u16 = 0xffff;
const HCI_CHANNEL_CONTROL: u16 = 3;
const HEADER: usize = 6;
const EVENT_DEVICE_FOUND: u16 = 0x0012;
const EVENT_ADV_MONITOR_DEVICE_FOUND: u16 = 0x002f;
const ADDRESS_LE_PUBLIC: u8 = 1;
const ADDRESS_LE_RANDOM: u8 = 2;
const RSSI_INVALID: i8 = 127;
pub const MAX_DATA: usize = 62;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Advertisement {
    pub address: u64,
    pub address_type: u32,
    pub rssi: i32,
    pub data: Vec<u8>,
}

#[repr(C)]
struct SocketAddress {
    family: libc::sa_family_t,
    device: u16,
    channel: u16,
}

pub struct Control {
    socket: OwnedFd,
}

impl Control {
    pub fn open(read_timeout: Duration) -> io::Result<Self> {
        let raw = unsafe { libc::socket(AF_BLUETOOTH, libc::SOCK_RAW | libc::SOCK_CLOEXEC, BTPROTO_HCI) };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        let socket = unsafe { OwnedFd::from_raw_fd(raw) };
        let address = SocketAddress { family: AF_BLUETOOTH as libc::sa_family_t, device: HCI_DEV_NONE, channel: HCI_CHANNEL_CONTROL };
        let bound = unsafe {
            libc::bind(
                socket.as_raw_fd(),
                &address as *const SocketAddress as *const libc::sockaddr,
                mem::size_of::<SocketAddress>() as libc::socklen_t,
            )
        };
        if bound < 0 {
            return Err(io::Error::last_os_error());
        }
        let timeout = libc::timeval { tv_sec: read_timeout.as_secs() as libc::time_t, tv_usec: read_timeout.subsec_micros() as libc::suseconds_t };
        let set = unsafe {
            libc::setsockopt(
                socket.as_raw_fd(),
                libc::SOL_SOCKET,
                libc::SO_RCVTIMEO,
                &timeout as *const libc::timeval as *const libc::c_void,
                mem::size_of::<libc::timeval>() as libc::socklen_t,
            )
        };
        if set < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Control { socket })
    }

    pub fn receive(&self, buffer: &mut [u8]) -> io::Result<Option<usize>> {
        let read = unsafe { libc::read(self.socket.as_raw_fd(), buffer.as_mut_ptr() as *mut libc::c_void, buffer.len()) };
        if read >= 0 {
            return Ok(Some(read as usize));
        }
        let error = io::Error::last_os_error();
        match error.kind() {
            io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted => Ok(None),
            _ => Err(error),
        }
    }
}

fn u16_at(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(offset..offset + 2)?.try_into().ok()?))
}

fn truncate_structures(data: &[u8]) -> Vec<u8> {
    let mut end = 0;
    while end < data.len() {
        let length = data[end] as usize;
        if length == 0 {
            break;
        }
        let next = end + 1 + length;
        if next > data.len() || next > MAX_DATA {
            break;
        }
        end = next;
    }
    data[..end].to_vec()
}

fn device_found(parameters: &[u8]) -> Option<Advertisement> {
    let address_bytes = parameters.get(..6)?;
    let address_type = match *parameters.get(6)? {
        ADDRESS_LE_PUBLIC => 0,
        ADDRESS_LE_RANDOM => 1,
        _ => return None,
    };
    let rssi = *parameters.get(7)? as i8;
    if rssi == RSSI_INVALID {
        return None;
    }
    let data_length = u16_at(parameters, 12)? as usize;
    let data = parameters.get(14..14 + data_length)?;
    let address = address_bytes.iter().rev().fold(0u64, |address, byte| address << 8 | *byte as u64);
    Some(Advertisement { address, address_type, rssi: rssi as i32, data: truncate_structures(data) })
}

pub fn parse(packet: &[u8]) -> Option<Advertisement> {
    let event = u16_at(packet, 0)?;
    let length = u16_at(packet, 4)? as usize;
    let parameters = packet.get(HEADER..HEADER + length)?;
    match event {
        EVENT_DEVICE_FOUND => device_found(parameters),
        EVENT_ADV_MONITOR_DEVICE_FOUND => device_found(parameters.get(2..)?),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(event: u16, prefix: &[u8], address_type: u8, rssi: i8, data: &[u8]) -> Vec<u8> {
        let mut parameters = prefix.to_vec();
        parameters.extend_from_slice(&[0x66, 0x55, 0x44, 0x33, 0x22, 0x11, address_type, rssi as u8, 0, 0, 0, 0]);
        parameters.extend_from_slice(&(data.len() as u16).to_le_bytes());
        parameters.extend_from_slice(data);
        let mut bytes = event.to_le_bytes().to_vec();
        bytes.extend_from_slice(&0u16.to_le_bytes());
        bytes.extend_from_slice(&(parameters.len() as u16).to_le_bytes());
        bytes.extend_from_slice(&parameters);
        bytes
    }

    #[test]
    fn parses_device_found_with_esphome_address_order() {
        let data = [0x02, 0x01, 0x06, 0x03, 0x16, 0xd2, 0xfc];
        let advertisement = parse(&packet(EVENT_DEVICE_FOUND, &[], ADDRESS_LE_RANDOM, -70, &data)).unwrap();
        assert_eq!(advertisement, Advertisement { address: 0x1122_3344_5566, address_type: 1, rssi: -70, data: data.to_vec() });
    }

    #[test]
    fn parses_monitor_device_found() {
        let data = [0x02, 0x01, 0x06];
        let advertisement = parse(&packet(EVENT_ADV_MONITOR_DEVICE_FOUND, &[1, 0], ADDRESS_LE_PUBLIC, -40, &data)).unwrap();
        assert_eq!(advertisement.address_type, 0);
        assert_eq!(advertisement.data, data);
    }

    #[test]
    fn skips_classic_devices_and_invalid_rssi() {
        assert!(parse(&packet(EVENT_DEVICE_FOUND, &[], 0, -40, &[0x02, 0x01, 0x06])).is_none());
        assert!(parse(&packet(EVENT_DEVICE_FOUND, &[], ADDRESS_LE_PUBLIC, 127, &[0x02, 0x01, 0x06])).is_none());
    }

    #[test]
    fn skips_other_events_and_short_packets() {
        assert!(parse(&packet(0x0006, &[], ADDRESS_LE_PUBLIC, -40, &[])).is_none());
        assert!(parse(&[0x12, 0x00, 0x00]).is_none());
        let mut truncated = packet(EVENT_DEVICE_FOUND, &[], ADDRESS_LE_PUBLIC, -40, &[0x02, 0x01, 0x06]);
        truncated.truncate(truncated.len() - 1);
        assert!(parse(&truncated).is_none());
    }

    #[test]
    fn truncates_long_data_at_a_structure_boundary() {
        let mut data = Vec::new();
        for _ in 0..3 {
            data.push(30);
            data.extend_from_slice(&[0xff; 30]);
        }
        let advertisement = parse(&packet(EVENT_DEVICE_FOUND, &[], ADDRESS_LE_PUBLIC, -40, &data)).unwrap();
        assert_eq!(advertisement.data.len(), 62);
    }

    #[test]
    fn stops_at_a_malformed_structure() {
        assert_eq!(truncate_structures(&[0x02, 0x01, 0x06, 0x09, 0xff]), vec![0x02, 0x01, 0x06]);
        assert_eq!(truncate_structures(&[0x02, 0x01, 0x06, 0x00, 0x05]), vec![0x02, 0x01, 0x06]);
    }
}
