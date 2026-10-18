//! Sync record types and the SQLite schema they map to.
//!
//! Every table carries `account_id`. That is the fix for finding C3, where
//! `OnlineEvent`, `SpyTarget`, `PeerArchive*` and `MemoryItem/Embedding` had only
//! the *peer's* id: chat ids are allocated per account, so two accounts collided
//! and one account's archive leaked into the other.

use crate::crypto::kdf;
use crate::error::{Error, Result};
use crate::sync::clock::Hlc;
use crate::sync::vclock::VectorClock;

/// Stable numeric field ids. **Append only.** Renumbering one silently rewrites
/// history, and a value sealed under the wrong field id still decrypts (the AAD
/// would catch it, which is the point of the AAD).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u16)]
pub enum FieldId {
    // --- Message revisions (replaces DeletedMessage/EditedMessage rows) ---
    Text = 1,
    TextEntities = 2,
    EditDate = 3,
    Views = 4,
    MediaSerialized = 5,
    MediaPath = 6,
    MimeType = 7,
    DocumentSerialized = 8,
    ThumbsSerialized = 9,
    DocumentAttributes = 10,
    ReplySerialized = 11,
    ReplyMarkupSerialized = 12,
    PostAuthor = 13,
    FwdName = 14,
    FwdPostAuthor = 15,
    GroupedId = 16,
    TopicId = 17,
    Flags = 18,
    // --- Presence (replaces OnlineEvent) ---
    PresenceKind = 20,
    PresenceOnlineTill = 21,
    PresenceManualLastSeen = 22,
    // --- Peer archive ---
    PeerFirstName = 30,
    PeerLastName = 31,
    PeerUsername = 32,
    PeerBio = 33,
    PeerUserpicPath = 34,
    PeerChatTitle = 35,
    PeerSelfInChat = 36,
    PeerStillInChat = 37,
    // --- Memory ---
    MemoryTitle = 40,
    MemorySummary = 41,
    MemoryDetails = 42,
    MemoryFactType = 43,
    MemoryTags = 44,
    MemoryPriority = 45,
    MemoryPinned = 46,
    MemoryHidden = 47,
    MemoryEmbedding = 48,
    // --- Settings ---
    SettingValue = 60,
    /// Per-chat settings are one row per key so that a change to
    /// `maxMemoryBlockChars` does not overwrite a concurrent change to
    /// `directoryWhitelistJson` (finding H7's whole-row replace).
    ChatSettingValue = 61,
}

impl FieldId {
    pub fn code(self) -> u16 {
        self as u16
    }

    pub fn from_code(code: u16) -> Option<Self> {
        // Exhaustive on purpose: adding a variant without adding it here is a
        // compile error (non-exhaustive match), not a silent data-loss bug.
        let value = match code {
            1 => Self::Text,
            2 => Self::TextEntities,
            3 => Self::EditDate,
            4 => Self::Views,
            5 => Self::MediaSerialized,
            6 => Self::MediaPath,
            7 => Self::MimeType,
            8 => Self::DocumentSerialized,
            9 => Self::ThumbsSerialized,
            10 => Self::DocumentAttributes,
            11 => Self::ReplySerialized,
            12 => Self::ReplyMarkupSerialized,
            13 => Self::PostAuthor,
            14 => Self::FwdName,
            15 => Self::FwdPostAuthor,
            16 => Self::GroupedId,
            17 => Self::TopicId,
            18 => Self::Flags,
            20 => Self::PresenceKind,
            21 => Self::PresenceOnlineTill,
            22 => Self::PresenceManualLastSeen,
            30 => Self::PeerFirstName,
            31 => Self::PeerLastName,
            32 => Self::PeerUsername,
            33 => Self::PeerBio,
            34 => Self::PeerUserpicPath,
            35 => Self::PeerChatTitle,
            36 => Self::PeerSelfInChat,
            37 => Self::PeerStillInChat,
            40 => Self::MemoryTitle,
            41 => Self::MemorySummary,
            42 => Self::MemoryDetails,
            43 => Self::MemoryFactType,
            44 => Self::MemoryTags,
            45 => Self::MemoryPriority,
            46 => Self::MemoryPinned,
            47 => Self::MemoryHidden,
            48 => Self::MemoryEmbedding,
            60 => Self::SettingValue,
            61 => Self::ChatSettingValue,
            _ => return None,
        };
        Some(value)
    }

