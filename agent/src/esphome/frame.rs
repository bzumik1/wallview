use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};

use snow::TransportState;

const PATTERN: &str = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
const PROLOGUE: &[u8] = b"NoiseAPIInit\x00\x00";
const PLAINTEXT_PREAMBLE: u8 = 0x00;
const NOISE_PREAMBLE: u8 = 0x01;
const PROTOCOL_NOISE: u8 = 0x01;
const MAX_FRAME: usize = 65535;

fn read_frame(stream: &mut TcpStream) -> io::Result<Vec<u8>> {
    let mut header = [0u8; 3];
    stream.read_exact(&mut header)?;
    if header[0] != NOISE_PREAMBLE {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "client does not use encryption"));
    }
    let mut frame = vec![0u8; ((header[1] as usize) << 8) | header[2] as usize];
    stream.read_exact(&mut frame)?;
    Ok(frame)
}

fn write_frame(stream: &mut TcpStream, payload: &[u8]) -> io::Result<()> {
    let mut frame = Vec::with_capacity(payload.len() + 3);
    frame.push(NOISE_PREAMBLE);
    frame.push((payload.len() >> 8) as u8);
    frame.push(payload.len() as u8);
    frame.extend_from_slice(payload);
    stream.write_all(&frame)
}

fn read_varint(stream: &mut TcpStream) -> io::Result<usize> {
    let mut value = 0usize;
    for shift in (0..28).step_by(7) {
        let mut byte = [0u8; 1];
        stream.read_exact(&mut byte)?;
        value |= ((byte[0] & 0x7f) as usize) << shift;
        if byte[0] & 0x80 == 0 {
            return Ok(value);
        }
    }
    Err(protocol_error("varint too long"))
}

fn write_varint(buffer: &mut Vec<u8>, mut value: usize) {
    while value >= 0x80 {
        buffer.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    buffer.push(value as u8);
}

fn read_plaintext(stream: &mut TcpStream) -> io::Result<(u16, Vec<u8>)> {
    let mut preamble = [0u8; 1];
    stream.read_exact(&mut preamble)?;
    if preamble[0] != PLAINTEXT_PREAMBLE {
        return Err(protocol_error("invalid plaintext frame"));
    }
    let length = read_varint(stream)?;
    let message_type = u16::try_from(read_varint(stream)?).map_err(protocol_error)?;
    if length > MAX_FRAME {
        return Err(protocol_error("message too long"));
    }
    let mut data = vec![0u8; length];
    stream.read_exact(&mut data)?;
    Ok((message_type, data))
}

fn reject(stream: &mut TcpStream, reason: &[u8]) -> io::Result<()> {
    let mut failure = vec![0x01];
    failure.extend_from_slice(reason);
    write_frame(stream, &failure)
}

fn protocol_error(error: impl std::fmt::Display) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, error.to_string())
}

type Transport = Option<Arc<Mutex<TransportState>>>;

pub struct Connection {
    reader: TcpStream,
    writer: Arc<Mutex<TcpStream>>,
    transport: Transport,
}

#[derive(Clone)]
pub struct Sender {
    writer: Arc<Mutex<TcpStream>>,
    transport: Transport,
}

