//! Base64 (RFC 4648, standard alphabet, `=` padding).
//!
//! # Why hand-written
//!
//! The request body carries opaque `value` bytes (on the client: an AEAD
//! envelope). They cannot go into JSON as raw bytes, and hex would double the
//! wire size of every captured message (a 256 KiB value becomes 512 KiB of text,
//! which is the difference between one frame and two). Base64 is the smallest
//! dependency-free encoding that JSON can carry, and it is ~60 lines of well
//! understood code with an exhaustive decoder test rather than a crate.
//!
//! # Strictness
//!
//! The decoder is deliberately *not* lenient. It rejects:
//! * any character outside the alphabet (including whitespace and newlines) — a
//!   lenient decoder turns "garbage after the payload" into "still valid";
//! * a length that is not a multiple of four;
//! * `=` anywhere but the last one or two positions;
//! * non-zero trailing bits in the final quantum, so there is exactly one
//!   encoding for a given byte string.
//!
//! That matters because `value` bytes are compared by `content_hash`: if two
//! spellings of the same bytes were accepted, a replay would look like a new
//! observation and idempotency would silently break.

use crate::error::{Error, Result};

const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#[inline]
fn value_of(byte: u8) -> Option<u8> {
    match byte {
        b'A'..=b'Z' => Some(byte - b'A'),
        b'a'..=b'z' => Some(byte - b'a' + 26),
        b'0'..=b'9' => Some(byte - b'0' + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        _ => None,
    }
}

pub fn encode(input: &[u8]) -> String {
    let mut out = String::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let first = u32::from(chunk[0]);
        let second = u32::from(*chunk.get(1).unwrap_or(&0));
        let third = u32::from(*chunk.get(2).unwrap_or(&0));
        let triple = (first << 16) | (second << 8) | third;
        out.push(ALPHABET[((triple >> 18) & 0x3f) as usize] as char);
        out.push(ALPHABET[((triple >> 12) & 0x3f) as usize] as char);
        if chunk.len() > 1 {
            out.push(ALPHABET[((triple >> 6) & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(ALPHABET[(triple & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
    }
    out
}

pub fn decode(text: &str) -> Result<Vec<u8>> {
    let bytes = text.as_bytes();
    if bytes.len() % 4 != 0 {
        return Err(Error::Protocol(format!(
            "base64 length {} is not a multiple of 4",
            bytes.len()
        )));
    }
    let mut out = Vec::with_capacity(bytes.len() / 4 * 3);
    let quads = bytes.len() / 4;
    for (index, quad) in bytes.chunks(4).enumerate() {
        let last = index + 1 == quads;
        let mut values = [0u8; 4];
        let mut padding = 0usize;
        for (slot, byte) in quad.iter().enumerate() {
            if *byte == b'=' {
                // Padding is only allowed in the final quantum, and only in the
                // last two slots.
                if !last || slot < 2 {
                    return Err(Error::Protocol(
                        "base64 padding in the middle of the stream".to_string(),
                    ));
                }
                padding += 1;
                continue;
            }
            if padding > 0 {
                return Err(Error::Protocol("base64 data after padding".to_string()));
            }
            values[slot] = value_of(*byte).ok_or_else(|| {
                Error::Protocol(format!("base64 byte 0x{byte:02x} is outside the alphabet"))
            })?;
        }
        if padding > 2 {
            return Err(Error::Protocol("base64 has too much padding".to_string()));
        }
        // Canonical form: the bits the padding declares unused must be zero, so
        // there is exactly one encoding per byte string.
        if padding == 2 && values[1] & 0x0f != 0 {
            return Err(Error::Protocol(
                "base64 has non-zero trailing bits".to_string(),
            ));
        }
        if padding == 1 && values[2] & 0x03 != 0 {
            return Err(Error::Protocol(
                "base64 has non-zero trailing bits".to_string(),
            ));
        }
        let triple = (u32::from(values[0]) << 18)
            | (u32::from(values[1]) << 12)
            | (u32::from(values[2]) << 6)
            | u32::from(values[3]);
        out.push((triple >> 16) as u8);
        if padding < 2 {
            out.push((triple >> 8) as u8);
        }
        if padding < 1 {
            out.push(triple as u8);
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rfc4648_vectors_roundtrip() {
        for (plain, encoded) in [
            ("", ""),
            ("f", "Zg=="),
            ("fo", "Zm8="),
            ("foo", "Zm9v"),
            ("foob", "Zm9vYg=="),
            ("fooba", "Zm9vYmE="),
            ("foobar", "Zm9vYmFy"),
        ] {
            assert_eq!(encode(plain.as_bytes()), encoded);
            assert_eq!(decode(encoded).unwrap(), plain.as_bytes());
        }
    }

    #[test]
    fn every_byte_value_roundtrips() {
        let all: Vec<u8> = (0..=255u8).collect();
        assert_eq!(decode(&encode(&all)).unwrap(), all);
        for len in 0..32usize {
            let slice: Vec<u8> = (0..len).map(|value| value as u8).collect();
            assert_eq!(decode(&encode(&slice)).unwrap(), slice);
        }
    }

    #[test]
    fn whitespace_and_foreign_characters_are_rejected() {
        // A lenient decoder would accept these; then two spellings of the same
        // bytes would produce the same `value` but not the same `content_hash`.
        assert!(decode("Zm9v\n").is_err());
        assert!(decode("Zm 9v").is_err());
        assert!(decode("Zm9v-").is_err());
        assert!(decode("Zm9v!").is_err());
    }

    #[test]
    fn padding_rules_are_strict() {
        assert!(decode("Zg=").is_err(), "length not a multiple of four");
        assert!(decode("Zg===").is_err());
        assert!(decode("Z===").is_err());
        assert!(decode("Zg==Zg==").is_err(), "padding not only at the end");
        assert!(decode("Z=g=").is_err());
        assert!(decode("=Zg=").is_err());
        // Non-canonical trailing bits: "Zh==" decodes to the same 'f' only if the
        // unused bits are ignored.
        assert!(decode("Zh==").is_err());
    }

    #[test]
    fn binary_values_with_nulls_survive() {
        let value = vec![0u8, 0, 0, 255, 254, 0, 1];
        assert_eq!(decode(&encode(&value)).unwrap(), value);
    }
}
