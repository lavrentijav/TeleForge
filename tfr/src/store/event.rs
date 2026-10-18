//! The passive event: what the client reports, and what the agent accepts.
//!
//! The shape is deliberately flat and boring:
//!
//! ```text
//! Event { account_id, device_id, kind, entity_id, field?, value, hlc, content_hash }
//! ```
//!
//! # Address by deterministic key, never by a local autoincrement id
//!
//! `entity_id` is a *string* built by the client from Telegram ids, e.g.
//! `message:-1001234567890:42`. It is not a row id from the client's `data.tforge`:
//! `MemoryItem.id`-style keys are per-device and two devices overwrite each other
//! when they collide (audit H7). The parser below pins the format, so a typo
//! becomes a `Rejected` verdict instead of a new entity that silently duplicates
//! history.
//!
//! # Account isolation at the type level
//!
//! [`AccountId`] cannot hold all-zero bytes, and [`Event::account_id`] is an
//! `Option` so "the client forgot the account" is representable and *rejectable*
//! rather than silently turning into account `000…0`. Ingest additionally takes
//! the authenticated account as a scope, so an event for another account is
//! `Rejected(ForeignAccount)` — see [`crate::store::ingest`].

use core::fmt;

use sha2::{Digest, Sha256};

use crate::crypto::sig;
use crate::error::{Error, Result};
use crate::sync::clock::Hlc;

/// 16 random bytes identifying the owning Telegram account.
///
/// Construction rejects the all-zero value because that is what a missing or
/// uninitialised column materialises as, and a store full of `000…0` rows is a
/// cross-account leak waiting for the first real account with that id.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct AccountId([u8; 16]);

impl AccountId {
    pub const LEN: usize = 16;

    pub fn new(bytes: [u8; 16]) -> Result<Self> {
        if bytes.iter().all(|byte| *byte == 0) {
            return Err(Error::Invalid(
                "account_id must not be all zeroes".to_string(),
            ));
        }
        Ok(Self(bytes))
    }

    pub fn from_slice(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != Self::LEN {
            return Err(Error::Invalid(format!(
                "account_id must be {} bytes, got {}",
                Self::LEN,
                bytes.len()
            )));
        }
        let mut out = [0u8; 16];
        out.copy_from_slice(bytes);
        Self::new(out)
    }

    pub fn from_hex(text: &str) -> Result<Self> {
        let bytes = sig::parse_hex(text)?;
        Self::from_slice(&bytes)
    }

    pub fn as_bytes(&self) -> &[u8; 16] {
        &self.0
    }

    /// The same bytes as a plain slice, which is what every SQL binder wants.
    pub fn as_blob(&self) -> &[u8] {
        &self.0
    }

    pub fn to_hex(&self) -> String {
        sig::to_hex(&self.0)
    }
}

impl fmt::Debug for AccountId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "AccountId({})", self.to_hex())
    }
}

impl fmt::Display for AccountId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

/// What happened. Numeric values are storage format: they are written into
/// `ops.kind` and `presence.status`, so they must never be renumbered.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum EventKind {
    MessageNew = 1,
    MessageEdit = 2,
    MessageDelete = 3,
    PresenceOnline = 4,
    PresenceOffline = 5,
    ProfileUpdate = 6,
    ArchiveObservation = 7,
}

impl EventKind {
    pub fn code(self) -> u8 {
        self as u8
    }

    pub fn from_code(code: u8) -> Option<Self> {
        // Exhaustive: a new variant that forgets this match is a compile error,
        // not a silently unreadable history row.
        match code {
            1 => Some(Self::MessageNew),
            2 => Some(Self::MessageEdit),
            3 => Some(Self::MessageDelete),
            4 => Some(Self::PresenceOnline),
            5 => Some(Self::PresenceOffline),
            6 => Some(Self::ProfileUpdate),
            7 => Some(Self::ArchiveObservation),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::MessageNew => "message_new",
            Self::MessageEdit => "message_edit",
            Self::MessageDelete => "message_delete",
            Self::PresenceOnline => "presence_online",
            Self::PresenceOffline => "presence_offline",
            Self::ProfileUpdate => "profile_update",
            Self::ArchiveObservation => "archive_observation",
        }
    }

