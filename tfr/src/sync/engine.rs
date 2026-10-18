//! Apply/merge logic: the part that replaces `MergeDataDb`.
//!
//! The functions below that contain real logic ([`classify`], [`plan`]) are pure,
//! so they are unit-tested without a database. The SQL execution is stubbed and
//! carries the exact statements it will run, because the *ordering* of those
//! statements is the safety property: the sequence counter and the field write
//! must be in one transaction, or a crash can hand out an `op_seq` that no row
//! carries.
//!
//! # Transaction discipline
//!
//! Findings C4/H1/H6 all reduce to "a transaction that was opened and never
//! resolved". The engine therefore has exactly one way to write:
//!
//! ```text
//! BEGIN IMMEDIATE
//!   UPDATE seq SET next = next + 1 RETURNING next
//!   INSERT INTO entity_field (...)
//!   INSERT INTO op_log (...) ON CONFLICT(op_id) DO NOTHING
//!   INSERT/UPDATE entity_root
//! COMMIT            (on any error: ROLLBACK, then propagate)
//! ```
//!
//! A RAII guard is described in [`TransactionGuard`]; the real implementation
//! calls `ROLLBACK` from `Drop` so a `?` inside the block cannot leave the
//! connection mid-transaction (which is precisely the C4 bug:
//! `addEditedMessage`/`addDeletedMessage` call `begin_transaction` and only `LOG`
//! in `catch`).

use crate::error::{Error, Result};
use crate::sync::clock::Hlc;
use crate::sync::model::{Conflict, EntityRoot, FieldId, FieldRecord, Op, WriteVerdict};
use crate::sync::vclock::CausalOrder;

/// Decide what to do with one incoming op, given the local root and field.
///
/// Order of checks matters:
/// 1. entity tombstone — a delete that dominates the op wins over everything, so
///    a stale client cannot resurrect a deleted message;
/// 2. missing local field — trivially an apply;
/// 3. vector-clock domination — the only correct "newer" test;
/// 4. equal clocks with a different `op_id` — same logical write replayed from a
///    different device: treat as a conflict on value equality being unprovable
///    (we cannot compare ciphertext), which the caller resolves by keeping the
///    op with the greater `hlc` and still recording one `Conflict` row.
///
/// Note what is *not* here: no comparison by wall-clock seconds alone, and no
/// `>=` on a single timestamp.
pub fn classify(
    root: Option<&EntityRoot>,
    local_field: Option<&FieldRecord>,
    incoming: &Op,
) -> WriteVerdict {
    let Some(root) = root else {
        return WriteVerdict::Applied;
    };

    if root.deleted {
        if let Some(deleted_hlc) = root.deleted_hlc {
            if deleted_hlc >= incoming.hlc {
                return WriteVerdict::RejectedTombstone;
            }
        }
    }

    let Some(local_field) = local_field else {
        // The entity exists but this field has never been written: disjoint edit.
        return match root.vclock.compare(&incoming.vclock) {
            CausalOrder::Concurrent => WriteVerdict::Applied,
            _ => WriteVerdict::Applied,
        };
    };

    match root.vclock.compare(&incoming.vclock) {
        CausalOrder::Before => WriteVerdict::Applied,
        CausalOrder::After => WriteVerdict::Ignored,
        CausalOrder::Equal => {
            if local_field.version.hlc == incoming.hlc
                && local_field.version.writer_seq == incoming.writer_seq
            {
                WriteVerdict::Duplicate
            } else {
                WriteVerdict::Conflict
            }
        }
        CausalOrder::Concurrent => {
            if local_field.tombstone == incoming.tombstone {
                WriteVerdict::Conflict
            } else {
                // One side deleted the field, the other set it: that is a real
                // conflict too, but it is worth distinguishing in the telemetry.
                WriteVerdict::Conflict
            }
        }
    }
}

