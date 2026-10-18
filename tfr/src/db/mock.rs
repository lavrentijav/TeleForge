//! A [`DbSource`] that answers from memory.
//!
//! Compiled for tests only (`#[cfg(test)] mod mock` in [`crate::db`]): the release
//! binary must have no way to answer a `SELECT` other than through PostgreSQL or
//! MySQL, so a test double is not part of the shipped surface.
//!
//! It exists because the behaviour worth testing is not "can we talk to PostgreSQL"
//! — that needs a live server and belongs in an integration environment — but the
//! *orchestration*: cursor bookkeeping, sealing, transaction boundaries, health
//! counters, and the wire shape of `db.status` and `sync.pull`.

use crate::db::{class, DbRow, DbSource, DbStatus, Engine, VaultState};
use crate::error::{Error, Result};

/// An in-memory DB.
#[derive(Clone, Debug)]
pub struct MockSource {
    rows: Vec<DbRow>,
    connected: bool,
    tables_ok: bool,
    engine: Engine,
    database: String,
    user: String,
    /// Class of the next failure, if any. Cleared after it fires.
    fail_next: Option<&'static str>,
    /// Every failure, not just the next one.
    always_fail: Option<&'static str>,
    batches: usize,
    last_cursor: Option<i64>,
}

impl MockSource {
    pub fn new(rows: Vec<DbRow>) -> Self {
        Self {
            rows,
            connected: true,
            tables_ok: true,
            engine: Engine::Postgres,
            database: "teleforge".to_string(),
            user: "teleforge_agent".to_string(),
            fail_next: None,
            always_fail: None,
            batches: 0,
            last_cursor: None,
        }
    }

    /// A source whose `SELECT` fails with `class`.
    pub fn failing(class: &'static str) -> Self {
        let mut source = Self::new(Vec::new());
        source.connected = false;
        source.tables_ok = false;
        source.always_fail = Some(class);
        source
    }

    pub fn with_tables_ok(mut self, tables_ok: bool) -> Self {
        self.tables_ok = tables_ok;
        self
    }

    pub fn with_engine(mut self, engine: Engine) -> Self {
        self.engine = engine;
        self
    }

    pub fn with_database(mut self, database: &str) -> Self {
        self.database = database.to_string();
        self
    }

    pub fn with_user(mut self, user: &str) -> Self {
        self.user = user.to_string();
        self
    }

    /// Fail exactly once, then behave.
    pub fn fail_once(mut self, class: &'static str) -> Self {
        self.fail_next = Some(class);
        self
    }

    /// How many `next_batch` calls were made.
    pub fn batches(&self) -> usize {
        self.batches
    }

    /// The cursor the last `next_batch` was asked for.
    pub fn last_cursor(&self) -> Option<i64> {
        self.last_cursor
    }

    /// The rows this source still has after `cursor`, without advancing anything.
    pub fn rows_after(&self, cursor: i64) -> Vec<DbRow> {
        self.rows
            .iter()
            .filter(|row| row.key > cursor)
            .cloned()
            .collect()
    }
}

impl DbSource for MockSource {
    fn status(&self) -> DbStatus {
        DbStatus {
            connected: self.connected,
            latency_ms: 2,
            engine: Some(self.engine.as_str()),
            database: Some(self.database.clone()),
            reader_user: Some(self.user.clone()),
            tls: Some("tunnel"),
            cursor: None,
            lag_ms: None,
            last_read_ms: None,
            tables_ok: self.tables_ok,
            vault: VaultState::Unlocked.as_str(),
            last_error: self.fail_next.or(self.always_fail),
            last_ok_at: None,
            fails: 0,
            batches: self.batches as u64,
            applied: 0,
            duplicates: 0,
            skipped: 0,
        }
    }

    fn next_batch(&mut self, cursor: i64, limit: usize) -> Result<Vec<DbRow>> {
        self.batches += 1;
        self.last_cursor = Some(cursor);
        if let Some(class) = self.always_fail {
            return Err(Error::Database(class));
        }
        if let Some(class) = self.fail_next.take() {
            return Err(Error::Database(class));
        }
        if !self.connected {
            return Err(Error::Database(class::CONNECT));
        }
        Ok(self
            .rows
            .iter()
            .filter(|row| row.key > cursor)
            .take(limit)
            .cloned()
            .collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::DbRowBody;
    use crate::store::EventKind;

    fn row(key: i64) -> DbRow {
        DbRow {
            key,
            observed_ms: 1_000 + key,
            body: DbRowBody::Event {
                kind: EventKind::PresenceOnline,
                entity_id: format!("presence:{key}"),
                field: None,
                value: key.to_be_bytes().to_vec(),
            },
        }
    }

    #[test]
    fn the_mock_returns_only_rows_after_the_cursor() {
        let mut source = MockSource::new(vec![row(1), row(2), row(3)]);
        let first = source.next_batch(0, 2).unwrap();
        assert_eq!(first.len(), 2);
        assert_eq!(first[0].key, 1);
        let second = source.next_batch(2, 2).unwrap();
        assert_eq!(second.len(), 1);
        assert_eq!(second[0].key, 3);
        assert!(source.next_batch(3, 2).unwrap().is_empty());
        assert_eq!(source.batches(), 3);
        assert_eq!(source.last_cursor(), Some(3));
    }

    #[test]
    fn the_mock_can_fail_once_or_always() {
        let mut source = MockSource::new(vec![row(1)]).fail_once(class::QUERY);
        assert!(source.next_batch(0, 10).is_err());
        assert!(source.next_batch(0, 10).unwrap().len() == 1);

        let mut broken = MockSource::failing(class::CONNECT);
        assert!(broken.next_batch(0, 10).is_err());
        assert_eq!(broken.status().last_error, Some(class::CONNECT));
        assert!(!broken.status().connected);
    }
}