    /// Parse the wire name written by [`Self::as_str`].
    ///
    /// The wire form is the *name*, not the numeric code, because a client that
    /// sends `"kind": 2` is one enum reordering away from writing edits as deletes.
    /// The numeric code is still accepted by the API layer for a C++ client that
    /// already has constants; both end up here.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "message_new" => Some(Self::MessageNew),
            "message_edit" => Some(Self::MessageEdit),
            "message_delete" => Some(Self::MessageDelete),
            "presence_online" => Some(Self::PresenceOnline),
            "presence_offline" => Some(Self::PresenceOffline),
            "profile_update" => Some(Self::ProfileUpdate),
            "archive_observation" => Some(Self::ArchiveObservation),
            _ => None,
        }
    }

    /// Which entity namespace this kind must be addressed in.
    pub fn class(self) -> EntityClass {
        match self {
            Self::MessageNew | Self::MessageEdit | Self::MessageDelete => EntityClass::Message,
            Self::PresenceOnline | Self::PresenceOffline => EntityClass::Presence,
            Self::ProfileUpdate => EntityClass::Profile,
            Self::ArchiveObservation => EntityClass::Archive,
        }
    }

    /// A delete is an *observation of a deletion*, not a delete of our rows: the
    /// journal keeps everything that was captured before it.
    pub fn is_delete(self) -> bool {
        matches!(self, Self::MessageDelete)
    }

    pub fn is_message(self) -> bool {
        self.class() == EntityClass::Message
    }
}

/// The entity namespace. Encoded as the `entity_id` prefix.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum EntityClass {
    Message,
    Presence,
    Profile,
    Archive,
}

impl EntityClass {
    pub fn prefix(self) -> &'static str {
        match self {
            Self::Message => "message",
            Self::Presence => "presence",
            Self::Profile => "profile",
            Self::Archive => "archive",
        }
    }

    pub fn from_prefix(text: &str) -> Option<Self> {
        match text {
            "message" => Some(Self::Message),
            "presence" => Some(Self::Presence),
            "profile" => Some(Self::Profile),
            "archive" => Some(Self::Archive),
            _ => None,
        }
    }
}

pub const MAX_ENTITY_ID_LEN: usize = 128;
pub const MAX_DEVICE_ID_LEN: usize = 64;
/// One event's `value`. A serialised media/reply blob fits; a whole database
/// dump does not, and a client cannot make the agent allocate unbounded memory.
pub const MAX_VALUE_BYTES: usize = 256 * 1024;
pub const MAX_PROFILE_VALUE_BYTES: usize = 4 * 1024;
pub const MAX_ARCHIVE_PAYLOAD_BYTES: usize = 8 * 1024;

/// A parsed `entity_id`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EntityId {
    pub class: EntityClass,
    /// `dialog_id` for messages, `peer_id` otherwise.
    pub primary: i64,
    /// `message_id` for messages, `0` otherwise.
    pub secondary: i64,
    /// The canonical string, i.e. what is stored.
    pub text: String,
}