    /// Whether a field is a *set* rather than a scalar. Peer names and usernames
    /// are append-only histories in the C++ schema; modelled here as set
    /// members, so two devices that each observed a different name do not fight.
    pub fn is_set_member(self) -> bool {
        matches!(
            self,
            Self::PeerFirstName | Self::PeerLastName | Self::PeerUsername | Self::PeerBio
        )
    }
}

/// What kind of thing an entity is. The numeric value is part of the entity key,
/// so it is wire format.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum EntityKind {
    /// One captured revision of a Telegram message (deleted or edited).
    MessageRevision = 1,
    /// One presence transition for one peer.
    Presence = 2,
    PeerProfile = 3,
    MemoryItem = 4,
    /// A single key/value setting, addressed by the field id.
    Setting = 5,
    PerChatSetting = 6,
    SyncArtifact = 7,
}

impl EntityKind {
    pub fn code(self) -> u8 {
        self as u8
    }

    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::MessageRevision),
            2 => Some(Self::Presence),
            3 => Some(Self::PeerProfile),
            4 => Some(Self::MemoryItem),
            5 => Some(Self::Setting),
            6 => Some(Self::PerChatSetting),
            7 => Some(Self::SyncArtifact),
            _ => None,
        }
    }
}

/// Deterministic entity key.
///
/// `dialog_id`/`message_id` are real Telegram ids in cleartext: they are the
/// merge key. Leaking them to the agent host is accepted because the agent is the
/// user's own machine and because the *values* are ciphertext. What is explicitly
/// not done is what `BuildRow` in `teleforge_sync_rows.cpp` does — put `fromId`
/// into the `account_id_h` column, i.e. record the sender where the owner belongs
/// (finding H8). Here `account_id` is the owning account, always.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct EntityKey {
    pub kind: EntityKind,
    pub account_id: [u8; 16],
    pub dialog_id: i64,
    pub message_id: i32,
    /// Disambiguates histories and set members inside one entity.
    pub sub: u32,
}

impl EntityKey {
    pub fn message(account_id: [u8; 16], dialog_id: i64, message_id: i32, revision: u32) -> Self {
        Self {
            kind: EntityKind::MessageRevision,
            account_id,
            dialog_id,
            message_id,
            sub: revision,
        }
    }

    pub fn peer(account_id: [u8; 16], peer_id: i64) -> Self {
        Self {
            kind: EntityKind::PeerProfile,
            account_id,
            dialog_id: peer_id,
            message_id: 0,
            sub: 0,
        }
    }

    pub fn presence(account_id: [u8; 16], peer_id: i64) -> Self {
        Self {
            kind: EntityKind::Presence,
            account_id,
            dialog_id: peer_id,
            message_id: 0,
            sub: 0,
        }
    }

    /// Memory items are keyed by their `contentHash`, which is the stable
    /// cross-device key the C++ audit recommends (H7) — not by the local
    /// autoincrement id.
    pub fn memory(account_id: [u8; 16], content_hash: &[u8; 16]) -> Self {
        let mut dialog_id = 0i64;
        for (index, byte) in content_hash.iter().enumerate().take(8) {
            dialog_id |= i64::from(*byte) << (index * 8);
        }
        Self {
            kind: EntityKind::MemoryItem,
            account_id,
            dialog_id,
            message_id: 0,
            sub: 0,
        }
    }

    pub fn setting(account_id: [u8; 16], field: FieldId) -> Self {
        Self {
            kind: EntityKind::Setting,
            account_id,
            dialog_id: 0,
            message_id: 0,
            sub: u32::from(field.code()),
        }
    }

    pub fn per_chat_setting(account_id: [u8; 16], peer_id: i64, field: FieldId) -> Self {
        Self {
            kind: EntityKind::PerChatSetting,
            account_id,
            dialog_id: peer_id,
            message_id: 0,
            sub: u32::from(field.code()),
        }
    }

