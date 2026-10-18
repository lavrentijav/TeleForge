//! Sync engine.
//!
//! # What it replaces
//!
//! Today `TeleForge::Sync` replicates by taking a whole-database snapshot
//! (`CreateDatabaseSnapshot` → `VACUUM INTO`), compressing it
//! (`CompressSnapshot`), encrypting it (`EncryptSnapshot`) and uploading it as a
//! Telegram document (`UploadEncryptedDocument`) or as rows into
//! `teleforge_messages` (`teleforge_sync_pg.cpp` / `teleforge_sync_mysql.cpp`).
//! Both devices then merge snapshots with `MergeDataDb`.
//!
//! Four properties of that design are unfixable, which is why the agent replaces
//! it rather than porting it:
//!
//! 1. **It copies secrets.** `VACUUM INTO` copies `PersonalityCore`, i.e.
//!    `openAiApiKey`, `pgConnString` (with the DB password) and the SSH target
//!    into whatever the cloud is (audit C2). Whitelisting is a patch; a logical
//!    change feed that never contains those rows is a fix.
//! 2. **It transfers local autoincrement ids.** `MergeDataDb` matches
//!    `MemoryItem.id = r.id`, and ids are per-device, so two devices overwrite
//!    each other's facts (H7).
//! 3. **It resolves conflicts by whole-row "latest second wins"** with `>=`, so
//!    two edits in the same second lose one, and a remote row that differs only
//!    in `updatedAt` replaces locally-edited fields (H7).
//! 4. **It is not a log.** `teleforge_events` is keyed
//!    `(tenant_id, content_hash, event_type)`, so the second edit of a message
//!    cannot be stored (H9).
//!
//! # Model
//!
//! An **entity** is addressed by a deterministic key, never by a local id:
//!
//! ```text
//! EntityKey = { kind, account_id: [16], dialog_id: i64, message_id: i32, sub: u8 }
//! ```
//!
//! An **entity** has one row in `entity_root` holding its causality metadata and
//! one row per changed **field** in `entity_field`. Field ids are stable integers
//! (`FieldId`), because a field name in the clear would leak more than it needs
//! to; the enum is the only place they are defined.
//!
//! Ordering uses two clocks:
//!
//! * a **hybrid logical clock** (`Hlc`) per write — physical milliseconds plus a
//!   logical counter, so a skewed VPS clock cannot reorder writes that are
//!   causally ordered;
//! * a **vector clock** (`VectorClock`) per entity — the only thing that can tell
//!   "concurrent" from "newer".
//!
//! Conflict policy:
//!
//! * `CausalOrder::After` (local is behind) → apply.
//! * `CausalOrder::Before` (local is ahead) → ignore, report `Ignored`.
//! * `CausalOrder::Equal` → duplicate, report `Duplicate` (idempotency).
//! * `CausalOrder::Concurrent` → **never silently pick a winner**. Different
//!   fields merge cleanly; the same field produces a `Conflict` row that the
//!   client surfaces to the user. That is the difference from H7's `INSERT OR
//!   REPLACE`.
//!
//! Values are opaque ciphertext (`crypto::aead`) sealed with
//! `subkey(DEK, LABEL_SYNC_DATA)`; metadata (kind, key, field id, clock,
//! tombstone) is cleartext so the agent can order writes without reading them.

pub mod clock;
pub mod cursor;
pub mod engine;
pub mod model;
pub mod vclock;