impl EntityId {
    pub fn parse(raw: &str) -> Result<Self> {
        if raw.is_empty() || raw.len() > MAX_ENTITY_ID_LEN {
            return Err(Error::Invalid(format!(
                "entity_id must be 1..={MAX_ENTITY_ID_LEN} bytes, got {}",
                raw.len()
            )));
        }
        if !raw.is_ascii() || raw.bytes().any(|byte| byte.is_ascii_whitespace()) {
            return Err(Error::Invalid(
                "entity_id must be printable ASCII without spaces".to_string(),
            ));
        }
        let mut parts = raw.split(':');
        let prefix = parts
            .next()
            .ok_or_else(|| Error::Invalid("entity_id is empty".to_string()))?;
        let class = EntityClass::from_prefix(prefix)
            .ok_or_else(|| Error::Invalid(format!("unknown entity prefix {prefix:?}")))?;
        let primary = parse_i64(parts.next(), "primary id")?;
        let secondary = match class {
            EntityClass::Message => {
                let message_id = parse_i64(parts.next(), "message id")?;
                if message_id < 0 {
                    return Err(Error::Invalid(
                        "message id must not be negative".to_string(),
                    ));
                }
                message_id
            }
            _ => {
                if primary == 0 {
                    return Err(Error::Invalid("peer id must not be zero".to_string()));
                }
                0
            }
        };
        if parts.next().is_some() {
            return Err(Error::Invalid(format!(
                "entity_id {raw:?} has too many components"
            )));
        }
        if class == EntityClass::Message && primary == 0 {
            return Err(Error::Invalid("dialog id must not be zero".to_string()));
        }
        Ok(Self {
            class,
            primary,
            secondary,
            text: raw.to_string(),
        })
    }

    pub fn message(dialog_id: i64, message_id: i64) -> Result<Self> {
        Self::parse(&format!("message:{dialog_id}:{message_id}"))
    }

    pub fn presence(peer_id: i64) -> Result<Self> {
        Self::parse(&format!("presence:{peer_id}"))
    }

    pub fn profile(peer_id: i64) -> Result<Self> {
        Self::parse(&format!("profile:{peer_id}"))
    }

    pub fn archive(peer_id: i64) -> Result<Self> {
        Self::parse(&format!("archive:{peer_id}"))
    }
}

fn parse_i64(value: Option<&str>, what: &str) -> Result<i64> {
    let text = value.ok_or_else(|| Error::Invalid(format!("entity_id has no {what}")))?;
    text.parse::<i64>()
        .map_err(|_| Error::Invalid(format!("{what} {text:?} is not an integer")))
}

/// Field names accepted per class. Keeping them as a closed set means a typo
/// (`"usernme"`) is rejected at the edge instead of creating a second history for
/// a field nobody will ever read. Adding a name is a format change; removing one
/// would orphan history.
pub const MESSAGE_FIELDS: [&str; 18] = [
    "text",
    "entities",
    "edit_date",
    "views",
    "media",
    "media_path",
    "mime_type",
    "document",
    "thumbs",
    "attributes",
    "reply",
    "reply_markup",
    "post_author",
    "fwd_name",
    "fwd_post_author",
    "grouped_id",
    "topic_id",
    "flags",
];

pub const PROFILE_FIELDS: [&str; 8] = [
    "first_name",
    "last_name",
    "username",
    "bio",
    "userpic_path",
    "chat_title",
    "self_in_chat",
    "still_in_chat",
];

pub const ARCHIVE_SOURCES: [&str; 5] = [
    "new_message",
    "forward_author",
    "participant",
    "opened_chat",
    "profile_update",
];

/// One event from the client. `Debug` redacts `value`: the agent forwards these
/// into logs and metrics, and "value is ciphertext anyway" is not a property the
/// agent can check.
#[derive(Clone, Default)]
pub struct Event {
    pub account_id: Option<AccountId>,
    pub device_id: String,
    pub kind: Option<EventKind>,
    pub entity_id: String,
    pub field: Option<String>,
    pub value: Vec<u8>,
    pub hlc: Hlc,
    pub content_hash: [u8; 32],
}

impl fmt::Debug for Event {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Event")
            .field("account_id", &self.account_id)
            .field("device_id", &self.device_id)
            .field("kind", &self.kind)
            .field("entity_id", &self.entity_id)
            .field("field", &self.field)
            .field(
                "value",
                &format_args!("<redacted, {} bytes>", self.value.len()),
            )
            .field("hlc", &self.hlc)
            .finish()
    }
}