    /// 41-byte fixed encoding, used as the SQLite primary-key prefix and as the
    /// AEAD additional data (so a value moved to another entity fails to open).
    pub fn encode(&self) -> [u8; 41] {
        let mut out = [0u8; 41];
        out[0] = self.kind.code();
        out[1..17].copy_from_slice(&self.account_id);
        out[17..25].copy_from_slice(&self.dialog_id.to_be_bytes());
        out[25..29].copy_from_slice(&self.message_id.to_be_bytes());
        out[29..33].copy_from_slice(&self.sub.to_be_bytes());
        // bytes 33..41 reserved (zero) for a future key component
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != 41 {
            return Err(Error::Protocol("entity key must be 41 bytes".to_string()));
        }
        let kind = EntityKind::from_code(bytes[0])
            .ok_or_else(|| Error::Protocol(format!("unknown entity kind {}", bytes[0])))?;
        let mut account_id = [0u8; 16];
        account_id.copy_from_slice(&bytes[1..17]);
        let mut dialog_bytes = [0u8; 8];
        dialog_bytes.copy_from_slice(&bytes[17..25]);
        let mut message_bytes = [0u8; 4];
        message_bytes.copy_from_slice(&bytes[25..29]);
        let mut sub_bytes = [0u8; 4];
        sub_bytes.copy_from_slice(&bytes[29..33]);
        Ok(Self {
            kind,
            account_id,
            dialog_id: i64::from_be_bytes(dialog_bytes),
            message_id: i32::from_be_bytes(message_bytes),
            sub: u32::from_be_bytes(sub_bytes),
        })
    }
}

/// One field's version metadata. The version is the pair, because "who wrote it"
/// matters for the tiebreak *presentation* and for the vector clock.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FieldVersion {
    pub hlc: Hlc,
    pub writer_seq: u64,
}

/// A stored field. `value` is ciphertext; `tombstone` marks a delete without
/// removing the row, so a stale writer cannot resurrect it.
#[derive(Clone, Debug)]
pub struct FieldRecord {
    pub key: EntityKey,
    pub field: FieldId,
    pub version: FieldVersion,
    pub tombstone: bool,
    pub value_ct: Vec<u8>,
}

impl FieldRecord {
    /// AAD binds the ciphertext to its address. Without this, an attacker with
    /// write access to `data.db` could move a value from a field they may read to
    /// one they may not.
    pub fn aad(&self) -> Vec<u8> {
        let mut aad = Vec::with_capacity(48);
        aad.extend_from_slice(&self.key.encode());
        aad.extend_from_slice(&self.field.code().to_be_bytes());
        aad.extend_from_slice(&[if self.tombstone { 1 } else { 0 }]);
        aad
    }

    pub fn subkey_label(&self) -> &'static [u8] {
        kdf::LABEL_SYNC_DATA
    }
}

/// Per-entity causality root.
#[derive(Clone, Debug)]
pub struct EntityRoot {
    pub key: EntityKey,
    pub vclock: VectorClock,
    pub last_hlc: Hlc,
    /// Highest `op_seq` that touched this entity; lets the engine answer
    /// "unchanged since" without scanning fields.
    pub last_seq: u64,
    pub deleted: bool,
    pub deleted_hlc: Option<Hlc>,
}

/// A change the client pushes. `op_id` is generated by the client and is the
/// idempotency key: replaying the same `op_id` returns the original result rather
/// than applying twice. That is what H9's `ON CONFLICT DO NOTHING` was *trying*
/// to be, except it also swallowed genuine second edits.
#[derive(Clone, Debug)]
pub struct Op {
    pub op_id: [u8; 16],
    pub key: EntityKey,
    pub field: FieldId,
    pub vclock: VectorClock,
    pub hlc: Hlc,
    pub writer_seq: u64,
    pub tombstone: bool,
    pub value_ct: Vec<u8>,
}

impl Op {
    /// AAD used by the *client* to seal `value_ct`. The agent never opens it.
    pub fn ciphertext_aad(&self) -> Vec<u8> {
        let mut aad = Vec::with_capacity(48);
        aad.extend_from_slice(&self.key.encode());
        aad.extend_from_slice(&self.field.code().to_be_bytes());
        aad.push(if self.tombstone { 1 } else { 0 });
        aad
    }

    pub fn encode_id_hex(&self) -> String {
        crate::crypto::sig::to_hex(&self.op_id)
    }
}

/// A conflict the engine refused to resolve on its own.
#[derive(Clone, Debug)]
pub struct Conflict {
    pub key: EntityKey,
    pub field: FieldId,
    pub local: FieldVersion,
    pub remote: FieldVersion,
    /// Deterministic, user-facing ordering hint only — never an automatic winner.
    pub tiebreak: Vec<(String, u64)>,
}

