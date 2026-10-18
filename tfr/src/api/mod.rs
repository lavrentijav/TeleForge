//! Client ↔ agent API.
//!
//! Transport stack, outermost first:
//!
//! ```text
//! SSH channel (the client's own credentials)        ← external, never seen here
//!   TLS 1.3 (self-signed listener cert, pinned by the client during install)
//!     mTLS client cert signed by the agent's per-host CA   [optional but default]
//!       JSON-RPC 2.0 envelope over length-prefixed frames
//!         bearer device token + request nonce/counter      [auth.rs]
//!           per-tenant AEAD on every value payload         [crypto::aead]
//! ```
//!
//! The envelope is JSON because the desktop client already speaks it in three
//! places (`TFMETA` manifests in `teleforge_sync_transport.cpp`, the plugin API in
//! `plugin_runner.cpp`, and `PersonalityCore` serialisation), so there is no new
//! format to debug. Protobuf is defined as the v2 option precisely because it is
//! *not* needed yet; see `docs/architecture.md`, "почему не gRPC в v1".

pub mod base64;
pub mod framing;
pub mod http;
pub mod pem;
pub mod protocol;
pub mod rpc;
pub mod server;
pub mod signals;
pub mod sync;
pub mod tls;