/// Merge a batch. Returns the verdicts in input order plus the conflicts that
/// must be surfaced to the user.
///
/// Batch-level rule: the *first* op for a given `(key, field)` wins the right to
/// be compared against the *original* local state; later ops in the same batch see
/// the state produced by earlier ones. That ordering makes a replay of a batch
/// idempotent, which is what a client retrying after a dropped connection sends.
pub fn plan(
    local_roots: &[(Vec<u8>, EntityRoot)],
    local_fields: &[(Vec<u8>, FieldRecord)],
    ops: &[Op],
) -> Result<Plan> {
    let mut roots = std::collections::BTreeMap::new();
    for (key, root) in local_roots {
        roots.insert(key.clone(), root.clone());
    }
    let mut fields = std::collections::BTreeMap::new();
    for (key, field) in local_fields {
        fields.insert(key.clone(), field.clone());
    }

    let mut plan = Plan::default();
    for op in ops {
        let root_id = op.key.encode().to_vec();
        let field_id = field_id_bytes(&root_id, op.field);
        let verdict = classify(roots.get(&root_id), fields.get(&field_id), op);
        plan.write_ops += 1;

        match verdict {
            WriteVerdict::Applied | WriteVerdict::PartiallyApplied => {
                plan.applied += 1;
                let merged = match roots.get(&root_id) {
                    Some(root) => root.vclock.merge(&op.vclock),
                    None => op.vclock.clone(),
                };
                roots.insert(
                    root_id.clone(),
                    EntityRoot {
                        key: op.key.clone(),
                        vclock: merged.clone(),
                        last_hlc: match roots.get(&root_id) {
                            Some(root) if root.last_hlc > op.hlc => root.last_hlc,
                            _ => op.hlc,
                        },
                        last_seq: 0,
                        deleted: false,
                        deleted_hlc: roots.get(&root_id).and_then(|r| r.deleted_hlc),
                    },
                );
                fields.insert(
                    field_id,
                    FieldRecord {
                        key: op.key.clone(),
                        field: op.field,
                        version: crate::sync::model::FieldVersion {
                            hlc: op.hlc,
                            writer_seq: op.writer_seq,
                        },
                        tombstone: op.tombstone,
                        value_ct: op.value_ct.clone(),
                    },
                );
            }
            WriteVerdict::Ignored => plan.ignored += 1,
            WriteVerdict::Duplicate => plan.duplicates += 1,
            WriteVerdict::RejectedTombstone => plan.rejected += 1,
            WriteVerdict::Conflict => {
                plan.conflicts += 1;
                let local = fields
                    .get(&field_id)
                    .map(|f| (f.version.hlc, f.version.writer_seq));
                plan.conflict_list.push(Conflict {
                    key: op.key.clone(),
                    field: op.field,
                    local: crate::sync::model::FieldVersion {
                        hlc: local.map(|l| l.0).unwrap_or(Hlc::ZERO),
                        writer_seq: local.map(|l| l.1).unwrap_or(0),
                    },
                    remote: crate::sync::model::FieldVersion {
                        hlc: op.hlc,
                        writer_seq: op.writer_seq,
                    },
                    tiebreak: op.vclock.deterministic_tiebreak(),
                });
            }
        }
    }
    Ok(plan)
}

/// Result of planning a batch. Counted rather than per-op so the client can show
/// "applied 12, conflicts 1" without a second round-trip.
#[derive(Debug, Default, Clone)]
pub struct Plan {
    pub write_ops: u64,
    pub applied: u64,
    pub ignored: u64,
    pub duplicates: u64,
    pub rejected: u64,
    pub conflicts: u64,
    pub conflict_list: Vec<Conflict>,
}

impl Plan {
    pub fn all_idempotent(&self) -> bool {
        self.applied == 0 && self.conflicts == 0 && self.rejected == 0
    }

    pub fn to_json(&self) -> String {
        format!(
            "{{\"applied\":{},\"ignored\":{},\"duplicates\":{},\"rejected\":{},\"conflicts\":{}}}",
            self.applied, self.ignored, self.duplicates, self.rejected, self.conflicts
        )
    }
}

fn field_id_bytes(key: &[u8], field: FieldId) -> Vec<u8> {
    let mut out = Vec::with_capacity(key.len() + 2);
    out.extend_from_slice(key);
    out.extend_from_slice(&field.code().to_be_bytes());
    out
}

/// RAII transaction.
///
/// ```text
/// let tx = TransactionGuard::begin(&mut conn)?;
/// let seq = tx.next_op_seq()?;
/// tx.insert_field(...)?;
/// tx.commit()?;            // explicit
/// // any `?` before commit drops the guard, which issues ROLLBACK.
/// ```
pub struct TransactionGuard<'a> {
    connection: &'a mut SyncConnection,
    finished: bool,
}

impl<'a> TransactionGuard<'a> {
    pub fn begin(connection: &'a mut SyncConnection) -> Result<Self> {
        connection.execute("BEGIN IMMEDIATE")?;
        Ok(Self {
            connection,
            finished: false,
        })
    }

    pub fn next_op_seq(&mut self) -> Result<i64> {
        // `RETURNING` keeps the read and the bump atomic; a separate SELECT then
        // UPDATE would let two writers read the same value.
        self.connection
            .query_one("UPDATE seq SET next = next + 1 WHERE name = 'op_seq' RETURNING next")
    }