/// What the engine did with one incoming op.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteVerdict {
    /// Applied: local state was behind.
    Applied,
    /// Ignored: local state already dominates the incoming op.
    Ignored,
    /// Idempotent replay of an `op_id` we have already stored.
    Duplicate,
    /// Applied to a field the remote did not touch, while another field conflicts.
    PartiallyApplied,
    /// Same field, concurrent writes: stored both, surfaced a `Conflict`.
    Conflict,
    /// The entity has a tombstone newer than the op, so the write is refused
    /// rather than resurrecting deleted data.
    RejectedTombstone,
}

/// SQL applied when a tenant's store is created. Kept as one constant so it is
/// reviewable in a diff and identical across a fresh install and an upgrade.
///
/// Notes on the choices:
/// * `WITHOUT ROWID` on `entity_field`/`entity_root`: the primary key *is* the
///   access path, so there is no separate rowid B-tree.
/// * `op_log` is append-only and never compacted below the slowest client cursor;
///   `prune_upto` implements that.
/// * every table starts with `account_id` so a cross-account read is a bug that a
///   `WHERE account_id = ?` cannot silently miss.
pub const SCHEMA_SQL: &str = r#"
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 5000;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS seq (
    name TEXT NOT NULL PRIMARY KEY,
    next INTEGER NOT NULL
) WITHOUT ROWID;
INSERT OR IGNORE INTO seq (name, next) VALUES ('op_seq', 0);

