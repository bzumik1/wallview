use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};

use snow::TransportState;

const PATTERN: &str = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
const PROLOGUE: &[u8] = b"NoiseAPIInit\x00\x00";
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

fn protocol_error(error: impl std::fmt::Display) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, error.to_string())
}

pub struct Connection {
    reader: TcpStream,
    writer: Arc<Mutex<TcpStream>>,
    transport: Arc<Mutex<TransportState>>,
}

#[derive(Clone)]
pub struct Sender {
    writer: Arc<Mutex<TcpStream>>,
    transport: Arc<Mutex<TransportState>>,
}

impl Connection {
    pub fn accept(mut stream: TcpStream, psk: &[u8; 32], name: &str, mac: &str) -> io::Result<Self> {
        read_frame(&mut stream)?;
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
            .psk(0, psk)
            .prologue(PROLOGUE)
            .build_responder()
            .map_err(protocol_error)?;
        let mut buffer = vec![0u8; MAX_FRAME];
        if noise.read_message(&handshake[1..], &mut buffer).is_err() {
            let mut failure = vec![0x01];
            failure.extend_from_slice(b"Handshake MAC failure");
            write_frame(&mut stream, &failure)?;
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
            transport: Arc::new(Mutex::new(transport)),
        })
    }

    pub fn sender(&self) -> Sender {
        Sender {
            writer: self.writer.clone(),
            transport: self.transport.clone(),
        }
    }

    pub fn receive(&mut self) -> io::Result<(u16, Vec<u8>)> {
        let frame = read_frame(&mut self.reader)?;
        let mut plain = vec![0u8; frame.len()];
        let length = self
            .transport
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
    pub fn send(&self, message_type: u16, data: &[u8]) -> io::Result<()> {
        let mut plain = Vec::with_capacity(data.len() + 4);
        plain.extend_from_slice(&message_type.to_be_bytes());
        plain.extend_from_slice(&(data.len() as u16).to_be_bytes());
        plain.extend_from_slice(data);
        let mut encrypted = vec![0u8; plain.len() + 16];
        let length = self
            .transport
            .lock()
            .map_err(protocol_error)?
            .write_message(&plain, &mut encrypted)
            .map_err(protocol_error)?;
        let mut writer = self.writer.lock().map_err(protocol_error)?;
        write_frame(&mut writer, &encrypted[..length])
    }
}