impl Event {
    /// A fully populated event. Kept as a constructor only to keep the call sites
    /// readable; the struct is plain data on purpose.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        account_id: AccountId,
        device_id: impl Into<String>,
        kind: EventKind,
        entity_id: impl Into<String>,
        field: Option<&str>,
        value: Vec<u8>,
        hlc: Hlc,
    ) -> Self {
        let mut event = Self {
            account_id: Some(account_id),
            device_id: device_id.into(),
            kind: Some(kind),
            entity_id: entity_id.into(),
            field: field.map(str::to_string),
            value,
            hlc,
            content_hash: [0u8; 32],
        };
        event.content_hash = event.compute_content_hash();
        event
    }

    /// `sha256` over the event's identity, used as the dedup key.
    ///
    /// The client sends it, because only the client knows whether two events are
    /// "the same observation submitted twice" or "the same text captured twice by
    /// two devices at two moments". The agent uses it for idempotency only; it
    /// never derives any semantics from it, so a client that sends a constant hash
    /// only breaks its own ingest.
    pub fn compute_content_hash(&self) -> [u8; 32] {
        content_hash(
            self.kind,
            &self.entity_id,
            self.field.as_deref(),
            &self.value,
            self.hlc,
        )
    }

    /// Fill in `content_hash` from the rest of the event.
    pub fn with_computed_hash(mut self) -> Self {
        self.content_hash = self.compute_content_hash();
        self
    }

    pub fn hlc_physical_ms(&self) -> i64 {
        i64::try_from(self.hlc.physical_ms).unwrap_or(i64::MAX)
    }
}

/// The dedup key. `account_id` is not mixed in because the uniqueness constraint
/// is already scoped by account, and mixing it in would make one account's
/// identical observation dedupe against another's only by accident of a hash
/// collision.
pub fn content_hash(
    kind: Option<EventKind>,
    entity_id: &str,
    field: Option<&str>,
    value: &[u8],
    hlc: Hlc,
) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(b"tf/passive/event/v1\0");
    hasher.update([kind.map(EventKind::code).unwrap_or(0)]);
    hasher.update((entity_id.len() as u32).to_be_bytes());
    hasher.update(entity_id.as_bytes());
    match field {
        Some(field) => {
            hasher.update([1u8]);
            hasher.update((field.len() as u32).to_be_bytes());
            hasher.update(field.as_bytes());
        }
        None => hasher.update([0u8]),
    }
    hasher.update(hlc.encode());
    hasher.update((value.len() as u64).to_be_bytes());
    hasher.update(value);
    let digest = hasher.finalize();
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest);
    out
}

/// Why an event was not applied. Every variant is a *client* mistake, so the
/// message can be shown to the user without leaking anything server-side.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RejectReason {
    /// `account_id` absent (the `Option` was `None`).
    MissingAccount,
    /// `account_id` is not the account the request is authenticated for.
    ForeignAccount,
    /// `device_id` empty, too long or not `[A-Za-z0-9._-]`.
    MalformedDevice,
    /// The event carries no `kind`, so it cannot be addressed at all.
    MissingKind,
    /// HLC is `0`/`0`, i.e. the client did not stamp the write.
    MissingHlc,
    /// HLC did not advance past the last one seen from this device.
    NonMonotonicHlc,
    /// `entity_id` does not parse, or its prefix does not match the kind.
    MalformedEntityId,
    /// The kind's namespace does not match the entity's prefix.
    KindEntityMismatch,
    /// A field name this kind does not define, where one is required.
    UnknownField,
    /// A field was required and missing.
    MissingField,
    /// A field was supplied where the kind carries none.
    UnexpectedField,
    /// `value` is larger than the class allows.
    OversizedValue,
    /// A presence event whose `value` is not exactly one big-endian `i64`.
    MalformedPresencePayload,
    /// `content_hash` is all zeroes, i.e. "not computed".
    MissingContentHash,
    /// A message revision older than the entity's delete. Refusing beats
    /// resurrecting deleted data (the mirror of `RejectedTombstone` in
    /// `sync::engine`).
    Tombstoned,
}

