//! A deliberately small HTTP/1.1 server-side parser and response writer.
//!
//! # Why HTTP and not raw frames
//!
//! `docs/architecture.md` § 9.1 fixed the envelope as JSON and the framing as
//! "length before the frame". A listener that only the agent's own client talks to
//! could have used that framing over TLS directly. HTTP was chosen anyway for two
//! practical reasons, and this module is the price:
//!
//! 1. **The SSH channel is not always a plain stream.** The client reaches the
//!    agent through `ssh -L` (a TCP forward) *or* through a unix socket. An HTTP
//!    request is also the one shape every tool can replay (`curl --insecure`),
//!    which is what makes a support case "the agent answers 401" instead of "the
//!    agent doesn't answer".
//! 2. **Status codes carry the failure class** the task requires (401/403/409/
//!    429/400) *before* the JSON body is parsed by the client, so a proxy or a
//!    buggy client that ignores bodies still shows the right thing.
//!
//! What is deliberately **not** implemented: keep-alive, chunked transfer
//! encoding, compression, `Expect: 100-continue`, multipart, and HTTP/2. Each one
//! is a parser surface for a peer that is already authenticated by TLS + token,
//! and none is needed: `Connection: close` costs one TCP handshake per batch, and
//! the client batches (2000 events per push) rather than trickling.
//!
//! # Bounds are checked before allocation
//!
//! `docs/architecture.md` § 9.1 requires the size limit to be enforced *before*
//! reading the body, so an unauthenticated peer cannot make the agent allocate
//! memory. Here the `Content-Length` is validated against
//! [`HttpLimits::max_body_bytes`] first, and only then is the buffer allocated.
//! A missing `Content-Length` is a 411-class rejection, not "read until close":
//! reading until close is exactly the unbounded allocation this is avoiding.

use std::collections::BTreeMap;
use std::io::{BufRead, Write};

use crate::error::{Error, Result};

/// Hard bounds for one request. All of them are enforced while parsing, not after.
#[derive(Clone, Copy, Debug)]
pub struct HttpLimits {
    /// Largest accepted request body. `sync.push` of 2000 events with 256 KiB
    /// values would be far larger, which is why the batch limit and this limit are
    /// configured together: the agent tells the client how much one request may
    /// cost, and the client pages accordingly.
    pub max_body_bytes: usize,
    /// Longest accepted request line / header line.
    pub max_line_bytes: usize,
    /// Largest number of headers. Bounds the map and the parse loop.
    pub max_headers: usize,
}

