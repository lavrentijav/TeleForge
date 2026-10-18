//! TeleForge server-side agent.
//!
//! The agent is deployed *by the desktop client* onto the user's own server:
//! the client uploads a musl-static binary over the SSH access it already has,
//! installs a user-level systemd unit, and then talks to the agent exclusively
//! through a loopback listener or a unix socket reached over an SSH tunnel.
//! Nothing in this crate ever opens a public port on its own initiative.
//!
//! Crate policy:
//! * `unsafe` is confined to [`secret`] (volatile wipe) and [`tdlib`] (C ABI).
//! * No secret is ever formatted with `Debug`/`Display`; see [`logging`].
//! * Dependencies are pulled in *with* the code that uses them, and are limited
//!   to what a primitive needs: `rusqlite` (bundled SQLite) for [`store`],
//!   `argon2`/`chacha20poly1305`/`aes-gcm`/`hkdf`/`sha2`/`rand`/`zeroize` for
//!   [`crypto`] and [`keystore`], `postgres`/`mysql` (+ rustls via `ring`) for the
//!   server-side DB [`db`] ingest. Everything else still carries a
//!   `TODO(<crate>)` and returns [`error::Error::NotImplemented`].
//!   `cargo check --offline` works against the vendored lockfile.

pub mod api;
pub mod auth;
pub mod cli;
pub mod config;
pub mod crypto;
// Reading the user's PostgreSQL/MySQL and publishing it into the journal
// (`docs/db-ingest.md`): the pull-first half of the sync.
pub mod db;
pub mod error;
pub mod fsperm;
pub mod keystore;
pub mod logging;
pub mod secret;
pub mod selftest;
pub mod store;
pub mod supervisor;
pub mod sync;
pub mod tdlib;
#[cfg(test)]
mod test_support;
pub mod tunnel;
pub mod updater;
pub mod vault;

/// Wire/format revision of the *agent* build, independent of any client build.
pub const AGENT_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Oldest client the agent refuses to serve, checked in `api::server`.
/// The client enforces the mirror-image field, `manifest.min_client_version`.
pub const MIN_CLIENT_VERSION: &str = "0.1.0";

/// Bumped whenever the on-disk layout under `--data-dir` changes incompatibly.
pub const STORAGE_FORMAT_VERSION: u32 = 1;

/// Bumped whenever the client<->agent message shapes change incompatibly.
pub const PROTOCOL_VERSION: u16 = 1;