impl RejectReason {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::MissingAccount => "missing_account",
            Self::ForeignAccount => "foreign_account",
            Self::MalformedDevice => "malformed_device",
            Self::MissingKind => "missing_kind",
            Self::MissingHlc => "missing_hlc",
            Self::NonMonotonicHlc => "non_monotonic_hlc",
            Self::MalformedEntityId => "malformed_entity_id",
            Self::KindEntityMismatch => "kind_entity_mismatch",
            Self::UnknownField => "unknown_field",
            Self::MissingField => "missing_field",
            Self::UnexpectedField => "unexpected_field",
            Self::OversizedValue => "oversized_value",
            Self::MalformedPresencePayload => "malformed_presence_payload",
            Self::MissingContentHash => "missing_content_hash",
            Self::Tombstoned => "tombstoned",
        }
    }
}

impl fmt::Display for RejectReason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Outcome for one event of a batch.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum IngestResult {
    /// Stored, with the `op_seq` it was assigned.
    Applied { op_seq: u64 },
    /// Already stored under the same `content_hash`; `op_seq` is the original.
    Duplicate { op_seq: u64 },
    /// Not stored. The batch continues with the next event.
    Rejected { reason: RejectReason },
}

impl IngestResult {
    pub fn is_applied(&self) -> bool {
        matches!(self, Self::Applied { .. })
    }

    pub fn is_duplicate(&self) -> bool {
        matches!(self, Self::Duplicate { .. })
    }

    pub fn is_rejected(&self) -> bool {
        matches!(self, Self::Rejected { .. })
    }