impl Default for HttpLimits {
    fn default() -> Self {
        Self {
            // 8 MiB: a 2000-event batch of small values fits, a memcpy of the
            // whole archive does not.
            max_body_bytes: 8 << 20,
            max_line_bytes: 8 << 10,
            max_headers: 64,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HttpRequest {
    pub method: String,
    /// Path plus query, verbatim. The agent accepts `/` and `/rpc`; anything else
    /// is a 404, because a second endpoint would be a second thing to secure.
    pub path: String,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}

impl HttpRequest {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(|v| v.as_str())
    }

    pub fn is_post(&self) -> bool {
        self.method == "POST"
    }

    /// The bearer token, if the header is present and well-formed.
    pub fn bearer_token(&self) -> Option<&str> {
        let value = self.header("authorization")?;
        let (scheme, token) = value.split_once(' ')?;
        if !scheme.eq_ignore_ascii_case("bearer") {
            return None;
        }
        let token = token.trim();
        if token.is_empty() {
            None
        } else {
            Some(token)
        }
    }
}

/// Why a request could not be read. Separated from [`Error`] because each variant
/// maps to a different HTTP status *and* the connection must be answered (or
/// dropped) differently.
#[derive(Debug)]
pub enum RequestError {
    /// Peer closed before sending anything: not an error, just a closed connection.
    Closed,
    /// Declared body exceeds the configured limit → 413.
    TooLarge(String),
    /// Malformed request line, headers, or body framing → 400.
    Malformed(String),
    Io(std::io::Error),
}

impl RequestError {
    pub fn status(&self) -> Option<u16> {
        match self {
            Self::Closed => None,
            Self::TooLarge(_) => Some(413),
            Self::Malformed(_) => Some(400),
            Self::Io(_) => Some(400),
        }
    }

    pub fn message(&self) -> String {
        match self {
            Self::Closed => "connection closed".to_string(),
            Self::TooLarge(message) | Self::Malformed(message) => message.clone(),
            Self::Io(error) => format!("cannot read the request: {error}"),
        }
    }
}

pub struct HttpResponse {
    pub status: u16,
    pub body: String,
}

impl HttpResponse {
    pub fn json(status: u16, body: impl Into<String>) -> Self {
        Self {
            status,
            body: body.into(),
        }
    }

    /// A plain-text error for failures that happen before the JSON envelope exists
    /// (a malformed request line, a body that is over the limit).
    pub fn text(status: u16, message: &str) -> Self {
        Self {
            status,
            body: message.to_string(),
        }
    }
}

pub fn status_text(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        411 => "Length Required",
        413 => "Payload Too Large",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "Unknown",
    }
}

/// Read one request. `Ok(None)` means the peer closed cleanly before sending a
/// request line, which is how `Connection: close` connections normally end.
pub fn read_request<R: BufRead>(
    reader: &mut R,
    limits: &HttpLimits,
) -> std::result::Result<Option<HttpRequest>, RequestError> {
    let Some(line) = read_line(reader, limits.max_line_bytes)? else {
        return Ok(None);
    };
    if line.is_empty() {
        return Err(RequestError::Malformed(
            "empty request line (a stray CRLF is not a request)".to_string(),
        ));
    }
    let (method, path) = parse_request_line(&line)?;

    let mut headers = BTreeMap::new();
    loop {
        let Some(header) = read_line(reader, limits.max_line_bytes)? else {
            return Err(RequestError::Malformed(
                "connection ended in the header block".to_string(),
            ));
        };
        if header.is_empty() {
            break;
        }
        if headers.len() >= limits.max_headers {
            return Err(RequestError::Malformed(format!(
                "more than {} headers",
                limits.max_headers
            )));
        }
        let (name, value) = header
            .split_once(':')
            .ok_or_else(|| RequestError::Malformed("header without a colon".to_string()))?;
        let name = name.trim().to_ascii_lowercase();
        if name.is_empty() {
            return Err(RequestError::Malformed("empty header name".to_string()));
        }
        // First occurrence wins: a duplicate `Content-Length` is a request
        // smuggling classic, and the safe reading is "one request, one length".
        headers
            .entry(name)
            .or_insert_with(|| value.trim().to_string());
    }

    if headers.contains_key("transfer-encoding") {
        return Err(RequestError::Malformed(
            "Transfer-Encoding is not supported; send Content-Length".to_string(),
        ));
    }

    let declared = match headers.get("content-length") {
        Some(text) => text.parse::<usize>().map_err(|_| {
            RequestError::Malformed(format!("Content-Length {text:?} is not a length"))
        })?,
        None => 0,
    };
    if declared > limits.max_body_bytes {
        return Err(RequestError::TooLarge(format!(
            "declared body of {declared} bytes exceeds the {} byte limit",
            limits.max_body_bytes
        )));
    }
    if declared > 0 && method != "POST" {
        return Err(RequestError::Malformed(
            "only POST may carry a body".to_string(),
        ));
    }

    let mut body = vec![0u8; declared];
    if declared > 0 {
        reader.read_exact(&mut body).map_err(RequestError::Io)?;
    }

    Ok(Some(HttpRequest {
        method,
        path,
        headers,
        body,
    }))
}

fn parse_request_line(line: &str) -> std::result::Result<(String, String), RequestError> {
    let mut parts = line.split(' ');
    let method = parts
        .next()
        .ok_or_else(|| RequestError::Malformed("request line has no method".to_string()))?;
    let path = parts
        .next()
        .ok_or_else(|| RequestError::Malformed("request line has no path".to_string()))?;
    let version = parts
        .next()
        .ok_or_else(|| RequestError::Malformed("request line has no version".to_string()))?;
    if parts.next().is_some() {
        return Err(RequestError::Malformed(
            "request line has extra fields".to_string(),
        ));
    }
    if version != "HTTP/1.1" && version != "HTTP/1.0" {
        return Err(RequestError::Malformed(format!(
            "unsupported version {version:?}; this listener speaks HTTP/1.1"
        )));
    }
    if !matches!(method, "POST" | "GET") {
        return Err(RequestError::Malformed(format!(
            "method {method:?} is not supported"
        )));
    }
    if !path.starts_with('/') {
        return Err(RequestError::Malformed(
            "request path must start with '/'".to_string(),
        ));
    }
    Ok((method.to_string(), path.to_string()))
}

/// Read one CRLF/LF-terminated line with a hard byte limit.
///
/// A line longer than `limit` is rejected *while* reading, so a peer that never
/// sends a newline cannot make the buffer grow. `read_until` alone would do exactly
/// that, which is why this loops over single bytes instead.
fn read_line<R: BufRead>(
    reader: &mut R,
    limit: usize,
) -> std::result::Result<Option<String>, RequestError> {
    let mut line: Vec<u8> = Vec::new();
    loop {
        let mut byte = [0u8; 1];
        match reader.read(&mut byte) {
            Ok(0) => {
                if line.is_empty() {
                    return Ok(None);
                }
                return Err(RequestError::Malformed(
                    "connection ended inside a line".to_string(),
                ));
            }
            Ok(_) => {
                if byte[0] == b'\n' {
                    break;
                }
                line.push(byte[0]);
                if line.len() > limit {
                    return Err(RequestError::TooLarge(format!(
                        "line longer than {limit} bytes"
                    )));
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(RequestError::Io(error)),
        }
    }
    while line.last() == Some(&b'\r') {
        line.pop();
    }
    String::from_utf8(line)
        .map(Some)
        .map_err(|_| RequestError::Malformed("line is not valid UTF-8".to_string()))
}

/// Write one response. Always `Connection: close`: the agent does not keep
/// connections alive (see the module docs), and saying so lets a client reuse the
/// TLS session instead of guessing.
pub fn write_response<W: Write>(writer: &mut W, response: &HttpResponse) -> Result<()> {
    let mut head = format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json; charset=utf-8\r\n\
         Content-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        response.status,
        status_text(response.status),
        response.body.len()
    );
    head.push_str(&response.body);
    writer.write_all(head.as_bytes())?;
    writer.flush()?;
    Ok(())
}

/// Map a protocol error to the HTTP status the task requires.
///
/// This table is the contract: 401 for a bad token, 403 for someone else's
/// account, 409 for a version/cursor conflict, 429 for over the limit, 400 for a
/// body that is not a valid request.
pub fn status_for_code(code: u32) -> u16 {
    match code {
        crate::api::protocol::code::UNAUTHENTICATED => 401,
        crate::api::protocol::code::FORBIDDEN => 403,
        crate::api::protocol::code::VAULT_LOCKED => 403,
        crate::api::protocol::code::CONFLICT => 409,
        crate::api::protocol::code::RATE_LIMITED => 429,
        crate::api::protocol::code::NOT_FOUND => 404,
        crate::api::protocol::code::INVALID_REQUEST
        | crate::api::protocol::code::UNSUPPORTED_PROTOCOL => 400,
        crate::api::protocol::code::UPDATE_REJECTED => 409,
        _ => 500,
    }
}

/// Convenience: an [`Error`] that is not a protocol code but a storage/IO failure.
pub fn internal_response(error: &Error) -> HttpResponse {
    HttpResponse::text(500, &format!("internal error: {error}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn request(head: &str, body: &[u8]) -> Vec<u8> {
        let mut bytes = head.as_bytes().to_vec();
        bytes.extend_from_slice(body);
        bytes
    }

    #[test]
    fn a_minimal_post_roundtrips() {
        let limits = HttpLimits::default();
        let body = br#"{"method":"hello"}"#;
        let head = "POST /rpc HTTP/1.1\r\nHost: localhost\r\nContent-Length: 18\r\n\
                    Authorization: Bearer abc\r\n\r\n";
        let raw = request(head, body);
        let parsed = read_request(&mut Cursor::new(raw), &limits)
            .unwrap()
            .expect("a request");
        assert!(parsed.is_post());
        assert_eq!(parsed.path, "/rpc");
        assert_eq!(parsed.body, body.to_vec());
        assert_eq!(parsed.bearer_token(), Some("abc"));
        assert_eq!(parsed.header("HOST"), Some("localhost"));
    }

    #[test]
    fn a_closed_connection_is_not_an_error() {
        let limits = HttpLimits::default();
        assert!(read_request(&mut Cursor::new(Vec::new()), &limits)
            .unwrap()
            .is_none());
    }

    #[test]
    fn an_oversized_declared_body_is_rejected_before_reading_it() {
        let limits = HttpLimits {
            max_body_bytes: 8,
            ..HttpLimits::default()
        };
        let raw = request("POST / HTTP/1.1\r\nContent-Length: 100000\r\n\r\n", b"");
        let error = read_request(&mut Cursor::new(raw), &limits).unwrap_err();
        assert_eq!(error.status(), Some(413));
        assert!(error.message().contains("exceeds"));
    }

    #[test]
    fn chunked_encoding_is_refused() {
        let limits = HttpLimits::default();
        let raw = request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", b"");
        let error = read_request(&mut Cursor::new(raw), &limits).unwrap_err();
        assert_eq!(error.status(), Some(400));
    }

    #[test]
    fn a_missing_content_length_means_an_empty_body() {
        let limits = HttpLimits::default();
        let raw = request("GET / HTTP/1.1\r\nHost: x\r\n\r\n", b"");
        let parsed = read_request(&mut Cursor::new(raw), &limits)
            .unwrap()
            .expect("a request");
        assert!(parsed.body.is_empty());
        assert!(!parsed.is_post());
    }

    #[test]
    fn garbage_request_lines_are_rejected() {
        let limits = HttpLimits::default();
        for head in [
            "FROB / HTTP/1.1\r\n\r\n",
            "POST // HTTP/9.9\r\n\r\n",
            "POST / HTTP/1.1 extra\r\n\r\n",
        ] {
            assert!(
                read_request(&mut Cursor::new(request(head, b"")), &limits).is_err(),
                "{head:?} must not parse"
            );
        }
    }

    #[test]
    fn a_too_long_header_line_is_rejected_while_reading() {
        let limits = HttpLimits {
            max_line_bytes: 16,
            ..HttpLimits::default()
        };
        let mut raw = b"POST / HTTP/1.1\r\nX-Long: ".to_vec();
        raw.extend(std::iter::repeat_n(b'a', 100));
        raw.extend_from_slice(b"\r\n\r\n");
        let error = read_request(&mut Cursor::new(raw), &limits).unwrap_err();
        assert_eq!(error.status(), Some(413));
    }

    #[test]
    fn too_many_headers_are_rejected() {
        let limits = HttpLimits {
            max_headers: 2,
            ..HttpLimits::default()
        };
        let raw = request("POST / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", b"");
        assert!(read_request(&mut Cursor::new(raw), &limits).is_err());
    }

    #[test]
    fn only_post_may_carry_a_body() {
        let limits = HttpLimits::default();
        let raw = request("GET / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}", b"{}");
        let error = read_request(&mut Cursor::new(raw), &limits).unwrap_err();
        assert_eq!(error.status(), Some(400));
    }

    #[test]
    fn responses_carry_a_length_and_always_close() {
        let mut out = Vec::new();
        write_response(&mut out, &HttpResponse::json(200, "{\"ok\":true}")).unwrap();
        let text = String::from_utf8(out).unwrap();
        assert!(text.starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(text.contains("Content-Length: 11\r\n"));
        assert!(text.contains("Connection: close\r\n"));
        assert!(text.ends_with("{\"ok\":true}"));
    }

    #[test]
    fn status_codes_follow_the_protocol_codes() {
        use crate::api::protocol::code;
        assert_eq!(status_for_code(code::UNAUTHENTICATED), 401);
        assert_eq!(status_for_code(code::FORBIDDEN), 403);
        assert_eq!(status_for_code(code::CONFLICT), 409);
        assert_eq!(status_for_code(code::RATE_LIMITED), 429);
        assert_eq!(status_for_code(code::INVALID_REQUEST), 400);
        assert_eq!(status_for_code(code::INTERNAL), 500);
    }
}