    pub fn commit(mut self) -> Result<()> {
        self.connection.execute("COMMIT")?;
        self.finished = true;
        Ok(())
    }
}

impl Drop for TransactionGuard<'_> {
    fn drop(&mut self) {
        if !self.finished {
            // Best effort: an error here means the connection is already gone,
            // and the next `BEGIN IMMEDIATE` will surface it.
            let _ = self.connection.execute("ROLLBACK");
        }
    }
}

/// Thin wrapper over the SQLite handle.
///
/// Owned by exactly one thread — the sync worker. That is the structural fix for
/// finding C5 (one `sqlite3*` touched from `crl::async`); the type is not `Sync`
/// because the underlying handle is not, and `!Sync` is enforced by the type
/// system instead of by a comment.
pub struct SyncConnection {
    /// Kept so `self-test` and `status` can report which file a failure came
    /// from without asking the SQLite handle.
    pub path: String,
    handle: Option<*mut core::ffi::c_void>,
}

// The raw pointer is never shared: `SyncConnection` lives on the sync worker.
unsafe impl Send for SyncConnection {}

impl SyncConnection {
    pub fn open(path: &str) -> Result<Self> {
        // TODO(rusqlite): Connection::open(path) with `busy_timeout(5000)`,
        // `journal_mode=WAL`, `synchronous=FULL`, then `execute_batch(SCHEMA_SQL)`.
        // The busy_timeout must be set per connection — H6 is exactly the bug of
        // setting it on only one of several connections.
        let _ = path;
        Err(Error::NotImplemented("rusqlite::Connection::open"))
    }

    pub fn execute(&mut self, sql: &str) -> Result<()> {
        let _ = sql;
        Err(Error::NotImplemented("rusqlite execute"))
    }

    /// `RETURNING`-style single-value query, used by the sequence table.
    pub fn query_one(&mut self, sql: &str) -> Result<i64> {
        let _ = sql;
        Err(Error::NotImplemented("rusqlite query_row"))
    }

    /// Page of ops for a pull, in `op_seq` order.
    pub fn page(&mut self, since_seq: u64, limit: u32) -> Result<Vec<Op>> {
        let _ = (since_seq, limit);
        Err(Error::NotImplemented("rusqlite page query"))
    }

    /// Prune `op_log` below the slowest known device cursor.
    ///
    /// Deleting above it would make a returning device silently miss operations;
    /// `keep_floor` is the minimum `since_seq` across `device_cursor`, computed by
    /// the caller in the same transaction as the delete.
    pub fn prune_upto(&mut self, keep_floor: u64) -> Result<u64> {
        let _ = keep_floor;
        Err(Error::NotImplemented("rusqlite prune"))
    }

    pub fn close(self) {
        drop(self);
    }
}

impl Drop for SyncConnection {
    fn drop(&mut self) {
        if self.handle.is_some() {
            // TODO(rusqlite): the Connection drops itself.
            self.handle = None;
        }
    }
}

/// Anti-entropy summary returned by a cheap `sync.stamp` call.
///
/// Replaces `PgFetchRemoteStamp`'s `sha256` of the whole dataset. A hash of the
/// dataset is expensive to compute (it needs a full scan) and useless for telling
/// *how far* behind a device is; `high_water` answers both "am I behind" and "by
/// how much".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stamp {
    pub high_water: u64,
    pub account_count: u32,
    pub last_write_ms: u64,
}