    /// Stable one-line encoding, used in `sync.push`-style responses and tests.
    pub fn encode(&self) -> String {
        match self {
            Self::Applied { op_seq } => format!("applied:{op_seq}"),
            Self::Duplicate { op_seq } => format!("duplicate:{op_seq}"),
            Self::Rejected { reason } => format!("rejected:{}", reason.as_str()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn account() -> AccountId {
        AccountId::new([9u8; 16]).unwrap()
    }

    #[test]
    fn account_id_rejects_all_zeroes() {
        assert!(AccountId::new([0u8; 16]).is_err());
        let mut non_zero = [0u8; 16];
        non_zero[15] = 1;
        assert!(AccountId::new(non_zero).is_ok());
        assert!(AccountId::from_slice(&[1u8; 15]).is_err());
        assert!(AccountId::from_slice(&[1u8; 16]).is_ok());
    }

    #[test]
    fn account_id_hex_roundtrip() {
        let id = AccountId::from_hex("000102030405060708090a0b0c0d0e0f").unwrap();
        assert_eq!(id.to_hex(), "000102030405060708090a0b0c0d0e0f");
        assert!(AccountId::from_hex("00000000000000000000000000000000").is_err());
        assert!(AccountId::from_hex("zz").is_err());
    }

    #[test]
    fn every_kind_code_roundtrips() {
        for code in 1u8..=7 {
            let kind = EventKind::from_code(code).expect("known kind");
            assert_eq!(kind.code(), code);
            assert!(!kind.as_str().is_empty());
        }
        assert!(EventKind::from_code(0).is_none());
        assert!(EventKind::from_code(8).is_none());
    }

    #[test]
    fn kinds_map_to_the_right_entity_class() {
        assert_eq!(EventKind::MessageEdit.class(), EntityClass::Message);
        assert_eq!(EventKind::MessageDelete.class(), EntityClass::Message);
        assert_eq!(EventKind::PresenceOnline.class(), EntityClass::Presence);
        assert_eq!(EventKind::ProfileUpdate.class(), EntityClass::Profile);
        assert_eq!(EventKind::ArchiveObservation.class(), EntityClass::Archive);
        for class in [
            EntityClass::Message,
            EntityClass::Presence,
            EntityClass::Profile,
            EntityClass::Archive,
        ] {
            assert_eq!(EntityClass::from_prefix(class.prefix()), Some(class));
        }
        assert!(EntityClass::from_prefix("chat").is_none());
    }

    #[test]
    fn entity_ids_parse_for_every_class() {
        let message = EntityId::parse("message:-1001234567890:42").unwrap();
        assert_eq!(message.class, EntityClass::Message);
        assert_eq!(message.primary, -1001234567890);
        assert_eq!(message.secondary, 42);

        let presence = EntityId::presence(900).unwrap();
        assert_eq!(presence.primary, 900);
        assert_eq!(presence.secondary, 0);
        assert_eq!(presence.text, "presence:900");

        assert!(EntityId::profile(-5).is_ok());
        assert!(EntityId::archive(1).is_ok());
    }

    #[test]
    fn entity_id_rejects_the_shapes_that_would_duplicate_history() {
        for bad in [
            "",
            "message",
            "message:1",
            "message:1:2:3",
            "message:zero:2",
            "message:0:2",
            "message:1:-1",
            "chat:1",
            "presence:0",
            "presence:1:2",
            "presence: 1",
            "presence:1 ",
            "presence:999999999999999999999999",
        ] {
            assert!(
                EntityId::parse(bad).is_err(),
                "{bad:?} must not be a valid entity id"
            );
        }
        let too_long = format!("presence:{}", "1".repeat(MAX_ENTITY_ID_LEN));
        assert!(EntityId::parse(&too_long).is_err());
    }

    #[test]
    fn field_sets_are_closed_and_non_empty() {
        assert!(MESSAGE_FIELDS.contains(&"text"));
        assert!(!MESSAGE_FIELDS.contains(&"txt"));
        assert!(PROFILE_FIELDS.contains(&"username"));
        assert!(!PROFILE_FIELDS.contains(&"user_name"));
        assert!(ARCHIVE_SOURCES.contains(&"participant"));
        assert!(!ARCHIVE_SOURCES.contains(&"participants"));
        assert_eq!(MESSAGE_FIELDS.len(), 18);
        assert_eq!(PROFILE_FIELDS.len(), 8);
        assert_eq!(ARCHIVE_SOURCES.len(), 5);
    }

    #[test]
    fn content_hash_distinguishes_every_component() {
        let base = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:2",
            Some("text"),
            b"hello",
            Hlc::new(10, 0),
        );
        let other_kind = content_hash(
            Some(EventKind::MessageNew),
            "message:1:2",
            Some("text"),
            b"hello",
            Hlc::new(10, 0),
        );
        let other_entity = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:3",
            Some("text"),
            b"hello",
            Hlc::new(10, 0),
        );
        let other_field = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:2",
            Some("entities"),
            b"hello",
            Hlc::new(10, 0),
        );
        let other_value = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:2",
            Some("text"),
            b"hellp",
            Hlc::new(10, 0),
        );
        let other_hlc = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:2",
            Some("text"),
            b"hello",
            Hlc::new(11, 0),
        );
        let no_field = content_hash(
            Some(EventKind::MessageEdit),
            "message:1:2",
            None,
            b"hello",
            Hlc::new(10, 0),
        );
        let all = [
            base,
            other_kind,
            other_entity,
            other_field,
            other_value,
            other_hlc,
            no_field,
        ];
        for (index, left) in all.iter().enumerate() {
            for right in &all[index + 1..] {
                assert_ne!(left, right, "content_hash must separate every component");
            }
        }
        // Same input, same hash: idempotency depends on it.
        assert_eq!(
            base,
            content_hash(
                Some(EventKind::MessageEdit),
                "message:1:2",
                Some("text"),
                b"hello",
                Hlc::new(10, 0)
            )
        );
    }

    /// Golden vectors for the C++ side (`docs/client-integration.md` § 9).
    ///
    /// The client computes `content_hash` itself, so the two implementations must
    /// agree byte for byte. These values are asserted here and copied into the
    /// integration spec; if a change makes them differ, both sides have to change
    /// together or idempotency silently stops working across the boundary.
    #[test]
    fn golden_vectors_for_the_client() {
        let vectors = [
            (
                EventKind::MessageNew,
                "message:-1001234567890:42",
                Some("text"),
                b"hello".to_vec(),
                Hlc::new(1_000, 0),
            ),
            (
                EventKind::MessageDelete,
                "message:-1001234567890:42",
                None,
                Vec::new(),
                Hlc::new(1_001, 0),
            ),
            (
                EventKind::PresenceOnline,
                "presence:900",
                None,
                5_000i64.to_be_bytes().to_vec(),
                Hlc::new(1_002, 0),
            ),
            (
                EventKind::ProfileUpdate,
                "profile:900",
                Some("username"),
                b"@neo".to_vec(),
                Hlc::new(1_003, 7),
            ),
        ];
        // Expected values are also in `docs/client-integration.md` § 9.
        let expected = [
            "0707ecc057391f747e35b4c0666d88660d6579a5416aa91a12ea83c6b458985d",
            "fa7d62cd9f2a62290416f8dc8481626b7a2475ab351ebebd64be12fade9510ac",
            "9c3ec042b54dcd2776f37577d689b35a1ed889607648483625ac3b2275d386cd",
            "8e18b6d244eaf89bffdd4f5807727abee9ad60680c95baddf614a02e5caf05cd",
        ];
        for ((kind, entity, field, value, hlc), want) in vectors.into_iter().zip(expected) {
            let hash = content_hash(Some(kind), entity, field, &value, hlc);
            let got = crate::crypto::sig::to_hex(&hash);
            println!(
                "GOLDEN {} {} {} {} {} -> {}",
                kind.code(),
                entity,
                field.unwrap_or("-"),
                crate::crypto::sig::to_hex(&value),
                hlc.to_wire(),
                got
            );
            assert_eq!(got, want, "the wire hash changed; both sides must change");
        }
    }

    #[test]
    fn event_new_computes_the_hash() {
        let event = Event::new(
            account(),
            "dev-a",
            EventKind::MessageNew,
            "message:1:2",
            Some("text"),
            b"hi".to_vec(),
            Hlc::new(5, 0),
        );
        assert_ne!(event.content_hash, [0u8; 32]);
        assert_eq!(event.content_hash, event.compute_content_hash());
        assert_eq!(
            Event {
                content_hash: [0u8; 32],
                ..event.clone()
            }
            .with_computed_hash()
            .content_hash,
            event.content_hash
        );
    }

    #[test]
    fn event_debug_never_prints_the_value() {
        let event = Event::new(
            account(),
            "dev-a",
            EventKind::MessageNew,
            "message:1:2",
            Some("text"),
            b"secret message text".to_vec(),
            Hlc::new(5, 0),
        );
        let rendered = format!("{event:?}");
        assert!(!rendered.contains("secret message text"));
        assert!(rendered.contains("<redacted, 19 bytes>"));
        assert!(rendered.contains("message:1:2"));
    }

    #[test]
    fn ingest_result_encoding_is_stable() {
        assert_eq!(IngestResult::Applied { op_seq: 7 }.encode(), "applied:7");
        assert_eq!(
            IngestResult::Duplicate { op_seq: 7 }.encode(),
            "duplicate:7"
        );
        assert_eq!(
            IngestResult::Rejected {
                reason: RejectReason::ForeignAccount
            }
            .encode(),
            "rejected:foreign_account"
        );
        assert!(IngestResult::Applied { op_seq: 1 }.is_applied());
        assert!(IngestResult::Duplicate { op_seq: 1 }.is_duplicate());
        assert!(IngestResult::Rejected {
            reason: RejectReason::MissingAccount
        }
        .is_rejected());
    }

    #[test]
    fn reject_reasons_have_distinct_names() {
        let reasons = [
            RejectReason::MissingAccount,
            RejectReason::ForeignAccount,
            RejectReason::MalformedDevice,
            RejectReason::MissingKind,
            RejectReason::MissingHlc,
            RejectReason::NonMonotonicHlc,
            RejectReason::MalformedEntityId,
            RejectReason::KindEntityMismatch,
            RejectReason::UnknownField,
            RejectReason::MissingField,
            RejectReason::UnexpectedField,
            RejectReason::OversizedValue,
            RejectReason::MalformedPresencePayload,
            RejectReason::MissingContentHash,
            RejectReason::Tombstoned,
        ];
        let mut names: Vec<&str> = reasons.iter().map(|reason| reason.as_str()).collect();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), reasons.len());
    }
}
