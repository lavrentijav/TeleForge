//! Length-prefixed framing.
//!
//! `u32` big-endian length, then that many bytes of UTF-8 JSON.
//!
//! Why a length prefix and not newline-delimited JSON: a `sync.push` page can be
//! megabytes and may contain newlines inside strings, so a delimiter needs
//! escaping and a reader that scans. A length prefix makes the read a single
//! bounded allocation, which is also what enforces the frame-size limit *before*
//! reading the body — an unauthenticated peer can therefore not make the agent
//! allocate an arbitrary amount of memory.
//!
//! The limit is checked twice: once against the declared length (cheap, before the
//! allocation) and once against the actual read (defends against a truncated
//! stream being treated as a shorter frame).

use std::io::{Read, Write};

use crate::error::{Error, Result};

pub const LEN_PREFIX: usize = 4;
pub const DEFAULT_MAX_FRAME: u32 = 1 << 20;

#[derive(Clone, Copy, Debug)]
pub struct FrameLimits {
    pub max_frame_bytes: u32,
}

impl Default for FrameLimits {
    fn default() -> Self {
        Self {
            max_frame_bytes: DEFAULT_MAX_FRAME,
        }
    }
}

pub fn write_frame<W: Write>(writer: &mut W, payload: &[u8], limits: FrameLimits) -> Result<()> {
    if payload.is_empty() {
        return Err(Error::Protocol(
            "refusing to write an empty frame".to_string(),
        ));
    }
    if payload.len() > limits.max_frame_bytes as usize {
        return Err(Error::Protocol(format!(
            "frame of {} bytes exceeds the {} limit",
            payload.len(),
            limits.max_frame_bytes
        )));
    }
    writer.write_all(&(payload.len() as u32).to_be_bytes())?;
    writer.write_all(payload)?;
    writer.flush()?;
    Ok(())
}

/// Read one frame. Returns `Ok(None)` on a clean end of stream, so the caller can
/// tell "peer closed" from "peer sent garbage".
pub fn read_frame<R: Read>(reader: &mut R, limits: FrameLimits) -> Result<Option<Vec<u8>>> {
    let mut prefix = [0u8; LEN_PREFIX];
    match reader.read_exact(&mut prefix) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(Error::Io(error)),
    }
    let declared = u32::from_be_bytes(prefix);
    if declared == 0 {
        return Err(Error::Protocol("zero-length frame".to_string()));
    }
    if declared > limits.max_frame_bytes {
        return Err(Error::Protocol(format!(
            "declared frame of {declared} bytes exceeds the {} limit",
            limits.max_frame_bytes
        )));
    }
    let mut payload = vec![0u8; declared as usize];
    reader.read_exact(&mut payload)?;
    Ok(Some(payload))
}

/// Encode a frame to a `Vec`, for tests and for the client's own request builder.
pub fn encode_frame(payload: &[u8], limits: FrameLimits) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(LEN_PREFIX + payload.len());
    write_frame(&mut out, payload, limits)?;
    Ok(out)
}

pub fn decode_frame(bytes: &[u8], limits: FrameLimits) -> Result<Vec<u8>> {
    let mut cursor = std::io::Cursor::new(bytes);
    match read_frame(&mut cursor, limits)? {
        Some(payload) => Ok(payload),
        None => Err(Error::Protocol("empty frame buffer".to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let limits = FrameLimits::default();
        let payload = br#"{"method":"hello"}"#;
        let encoded = encode_frame(payload, limits).unwrap();
        assert_eq!(decode_frame(&encoded, limits).unwrap(), payload.to_vec());
    }

    #[test]
    fn multi_frame_stream_roundtrip() {
        let limits = FrameLimits::default();
        let mut stream = Vec::new();
        write_frame(&mut stream, b"one", limits).unwrap();
        write_frame(&mut stream, b"two", limits).unwrap();
        let mut cursor = std::io::Cursor::new(stream);
        assert_eq!(read_frame(&mut cursor, limits).unwrap().unwrap(), b"one");
        assert_eq!(read_frame(&mut cursor, limits).unwrap().unwrap(), b"two");
        assert!(read_frame(&mut cursor, limits).unwrap().is_none());
    }

    #[test]
    fn oversize_is_rejected_before_reading_the_body() {
        let limits = FrameLimits { max_frame_bytes: 8 };
        let mut header_only = Vec::new();
        header_only.extend_from_slice(&1_000u32.to_be_bytes());
        let mut cursor = std::io::Cursor::new(header_only);
        match read_frame(&mut cursor, limits) {
            Err(Error::Protocol(message)) => assert!(message.contains("exceeds")),
            other => panic!("expected a size rejection, got {other:?}"),
        }
    }

    #[test]
    fn write_refuses_oversize_and_empty() {
        let limits = FrameLimits { max_frame_bytes: 4 };
        let mut sink = Vec::new();
        assert!(write_frame(&mut sink, b"12345", limits).is_err());
        assert!(write_frame(&mut sink, b"", limits).is_err());
        assert!(write_frame(&mut sink, b"1234", limits).is_ok());
    }

    #[test]
    fn zero_length_frame_is_an_error_not_a_close() {
        let limits = FrameLimits::default();
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0u32.to_be_bytes());
        let mut cursor = std::io::Cursor::new(bytes);
        assert!(read_frame(&mut cursor, limits).is_err());
    }

    #[test]
    fn truncated_body_is_an_error() {
        let limits = FrameLimits::default();
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&10u32.to_be_bytes());
        bytes.extend_from_slice(b"short");
        let mut cursor = std::io::Cursor::new(bytes);
        assert!(read_frame(&mut cursor, limits).is_err());
    }
}