impl Stamp {
    pub fn to_json(&self) -> String {
        format!(
            "{{\"high_water\":{},\"accounts\":{},\"last_write_ms\":{}}}",
            self.high_water, self.account_count, self.last_write_ms
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sync::model::EntityKey;
    use crate::sync::vclock::VectorClock;

    fn key() -> EntityKey {
        EntityKey::peer([9u8; 16], 100)
    }

    fn op(device: &str, seq: u64, hlc_ms: u64, field: FieldId) -> Op {
        Op {
            op_id: [0u8; 16],
            key: key(),
            field,
            vclock: VectorClock::from_pairs(&[(device, seq)]),
            hlc: Hlc::new(hlc_ms, 0),
            writer_seq: seq,
            tombstone: false,
            value_ct: vec![1, 2, 3],
        }
    }

    fn field_of(_device: &str, seq: u64, hlc_ms: u64, field: FieldId) -> FieldRecord {
        FieldRecord {
            key: key(),
            field,
            version: crate::sync::model::FieldVersion {
                hlc: Hlc::new(hlc_ms, 0),
                writer_seq: seq,
            },
            tombstone: false,
            value_ct: vec![9],
        }
    }

    fn root_of(clock: VectorClock, hlc_ms: u64) -> EntityRoot {
        EntityRoot {
            key: key(),
            vclock: clock,
            last_hlc: Hlc::new(hlc_ms, 0),
            last_seq: 1,
            deleted: false,
            deleted_hlc: None,
        }
    }

    #[test]
    fn no_local_state_applies() {
        let verdict = classify(None, None, &op("a", 1, 100, FieldId::PeerUsername));
        assert_eq!(verdict, WriteVerdict::Applied);
    }

    #[test]
    fn newer_remote_applies() {
        let root = root_of(VectorClock::from_pairs(&[("a", 1)]), 100);
        let local = field_of("a", 1, 100, FieldId::PeerUsername);
        let verdict = classify(
            Some(&root),
            Some(&local),
            &op("a", 2, 200, FieldId::PeerUsername),
        );
        assert_eq!(verdict, WriteVerdict::Applied);
    }

    #[test]
    fn older_remote_is_ignored() {
        let root = root_of(VectorClock::from_pairs(&[("a", 5)]), 500);
        let local = field_of("a", 5, 500, FieldId::PeerUsername);
        let verdict = classify(
            Some(&root),
            Some(&local),
            &op("a", 3, 200, FieldId::PeerUsername),
        );
        assert_eq!(verdict, WriteVerdict::Ignored);
    }

    #[test]
    fn concurrent_same_field_is_a_conflict_not_a_silent_win() {
        let root = root_of(VectorClock::from_pairs(&[("a", 1)]), 100);
        let local = field_of("a", 1, 100, FieldId::PeerUsername);
        let verdict = classify(
            Some(&root),
            Some(&local),
            &op("b", 1, 100, FieldId::PeerUsername),
        );
        assert_eq!(verdict, WriteVerdict::Conflict);
    }

    #[test]
    fn concurrent_disjoint_field_merges_cleanly() {
        let root = root_of(VectorClock::from_pairs(&[("a", 1)]), 100);
        let verdict = classify(Some(&root), None, &op("b", 1, 100, FieldId::PeerBio));
        assert_eq!(verdict, WriteVerdict::Applied);
    }

    #[test]
    fn replay_is_a_duplicate() {
        let root = root_of(VectorClock::from_pairs(&[("a", 2)]), 200);
        let local = field_of("a", 2, 200, FieldId::PeerUsername);
        let mut incoming = op("a", 2, 200, FieldId::PeerUsername);
        incoming.op_id = [7u8; 16];
        let verdict = classify(Some(&root), Some(&local), &incoming);
        assert_eq!(verdict, WriteVerdict::Duplicate);
    }

    #[test]
    fn tombstone_beats_a_stale_write() {
        let mut root = root_of(VectorClock::from_pairs(&[("a", 5)]), 500);
        root.deleted = true;
        root.deleted_hlc = Some(Hlc::new(900, 0));
        let verdict = classify(
            Some(&root),
            Some(&field_of("a", 5, 500, FieldId::PeerUsername)),
            &op("a", 6, 600, FieldId::PeerUsername),
        );
        assert_eq!(verdict, WriteVerdict::RejectedTombstone);
    }

    #[test]
    fn plan_counts_and_keeps_conflicts() {
        let root = root_of(VectorClock::from_pairs(&[("a", 1)]), 100);
        let local = field_of("a", 1, 100, FieldId::PeerUsername);
        let ops = vec![
            op("b", 1, 100, FieldId::PeerUsername),
            op("a", 1, 100, FieldId::PeerUsername),
            op("a", 2, 200, FieldId::PeerBio),
        ];
        let plan = plan(
            &[(key().encode().to_vec(), root)],
            &[(
                {
                    let mut out = key().encode().to_vec();
                    out.extend_from_slice(&FieldId::PeerUsername.code().to_be_bytes());
                    out
                },
                local,
            )],
            &ops,
        )
        .unwrap();
        assert_eq!(plan.conflicts, 1);
        assert_eq!(plan.applied, 1);
        assert_eq!(plan.duplicates, 1);
        assert!(!plan.conflict_list.is_empty());
        assert!(!plan.all_idempotent());
    }

    #[test]
    fn stamp_json_is_stable() {
        let stamp = Stamp {
            high_water: 42,
            account_count: 2,
            last_write_ms: 1_700_000_000_000,
        };
        assert_eq!(
            stamp.to_json(),
            "{\"high_water\":42,\"accounts\":2,\"last_write_ms\":1700000000000}"
        );
    }
}
