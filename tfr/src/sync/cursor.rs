//! Monotone pull cursor.
//!
//! # Why not `updatedAt`
//!
//! `PgFetchRemoteStamp` compares a `sha256` of the dataset and
//! `teleforge_sync_state.revision` is `int(base::unixtime::now())` — a *second*
//! granularity value written by different devices. Two uploads in the same second
//! share a revision, so a client that remembers "I have revision 1000" cannot tell
//! whether it is missing the second one. Nothing in the C++ path is a cursor.
//!
//! # What this is
//!
//! An append-only `op_seq` per tenant, allocated by the agent from a dedicated
//! table:
//!
//! ```sql
//! CREATE TABLE seq (
//!   tenant   BLOB NOT NULL,
//!   name     TEXT NOT NULL,
//!   next     INTEGER NOT NULL,
//!   PRIMARY KEY (tenant, name)
//! ) WITHOUT ROWID;
//! ```
//!
//! Rules:
//! * a value is never handed out twice and never goes backwards, including after
//!   a crash (`UPDATE ... RETURNING` inside the same transaction as the insert);
//! * `VACUUM` does not renumber it (it is a stored counter, not a rowid) — the
//!   C++ code's reliance on `rowid`/`max(id)+1` is exactly what
//!   `storeSyncArtifact` got wrong (finding L7);
//! * the client sends `since_seq` and the agent returns everything `> since_seq`,
//!   so an unchanged tenant costs one integer comparison, not a snapshot hash.

use crate::error::{Error, Result};

/// Highest sequence a tenant has allocated. `0` means "nothing yet", which is
/// also a valid `since_seq` for a first sync.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Cursor(pub u64);

impl Cursor {
    pub const ZERO: Self = Self(0);

    pub fn allocate(&mut self) -> Result<u64> {
        let value = self
            .0
            .checked_add(1)
            .ok_or_else(|| Error::Protocol("op_seq exhausted; needs a new tenant".to_string()))?;
        self.0 = value;
        Ok(value)
    }

    pub fn value(self) -> u64 {
        self.0
    }

    /// Advance from a value read out of storage, refusing to move backwards. A
    /// backwards move would re-emit already-delivered operations forever.
    pub fn advance_to(&mut self, candidate: u64) -> Result<()> {
        if candidate < self.0 {
            return Err(Error::Protocol(format!(
                "cursor would move backwards: {} -> {candidate}",
                self.0
            )));
        }
        self.0 = candidate;
        Ok(())
    }
}

/// What a client reports it has. Persisted next to the client's own state so a
/// reinstall can resume instead of re-downloading the world.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CursorState {
    pub tenant_hex: String,
    pub device_id: String,
    pub since_seq: u64,
    pub last_pull_at: Option<u64>,
    pub applied_ops: u64,
}

impl CursorState {
    pub fn new(tenant_hex: &str, device_id: &str) -> Self {
        Self {
            tenant_hex: tenant_hex.to_string(),
            device_id: device_id.to_string(),
            since_seq: 0,
            last_pull_at: None,
            applied_ops: 0,
        }
    }

    pub fn to_row(&self) -> String {
        format!(
            "{{\"tenant\":\"{}\",\"device\":\"{}\",\"since_seq\":{},\"applied_ops\":{}}}",
            self.tenant_hex, self.device_id, self.since_seq, self.applied_ops
        )
    }

    /// Sanity check on a client-supplied cursor: a value above the agent's own
    /// high-water mark means the client is talking about a different tenant or is
    /// lying, and answering "nothing new, forever" would be worse than an error.
    pub fn validate(&self, high_water: Cursor) -> Result<()> {
        if self.since_seq > high_water.0 {
            return Err(Error::Protocol(format!(
                "since_seq {} is beyond the tenant high-water mark {}",
                self.since_seq, high_water.0
            )));
        }
        Ok(())
    }
}

/// Bounded page for a pull. The limit is enforced by the agent, not requested by
/// the client, so a hostile client cannot ask for the whole dataset in one frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PageSpec {
    pub since_seq: u64,
    pub limit: u32,
}

pub const DEFAULT_PAGE_LIMIT: u32 = 500;
pub const MAX_PAGE_LIMIT: u32 = 2_000;

impl PageSpec {
    pub fn new(since_seq: u64, requested_limit: Option<u32>) -> Self {
        let limit = requested_limit
            .unwrap_or(DEFAULT_PAGE_LIMIT)
            .clamp(1, MAX_PAGE_LIMIT);
        Self { since_seq, limit }
    }
}

/// Result of a pull. `has_more` lets the client loop without guessing, and
/// `high_water` lets it show "N operations behind" in the UI.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Page {
    pub ops: u64,
    pub next_seq: u64,
    pub has_more: bool,
    pub high_water: u64,
}

impl Page {
    pub fn empty(high_water: u64) -> Self {
        Self {
            ops: 0,
            next_seq: high_water,
            has_more: false,
            high_water,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cursor_advances_monotonically() {
        let mut cursor = Cursor::ZERO;
        assert_eq!(cursor.allocate().unwrap(), 1);
        assert_eq!(cursor.allocate().unwrap(), 2);
        assert_eq!(cursor.value(), 2);
    }

    #[test]
    fn cursor_refuses_to_move_backwards() {
        let mut cursor = Cursor(10);
        assert!(cursor.advance_to(9).is_err());
        assert!(cursor.advance_to(10).is_ok());
        assert!(cursor.advance_to(11).is_ok());
    }

    #[test]
    fn cursor_overflow_is_an_error_not_a_wrap() {
        let mut cursor = Cursor(u64::MAX);
        assert!(cursor.allocate().is_err());
        assert_eq!(cursor.value(), u64::MAX);
    }

    #[test]
    fn page_limit_is_clamped() {
        assert_eq!(PageSpec::new(0, Some(999_999)).limit, MAX_PAGE_LIMIT);
        assert_eq!(PageSpec::new(0, Some(0)).limit, 1);
        assert_eq!(PageSpec::new(0, None).limit, DEFAULT_PAGE_LIMIT);
    }

    #[test]
    fn client_cursor_cannot_exceed_high_water() {
        let state = CursorState {
            tenant_hex: "t".to_string(),
            device_id: "d".to_string(),
            since_seq: 50,
            last_pull_at: None,
            applied_ops: 0,
        };
        assert!(state.validate(Cursor(40)).is_err());
        assert!(state.validate(Cursor(50)).is_ok());
    }
}