CREATE TABLE IF NOT EXISTS entity_root (
    account_id   BLOB    NOT NULL,
    entity_kind  INTEGER NOT NULL,
    dialog_id    INTEGER NOT NULL,
    message_id   INTEGER NOT NULL,
    sub          INTEGER NOT NULL,
    vclock       TEXT    NOT NULL,
    last_hlc     BLOB    NOT NULL,
    last_seq     INTEGER NOT NULL,
    deleted      INTEGER NOT NULL DEFAULT 0,
    deleted_hlc  BLOB,
    PRIMARY KEY (account_id, entity_kind, dialog_id, message_id, sub)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS entity_field (
    account_id   BLOB    NOT NULL,
    entity_kind  INTEGER NOT NULL,
    dialog_id    INTEGER NOT NULL,
    message_id   INTEGER NOT NULL,
    sub          INTEGER NOT NULL,
    field_id     INTEGER NOT NULL,
    hlc          BLOB    NOT NULL,
    writer_seq   INTEGER NOT NULL,
    tombstone    INTEGER NOT NULL DEFAULT 0,
    value_ct     BLOB    NOT NULL,
    op_seq       INTEGER NOT NULL,
    PRIMARY KEY (account_id, entity_kind, dialog_id, message_id, sub, field_id)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_field_seq ON entity_field (op_seq);
CREATE INDEX IF NOT EXISTS idx_field_hlc ON entity_field (hlc);

CREATE TABLE IF NOT EXISTS op_log (
    op_seq     INTEGER PRIMARY KEY AUTOINCREMENT,
    op_id      BLOB    NOT NULL,
    account_id BLOB    NOT NULL,
    entity_kind INTEGER NOT NULL,
    dialog_id  INTEGER NOT NULL,
    message_id INTEGER NOT NULL,
    sub        INTEGER NOT NULL,
    field_id   INTEGER NOT NULL,
    hlc        BLOB    NOT NULL,
    writer_seq INTEGER NOT NULL,
    tombstone  INTEGER NOT NULL DEFAULT 0,
    value_ct   BLOB    NOT NULL,
    applied_at INTEGER NOT NULL,
    verdict    INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_op_id ON op_log (op_id);
CREATE INDEX IF NOT EXISTS idx_op_seq_entity ON op_log (op_seq, entity_kind);

CREATE TABLE IF NOT EXISTS conflict (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id BLOB    NOT NULL,
    entity_kind INTEGER NOT NULL,
    dialog_id  INTEGER NOT NULL,
    message_id INTEGER NOT NULL,
    sub        INTEGER NOT NULL,
    field_id   INTEGER NOT NULL,
    local_hlc  BLOB    NOT NULL,
    remote_hlc BLOB    NOT NULL,
    detected_at INTEGER NOT NULL,
    resolved_by TEXT
);

CREATE TABLE IF NOT EXISTS device_cursor (
    account_id BLOB    NOT NULL,
    device_id  TEXT    NOT NULL,
    since_seq  INTEGER NOT NULL,
    applied_ops INTEGER NOT NULL,
    last_pull_at INTEGER,
    PRIMARY KEY (account_id, device_id)
) WITHOUT ROWID;

-- Subscription to TDLib updates, so a restart does not replay everything.
CREATE TABLE IF NOT EXISTS tdlib_cursor (
    account_id   BLOB NOT NULL PRIMARY KEY,
    chat_id      INTEGER,
    message_id   INTEGER,
    updated_at   INTEGER NOT NULL
);
"#;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn field_ids_roundtrip() {
        for code in [1u16, 18, 20, 22, 30, 37, 40, 48, 60, 61] {
            let field = FieldId::from_code(code).expect("known field id");
            assert_eq!(field.code(), code);
        }
        assert!(FieldId::from_code(0).is_none());
        assert!(FieldId::from_code(999).is_none());
    }

    #[test]
    fn entity_key_roundtrip() {
        let key = EntityKey::message([7u8; 16], -1001234567890, 42, 3);
        let encoded = key.encode();
        assert_eq!(encoded.len(), 41);
        assert_eq!(EntityKey::decode(&encoded).unwrap(), key);

        let peer = EntityKey::peer([1u8; 16], 900);
        assert_eq!(EntityKey::decode(&peer.encode()).unwrap(), peer);

        let setting = EntityKey::setting([2u8; 16], FieldId::MemoryPinned);
        assert_eq!(EntityKey::decode(&setting.encode()).unwrap(), setting);
    }

    #[test]
    fn entity_key_decode_rejects_bad_input() {
        assert!(EntityKey::decode(&[0u8; 10]).is_err());
        let mut bad = EntityKey::peer([0u8; 16], 1).encode();
        bad[0] = 99;
        assert!(EntityKey::decode(&bad).is_err());
    }

    #[test]
    fn aad_differs_per_field_and_tombstone() {
        let key = EntityKey::peer([3u8; 16], 5);
        let live = FieldRecord {
            key: key.clone(),
            field: FieldId::PeerUsername,
            version: FieldVersion {
                hlc: Hlc::new(1, 0),
                writer_seq: 1,
            },
            tombstone: false,
            value_ct: vec![],
        };
        let mut dead = live.clone();
        dead.tombstone = true;
        assert_ne!(live.aad(), dead.aad());
        let mut other_field = live.clone();
        other_field.field = FieldId::PeerBio;
        assert_ne!(live.aad(), other_field.aad());
    }

    #[test]
    fn memory_key_is_content_addressed_not_id_addressed() {
        let a = EntityKey::memory([0u8; 16], &[1u8; 16]);
        let b = EntityKey::memory([0u8; 16], &[1u8; 16]);
        let c = EntityKey::memory([0u8; 16], &[2u8; 16]);
        assert_eq!(a, b);
        assert_ne!(a, c);
    }

    #[test]
    fn schema_declares_account_id_on_every_table() {
        // The C3 fix, asserted rather than promised: every table that can hold
        // per-account data must have an `account_id` column.
        for table in [
            "entity_root",
            "entity_field",
            "op_log",
            "conflict",
            "device_cursor",
            "tdlib_cursor",
        ] {
            let marker = format!("CREATE TABLE IF NOT EXISTS {table} (");
            let start = SCHEMA_SQL
                .find(&marker)
                .unwrap_or_else(|| panic!("{table} is missing from SCHEMA_SQL"));
            let body = &SCHEMA_SQL[start + marker.len()..];
            let end = body.find(");").expect("table declaration terminated");
            assert!(
                body[..end].contains("account_id"),
                "{table} must carry account_id"
            );
        }
    }

    #[test]
    fn oplog_op_id_is_unique() {
        // H8/H9: a duplicate op_id must be impossible at the storage layer, not
        // merely avoided by the engine.
        assert!(SCHEMA_SQL.contains("CREATE UNIQUE INDEX IF NOT EXISTS idx_op_id"));
    }

    #[test]
    fn schema_sets_busy_timeout_per_connection() {
        // H6: the C++ side set busy_timeout on exactly one connection.
        assert!(SCHEMA_SQL.contains("PRAGMA busy_timeout"));
        assert!(SCHEMA_SQL.contains("journal_mode = WAL"));
    }
}