impl Connection {
    pub fn accept(mut stream: TcpStream, key: Option<[u8; 32]>, name: &str, mac: &str) -> io::Result<Self> {
        let mut preamble = [0u8; 1];
        stream.peek(&mut preamble)?;
        if preamble[0] == PLAINTEXT_PREAMBLE && key.is_none() {
            return Ok(Connection {
                writer: Arc::new(Mutex::new(stream.try_clone()?)),
                reader: stream,
                transport: None,
            });
        }
        let psk = key.unwrap_or([0u8; 32]);
        if let Err(error) = read_frame(&mut stream) {
            if error.kind() == io::ErrorKind::InvalidData {
                reject(&mut stream, b"Bad indicator byte")?;
            }
            return Err(error);
        }
        let mut hello = vec![PROTOCOL_NOISE];
        hello.extend_from_slice(name.as_bytes());
        hello.push(0);
        hello.extend_from_slice(mac.as_bytes());
        hello.push(0);
        write_frame(&mut stream, &hello)?;

        let handshake = read_frame(&mut stream)?;
        if handshake.first() != Some(&0) {
            return Err(protocol_error("invalid handshake frame"));
        }
        let mut noise = snow::Builder::new(PATTERN.parse().map_err(protocol_error)?)
            .psk(0, &psk)
            .prologue(PROLOGUE)
            .build_responder()
            .map_err(protocol_error)?;
        let mut buffer = vec![0u8; MAX_FRAME];
        if noise.read_message(&handshake[1..], &mut buffer).is_err() {
            reject(&mut stream, b"Handshake MAC failure")?;
            return Err(protocol_error("handshake MAC failure (wrong encryption key)"));
        }
        let length = noise.write_message(&[], &mut buffer).map_err(protocol_error)?;
        let mut reply = vec![0x00];
        reply.extend_from_slice(&buffer[..length]);
        write_frame(&mut stream, &reply)?;

        let transport = noise.into_transport_mode().map_err(protocol_error)?;
        Ok(Connection {
            writer: Arc::new(Mutex::new(stream.try_clone()?)),
            reader: stream,
            transport: Some(Arc::new(Mutex::new(transport))),
        })
    }

    pub fn sender(&self) -> Sender {
        Sender {
            writer: self.writer.clone(),
            transport: self.transport.clone(),
        }
    }

    pub fn receive(&mut self) -> io::Result<(u16, Vec<u8>)> {
        let Some(transport) = &self.transport else {
            return read_plaintext(&mut self.reader);
        };
        let frame = read_frame(&mut self.reader)?;
        let mut plain = vec![0u8; frame.len()];
        let length = transport
            .lock()
            .map_err(protocol_error)?
            .read_message(&frame, &mut plain)
            .map_err(protocol_error)?;
        if length < 4 {
            return Err(protocol_error("message too short"));
        }
        let message_type = u16::from_be_bytes([plain[0], plain[1]]);
        let data_length = (u16::from_be_bytes([plain[2], plain[3]]) as usize).min(length - 4);
        Ok((message_type, plain[4..4 + data_length].to_vec()))
    }
}

impl Sender {
    pub fn close(&self) {
        if let Ok(writer) = self.writer.lock() {
            let _ = writer.shutdown(std::net::Shutdown::Both);
        }
    }

    pub fn send(&self, message_type: u16, data: &[u8]) -> io::Result<()> {
        let Some(transport) = &self.transport else {
            let mut frame = vec![PLAINTEXT_PREAMBLE];
            write_varint(&mut frame, data.len());
            write_varint(&mut frame, message_type as usize);
            frame.extend_from_slice(data);
            return self.writer.lock().map_err(protocol_error)?.write_all(&frame);
        };
        let mut plain = Vec::with_capacity(data.len() + 4);
        plain.extend_from_slice(&message_type.to_be_bytes());
        plain.extend_from_slice(&(data.len() as u16).to_be_bytes());
        plain.extend_from_slice(data);
        let mut encrypted = vec![0u8; plain.len() + 16];
        let length = transport
            .lock()
            .map_err(protocol_error)?
            .write_message(&plain, &mut encrypted)
            .map_err(protocol_error)?;
        let mut writer = self.writer.lock().map_err(protocol_error)?;
        write_frame(&mut writer, &encrypted[..length])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn varint_round_trip() {
        for value in [0usize, 1, 127, 128, 300, 16_383, 16_384, 65_535] {
            let mut buffer = Vec::new();
            write_varint(&mut buffer, value);
            let decoded = buffer
                .iter()
                .enumerate()
                .fold(0usize, |sum, (index, byte)| sum | (((byte & 0x7f) as usize) << (7 * index)));
            assert_eq!(decoded, value);
        }
    }
}
