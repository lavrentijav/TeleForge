//! Ingest: validate a batch, then write it in one transaction.
//!
//! # Order of checks
//!
//! Validation happens *before* any write, and in this order:
//!
//! 1. `account_id` present (§ "без account_id" must be dropped);
//! 2. `account_id` equals the authenticated account (a foreign account is a
//!    client bug or an attack, never a merge);
//! 3. shape of the event: device id, HLC, content hash, `entity_id` namespace,
//!    field name, value size;
//! 4. duplicate `content_hash` — idempotency, checked both against the store and
//!    against the events already accepted in this same batch;
//! 5. monotone HLC per `(account, device)` — a device that goes backwards would
//!    otherwise be able to insert writes "in the past" and reorder its own
//!    history;
//! 6. entity tombstone — a message revision older than the observed delete is
//!    refused instead of resurrecting deleted content.
//!
//! Only then does the event get an `op_seq` and a row.
//!
//! # One transaction per batch
//!
//! [`ingest_batch`] opens exactly one [`crate::store::conn::Tx`] for the whole
//! batch. A rejected event does not abort the batch (it is a per-event verdict);
//! a *storage* error does, and the whole batch rolls back — a partially applied
//! batch would hand out `op_seq` values for rows that a retry then sees as
//! duplicates of data it does not have.

use crate::error::{Error, Result};
use crate::store::conn::{
    ArchiveInsert, ConflictInsert, EntityUpdate, NewOp, PresenceInsert, ProfileInsert, Store,
};
use crate::store::event::{
    AccountId, EntityClass, EntityId, Event, EventKind, IngestResult, RejectReason,
    ARCHIVE_SOURCES, MAX_ARCHIVE_PAYLOAD_BYTES, MAX_DEVICE_ID_LEN, MAX_PROFILE_VALUE_BYTES,
    MAX_VALUE_BYTES, MESSAGE_FIELDS, PROFILE_FIELDS,
};
use crate::sync::clock::Clock;

/// Upper bound on one batch. Matches the `PageSpec::limit` ceiling from
/// `docs/architecture.md` § 10.5: the agent, not the client, decides how much work
/// one request may cost.
pub const MAX_BATCH_EVENTS: usize = 2_000;

/// What a batch did, in input order plus counters.
#[derive(Clone, Debug, Default)]
pub struct IngestOutcome {
    pub results: Vec<IngestResult>,
    pub applied: u64,
    pub duplicates: u64,
    pub rejected: u64,
    /// Conflicts recorded while applying the batch. A conflict is *not* a
    /// rejection: the event is applied and kept, and the pair is surfaced through
    /// `sync.conflicts` because an HLC-ordered merge would have dropped one side.
    pub conflicts: u64,
    /// Ids of the conflicts recorded by this batch, in input order.
    pub conflict_ids: Vec<u64>,
    /// `accounts.high_water` after the batch, i.e. the largest `op_seq` this
    /// account has ever been given.
    pub high_water: u64,
}

impl IngestOutcome {
    pub fn counts(&self) -> (u64, u64, u64) {
        (self.applied, self.duplicates, self.rejected)
    }

    /// Stable JSON-ish summary, used by `sync.push`-style responses.
    pub fn to_json(&self) -> String {
        format!(
            concat!(
                "{{\"applied\":{},\"duplicates\":{},\"rejected\":{},\"conflicts\":{},",
                "\"high_water\":{}}}"
            ),
            self.applied, self.duplicates, self.rejected, self.conflicts, self.high_water
        )
    }
}

/// Accept a batch at the current wall clock. See [`ingest_batch_at`].
pub fn ingest_batch(
    store: &mut Store,
    scope: &AccountId,
    events: &[Event],
) -> Result<IngestOutcome> {
    let now = crate::sync::clock::SystemClock.now_ms();
    ingest_batch_at(store, scope, events, i64::try_from(now).unwrap_or(i64::MAX))
}

/// Accept a batch, stamping rows with `now_ms`.
///
/// `now_ms` is a parameter, not a call to `SystemTime`, so retention and ordering
/// tests do not depend on the host clock.
pub fn ingest_batch_at(
    store: &mut Store,
    scope: &AccountId,
    events: &[Event],
    now_ms: i64,
) -> Result<IngestOutcome> {
    if events.len() > MAX_BATCH_EVENTS {
        return Err(Error::Invalid(format!(
            "batch of {} events exceeds the limit of {MAX_BATCH_EVENTS}",
            events.len()
        )));
    }
    if events.is_empty() {
        let high_water = store.transaction(|tx| tx.high_water(scope))?;
        return Ok(IngestOutcome {
            high_water,
            ..IngestOutcome::default()
        });
    }

    store.transaction(|tx| ingest_batch_in_tx(tx, scope, events, now_ms))
}

/// The body of [`ingest_batch_at`], for callers that already own a transaction.
///
/// The server-side DB ingest (`crate::db::ingest`) is the reason this exists: it
/// must write the `ops` rows **and** the `db_cursor` row in one transaction, so
/// "published but not recorded" cannot happen. Nested `Store::transaction` calls
/// are impossible (SQLite has no nested transactions here), so the batch core is
/// exposed instead of opening a second one.
pub fn ingest_batch_in_tx(
    tx: &crate::store::conn::Tx<'_>,
    scope: &AccountId,
    events: &[Event],
    now_ms: i64,
) -> Result<IngestOutcome> {
    if events.len() > MAX_BATCH_EVENTS {
        return Err(Error::Invalid(format!(
            "batch of {} events exceeds the limit of {MAX_BATCH_EVENTS}",
            events.len()
        )));
    }

    let mut outcome = IngestOutcome {
        results: Vec::with_capacity(events.len()),
        ..IngestOutcome::default()
    };
    // `content_hash -> op_seq` for the events accepted *in this batch*, so a
    // batch that repeats one event twice applies it once.
    let mut accepted: std::collections::BTreeMap<[u8; 32], u64> = std::collections::BTreeMap::new();

    for event in events {
        let valid = match validate(scope, event) {
            Ok(valid) => valid,
            Err(reason) => {
                outcome.rejected += 1;
                outcome.results.push(IngestResult::Rejected { reason });
                continue;
            }
        };

        if let Some(op_seq) = accepted.get(&event.content_hash) {
            outcome.duplicates += 1;
            outcome
                .results
                .push(IngestResult::Duplicate { op_seq: *op_seq });
            continue;
        }
        if let Some(op_seq) = tx.op_seq_for_content_hash(scope, &event.content_hash)? {
            accepted.insert(event.content_hash, op_seq);
            outcome.duplicates += 1;
            outcome.results.push(IngestResult::Duplicate { op_seq });
            continue;
        }

        let last_hlc = tx.device_last_hlc(scope, &event.device_id)?;
        if let Some(last) = last_hlc {
            if event.hlc <= last {
                outcome.rejected += 1;
                outcome.results.push(IngestResult::Rejected {
                    reason: RejectReason::NonMonotonicHlc,
                });
                continue;
            }
        }

        // A delete observed at HLC T forbids any *older* revision from
        // appearing afterwards. Refusing is the whole point of a tombstone:
        // `RejectedTombstone` in `sync::engine` is the same rule.
        let is_delete = event.kind.map(EventKind::is_delete).unwrap_or(false);
        if event.kind.map(EventKind::is_message).unwrap_or(false) && !is_delete {
            if let Some(deleted_hlc) = tx.entity_deleted_hlc(scope, &event.entity_id)? {
                if event.hlc <= deleted_hlc {
                    outcome.rejected += 1;
                    outcome.results.push(IngestResult::Rejected {
                        reason: RejectReason::Tombstoned,
                    });
                    continue;
                }
            }
        }

        // Cross-device concurrency, detected without vector clocks.
        //
        // A passive event carries a per-device HLC and no causal history, so the
        // only defensible statement is: if *another* device already wrote this
        // exact field with a strictly newer clock, an HLC-ordered merge would
        // drop the write that just arrived. Both values are kept (the journal is
        // append-only) and the pair is recorded, because silently keeping the
        // "newer" one is exactly the LWW behaviour H7 is about.
        let conflict_with = match event.field.as_deref() {
            Some(field) => tx
                .newest_field_op(scope, &event.entity_id, field)?
                .filter(|known| known.device_id != event.device_id && event.hlc < known.hlc),
            None => None,
        };

        let op_seq = tx.next_op_seq(scope, now_ms)?;
        let kind = event.kind.ok_or_else(|| {
            // `validate` already rejects a missing kind; this is unreachable
            // and returns an error instead of unwrapping in a write path.
            Error::Storage("validated event lost its kind".to_string())
        })?;
        let observed_ms = event.hlc_physical_ms();

        tx.insert_op(&NewOp {
            op_seq,
            account: *scope,
            kind,
            entity_id: &event.entity_id,
            field: event.field.as_deref(),
            value: &event.value,
            hlc: event.hlc,
            device_id: &event.device_id,
            content_hash: event.content_hash,
            applied_at_ms: now_ms,
        })?;

        let is_delete = kind.is_delete();
        tx.upsert_entity(&EntityUpdate {
            account: *scope,
            entity_id: &event.entity_id,
            kind,
            op_seq,
            hlc: event.hlc,
            observed_ms,
            deleted: is_delete,
            deleted_hlc: if is_delete { Some(event.hlc) } else { None },
        })?;

        project(
            tx,
            scope,
            &event.entity_id,
            &valid.entity,
            event,
            op_seq,
            observed_ms,
        )?;

        tx.update_device_clock(
            scope,
            &event.device_id,
            event.hlc,
            now_ms,
            last_hlc.is_none(),
        )?;

        if let Some(known) = conflict_with {
            let conflict_id = tx.next_conflict_id(scope, now_ms)?;
            tx.insert_conflict(&ConflictInsert {
                account: *scope,
                conflict_id,
                entity_id: &event.entity_id,
                field: event.field.as_deref(),
                kind,
                local_op_seq: known.op_seq,
                local_hlc: known.hlc,
                local_device: &known.device_id,
                remote_op_seq: op_seq,
                remote_hlc: event.hlc,
                remote_device: &event.device_id,
                detected_at_ms: now_ms,
            })?;
            outcome.conflicts += 1;
            outcome.conflict_ids.push(conflict_id);
        }

        accepted.insert(event.content_hash, op_seq);
        outcome.applied += 1;
        outcome.results.push(IngestResult::Applied { op_seq });
    }

    outcome.high_water = tx.high_water(scope)?;
    Ok(outcome)
}

/// Accept a single event. Convenience wrapper that keeps the "one batch, one
/// transaction" rule.
pub fn ingest(store: &mut Store, scope: &AccountId, event: &Event) -> Result<IngestResult> {
    let outcome = ingest_batch(store, scope, std::slice::from_ref(event))?;
    Ok(outcome
        .results
        .into_iter()
        .next()
        .unwrap_or(IngestResult::Rejected {
            reason: RejectReason::MissingAccount,
        }))
}

/// Write the typed projection row for events that have one.
///
/// Messages deliberately have **no** projection table: the journal *is* the
/// message history, and a "current message" table is exactly the row that
/// `MergeDataDb` overwrote (H7).
fn project(
    tx: &crate::store::conn::Tx<'_>,
    scope: &AccountId,
    entity_id: &str,
    entity: &EntityId,
    event: &Event,
    op_seq: u64,
    observed_ms: i64,
) -> Result<()> {
    // The projection must mirror the journal row; if these ever disagree, a
    // history query and an ops query would tell different stories.
    debug_assert_eq!(entity_id, entity.text);
    match entity.class {
        EntityClass::Message => Ok(()),
        EntityClass::Presence => {
            let detail_ms = presence_detail(&event.value);
            tx.insert_presence(&PresenceInsert {
                account: *scope,
                op_seq,
                peer_id: entity.primary,
                status: event.kind.map(EventKind::code).unwrap_or(0),
                detail_ms,
                observed_ms,
                hlc: event.hlc,
                device_id: &event.device_id,
                content_hash: event.content_hash,
            })
        }
        EntityClass::Profile => tx.insert_profile(&ProfileInsert {
            account: *scope,
            op_seq,
            peer_id: entity.primary,
            field: event.field.as_deref().unwrap_or_default(),
            value: &event.value,
            observed_ms,
            hlc: event.hlc,
            device_id: &event.device_id,
            content_hash: event.content_hash,
        }),
        EntityClass::Archive => tx.insert_archive(&ArchiveInsert {
            account: *scope,
            op_seq,
            peer_id: entity.primary,
            source: event.field.as_deref().unwrap_or_default(),
            payload: &event.value,
            observed_ms,
            hlc: event.hlc,
            device_id: &event.device_id,
            content_hash: event.content_hash,
        }),
    }
}

/// Presence payload is one big-endian `i64`: `online_till` for an online
/// observation, `last_seen` for an offline one. All-zero means "the client did not
/// know", which is stored as NULL rather than as the epoch.
fn presence_detail(value: &[u8]) -> Option<i64> {
    if value.len() != 8 {
        return None;
    }
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(value);
    let detail = i64::from_be_bytes(bytes);
    if detail == 0 {
        None
    } else {
        Some(detail)
    }
}

/// A validated event: the parsed entity. The field name is re-read from the event
/// by the projection, so nothing is copied for a rejected event.
struct Valid {
    entity: EntityId,
}

fn validate(scope: &AccountId, event: &Event) -> std::result::Result<Valid, RejectReason> {
    // 1. Account identity.
    match event.account_id {
        None => return Err(RejectReason::MissingAccount),
        Some(account) if account != *scope => return Err(RejectReason::ForeignAccount),
        Some(_) => {}
    }

    // 2. Device identity.
    if event.device_id.is_empty()
        || event.device_id.len() > MAX_DEVICE_ID_LEN
        || !event
            .device_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    {
        return Err(RejectReason::MalformedDevice);
    }

    // 3. Clock and idempotency key.
    if event.hlc.is_zero() {
        return Err(RejectReason::MissingHlc);
    }
    if event.content_hash.iter().all(|byte| *byte == 0) {
        return Err(RejectReason::MissingContentHash);
    }

    // 4. Entity address.
    let kind = event.kind.ok_or(RejectReason::MissingKind)?;
    let entity = EntityId::parse(&event.entity_id).map_err(|_| RejectReason::MalformedEntityId)?;
    if entity.class != kind.class() {
        return Err(RejectReason::KindEntityMismatch);
    }

    // 5. Field and value, per kind.
    let field = event.field.as_deref();
    match kind {
        EventKind::MessageNew | EventKind::MessageEdit => {
            let name = field.ok_or(RejectReason::MissingField)?;
            if !MESSAGE_FIELDS.contains(&name) {
                return Err(RejectReason::UnknownField);
            }
            if event.value.len() > MAX_VALUE_BYTES {
                return Err(RejectReason::OversizedValue);
            }
        }
        EventKind::MessageDelete => {
            if field.is_some() {
                return Err(RejectReason::UnexpectedField);
            }
            if event.value.len() > MAX_VALUE_BYTES {
                return Err(RejectReason::OversizedValue);
            }
        }
        EventKind::PresenceOnline | EventKind::PresenceOffline => {
            if field.is_some() {
                return Err(RejectReason::UnexpectedField);
            }
            if event.value.len() != 8 {
                return Err(RejectReason::MalformedPresencePayload);
            }
        }
        EventKind::ProfileUpdate => {
            let name = field.ok_or(RejectReason::MissingField)?;
            if !PROFILE_FIELDS.contains(&name) {
                return Err(RejectReason::UnknownField);
            }
            if event.value.len() > MAX_PROFILE_VALUE_BYTES {
                return Err(RejectReason::OversizedValue);
            }
        }
        EventKind::ArchiveObservation => {
            let source = field.ok_or(RejectReason::MissingField)?;
            if !ARCHIVE_SOURCES.contains(&source) {
                return Err(RejectReason::UnknownField);
            }
            if event.value.len() > MAX_ARCHIVE_PAYLOAD_BYTES {
                return Err(RejectReason::OversizedValue);
            }
        }
    }

    Ok(Valid { entity })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::query;
    use crate::sync::clock::Hlc;

    fn account() -> AccountId {
        AccountId::new([0xa1; 16]).unwrap()
    }

    fn other_account() -> AccountId {
        AccountId::new([0xb2; 16]).unwrap()
    }

    fn store() -> Store {
        Store::open_in_memory().unwrap()
    }

    fn message_new(account: AccountId, hlc: Hlc, text: &[u8]) -> Event {
        Event::new(
            account,
            "dev-a",
            EventKind::MessageNew,
            "message:-1001234567890:42",
            Some("text"),
            text.to_vec(),
            hlc,
        )
    }

    fn presence(account: AccountId, device: &str, hlc: Hlc, online_till: i64) -> Event {
        Event::new(
            account,
            device,
            EventKind::PresenceOnline,
            "presence:900",
            None,
            online_till.to_be_bytes().to_vec(),
            hlc,
        )
    }

    fn profile(account: AccountId, device: &str, hlc: Hlc, field: &str, value: &[u8]) -> Event {
        Event::new(
            account,
            device,
            EventKind::ProfileUpdate,
            "profile:900",
            Some(field),
            value.to_vec(),
            hlc,
        )
    }

    #[test]
    fn a_batch_of_fresh_events_applies_and_numbers_them() {
        let mut store = store();
        let account = account();
        let events = vec![
            message_new(account, Hlc::new(1_000, 0), b"first"),
            presence(account, "dev-a", Hlc::new(1_001, 0), 2_000),
            profile(account, "dev-a", Hlc::new(1_002, 0), "username", b"@neo"),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (3, 0, 0));
        assert_eq!(outcome.high_water, 3);
        assert_eq!(
            outcome
                .results
                .iter()
                .map(IngestResult::encode)
                .collect::<Vec<_>>(),
            vec!["applied:1", "applied:2", "applied:3"]
        );
        assert_eq!(query::op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn replaying_the_same_batch_adds_no_rows() {
        let mut store = store();
        let account = account();
        let events = vec![
            message_new(account, Hlc::new(1_000, 0), b"first"),
            presence(account, "dev-a", Hlc::new(1_001, 0), 2_000),
        ];
        let first = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(first.counts(), (2, 0, 0));

        let second = ingest_batch_at(&mut store, &account, &events, 6_000).unwrap();
        assert_eq!(
            second.counts(),
            (0, 2, 0),
            "a retried batch must be all duplicates, not re-applied"
        );
        assert_eq!(query::op_count(&store, &account).unwrap(), 2);
        assert_eq!(second.high_water, 2);
        // The duplicate reports the *original* sequence number.
        assert_eq!(second.results[0], IngestResult::Duplicate { op_seq: 1 });
    }

    #[test]
    fn one_event_repeated_inside_a_batch_applies_once() {
        let mut store = store();
        let account = account();
        let event = message_new(account, Hlc::new(1_000, 0), b"once");
        let outcome =
            ingest_batch_at(&mut store, &account, &[event.clone(), event], 5_000).unwrap();
        assert_eq!(outcome.counts(), (1, 1, 0));
        assert_eq!(query::op_count(&store, &account).unwrap(), 1);
    }

    #[test]
    fn op_seq_is_monotone_across_batches() {
        let mut store = store();
        let account = account();
        let mut expected = 1u64;
        for step in 0..5 {
            let event = message_new(account, Hlc::new(1_000 + step, 0), b"x");
            let outcome = ingest_batch_at(&mut store, &account, &[event], 9_000).unwrap();
            match outcome.results[0] {
                IngestResult::Applied { op_seq } => {
                    assert_eq!(op_seq, expected);
                    expected += 1;
                }
                ref other => panic!("unexpected {other:?}"),
            }
        }
        assert_eq!(query::op_count(&store, &account).unwrap(), 5);
    }

    #[test]
    fn an_event_for_another_account_is_rejected_not_stored() {
        let mut store = store();
        let account = account();
        let events = vec![
            message_new(other_account(), Hlc::new(1_000, 0), b"wrong account"),
            presence(other_account(), "dev-a", Hlc::new(1_001, 0), 1),
            message_new(account, Hlc::new(1_002, 0), b"right account"),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 2));
        assert_eq!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::ForeignAccount
            }
        );
        assert_eq!(query::op_count(&store, &account).unwrap(), 1);
        // Nothing was created for the foreign account either.
        assert_eq!(query::op_count(&store, &other_account()).unwrap(), 0);
        assert_eq!(query::accounts(&store).unwrap().len(), 1);
    }

    #[test]
    fn an_event_without_an_account_is_rejected() {
        let mut store = store();
        let account = account();
        let mut event = message_new(account, Hlc::new(1_000, 0), b"orphan");
        event.account_id = None;
        let outcome = ingest_batch_at(&mut store, &account, &[event], 5_000).unwrap();
        assert_eq!(outcome.counts(), (0, 0, 1));
        assert_eq!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::MissingAccount
            }
        );
        assert_eq!(query::op_count(&store, &account).unwrap(), 0);
    }

    #[test]
    fn a_device_that_goes_backwards_is_rejected() {
        let mut store = store();
        let account = account();
        let newer = presence(account, "dev-a", Hlc::new(2_000, 0), 10);
        let older = presence(account, "dev-a", Hlc::new(1_000, 0), 20);
        let outcome = ingest_batch_at(&mut store, &account, &[newer], 5_000).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0));

        let outcome = ingest_batch_at(&mut store, &account, &[older], 5_001).unwrap();
        assert_eq!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::NonMonotonicHlc
            }
        );

        // The same HLC as the last one is not "monotone" either.
        let equal = presence(account, "dev-a", Hlc::new(2_000, 0), 30);
        let outcome = ingest_batch_at(&mut store, &account, &[equal], 5_002).unwrap();
        assert!(matches!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::NonMonotonicHlc
            }
        ));
    }

    #[test]
    fn two_devices_may_each_have_their_own_clock() {
        let mut store = store();
        let account = account();
        let events = vec![
            presence(account, "dev-a", Hlc::new(2_000, 0), 10),
            // Older physical time, but a *different* device: accepted.
            presence(account, "dev-b", Hlc::new(1_000, 0), 20),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (2, 0, 0));
    }

    #[test]
    fn malformed_shapes_are_rejected_with_distinct_reasons() {
        let mut store = store();
        let account = account();
        let mut missing_kind = message_new(account, Hlc::new(1_000, 0), b"x");
        missing_kind.kind = None;

        let mut bad_device = message_new(account, Hlc::new(1_001, 0), b"x");
        bad_device.device_id = "dev a".to_string();

        let mut missing_hlc = message_new(account, Hlc::new(1_002, 0), b"x");
        missing_hlc.hlc = Hlc::ZERO;

        let mut bad_hash = message_new(account, Hlc::new(1_003, 0), b"x");
        bad_hash.content_hash = [0u8; 32];

        let mut bad_entity = message_new(account, Hlc::new(1_004, 0), b"x");
        bad_entity.entity_id = "chat:1".to_string();

        let mut wrong_namespace = message_new(account, Hlc::new(1_005, 0), b"x");
        wrong_namespace.entity_id = "presence:1".to_string();

        let mut unknown_field = message_new(account, Hlc::new(1_006, 0), b"x");
        unknown_field.field = Some("txt".to_string());

        let mut missing_field = message_new(account, Hlc::new(1_007, 0), b"x");
        missing_field.field = None;

        let mut unexpected_field = message_new(account, Hlc::new(1_008, 0), b"x");
        unexpected_field.kind = Some(EventKind::MessageDelete);
        unexpected_field.field = Some("text".to_string());

        let events = vec![
            missing_kind,
            bad_device,
            missing_hlc,
            bad_hash,
            bad_entity,
            wrong_namespace,
            unknown_field,
            missing_field,
            unexpected_field,
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (0, 0, 9));
        let reasons: Vec<RejectReason> = outcome
            .results
            .iter()
            .map(|result| match result {
                IngestResult::Rejected { reason } => *reason,
                other => panic!("expected a rejection, got {other:?}"),
            })
            .collect();
        assert_eq!(
            reasons,
            vec![
                RejectReason::MissingKind,
                RejectReason::MalformedDevice,
                RejectReason::MissingHlc,
                RejectReason::MissingContentHash,
                RejectReason::MalformedEntityId,
                RejectReason::KindEntityMismatch,
                RejectReason::UnknownField,
                RejectReason::MissingField,
                RejectReason::UnexpectedField,
            ]
        );
        assert_eq!(query::op_count(&store, &account).unwrap(), 0);
    }

    #[test]
    fn oversized_values_are_rejected_per_class() {
        let mut store = store();
        let account = account();
        let big_message = Event::new(
            account,
            "dev-a",
            EventKind::MessageEdit,
            "message:1:1",
            Some("text"),
            vec![0u8; MAX_VALUE_BYTES + 1],
            Hlc::new(1_000, 0),
        );
        let big_profile = profile(
            account,
            "dev-a",
            Hlc::new(1_001, 0),
            "bio",
            &vec![0u8; MAX_PROFILE_VALUE_BYTES + 1],
        );
        let outcome =
            ingest_batch_at(&mut store, &account, &[big_message, big_profile], 5_000).unwrap();
        assert_eq!(outcome.counts(), (0, 0, 2));
        assert!(outcome.results.iter().all(|result| matches!(
            result,
            IngestResult::Rejected {
                reason: RejectReason::OversizedValue
            }
        )));
    }

    #[test]
    fn presence_payload_must_be_one_i64() {
        let mut store = store();
        let account = account();
        let short = Event::new(
            account,
            "dev-a",
            EventKind::PresenceOffline,
            "presence:1",
            None,
            vec![1, 2, 3],
            Hlc::new(1_000, 0),
        );
        let outcome = ingest_batch_at(&mut store, &account, &[short], 5_000).unwrap();
        assert_eq!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::MalformedPresencePayload
            }
        );
    }

    #[test]
    fn history_is_append_only_never_last_write_wins() {
        // H7 in one test: a new message, two edits and a delete are four facts.
        let mut store = store();
        let account = account();
        let events = vec![
            message_new(account, Hlc::new(1_000, 0), b"original"),
            Event::new(
                account,
                "dev-a",
                EventKind::MessageEdit,
                "message:-1001234567890:42",
                Some("text"),
                b"first edit".to_vec(),
                Hlc::new(1_001, 0),
            ),
            Event::new(
                account,
                "dev-a",
                EventKind::MessageEdit,
                "message:-1001234567890:42",
                Some("text"),
                b"second edit".to_vec(),
                Hlc::new(1_002, 0),
            ),
            Event::new(
                account,
                "dev-a",
                EventKind::MessageDelete,
                "message:-1001234567890:42",
                None,
                Vec::new(),
                Hlc::new(1_003, 0),
            ),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (4, 0, 0));

        let ops = query::entity_ops(&store, &account, "message:-1001234567890:42").unwrap();
        assert_eq!(ops.len(), 4, "every revision must be kept");
        assert_eq!(ops[0].value, b"original");
        assert_eq!(ops[1].value, b"first edit");
        assert_eq!(ops[2].value, b"second edit");
        assert_eq!(ops[3].kind, EventKind::MessageDelete);

        let state = query::entity_state(&store, &account, "message:-1001234567890:42")
            .unwrap()
            .expect("entity must exist");
        assert_eq!(state.revision_count, 4);
        assert!(state.deleted);
        assert_eq!(state.last_op_seq, 4);
    }

    #[test]
    fn a_revision_older_than_the_delete_cannot_resurrect_the_message() {
        let mut store = store();
        let account = account();
        let delete = Event::new(
            account,
            "dev-b",
            EventKind::MessageDelete,
            "message:1:1",
            None,
            Vec::new(),
            Hlc::new(5_000, 0),
        );
        ingest_batch_at(&mut store, &account, &[delete], 5_000).unwrap();

        let stale_edit = Event::new(
            account,
            "dev-a",
            EventKind::MessageEdit,
            "message:1:1",
            Some("text"),
            b"zombie".to_vec(),
            Hlc::new(4_000, 0),
        );
        let outcome = ingest_batch_at(&mut store, &account, &[stale_edit], 5_001).unwrap();
        assert_eq!(
            outcome.results[0],
            IngestResult::Rejected {
                reason: RejectReason::Tombstoned
            }
        );
        let state = query::entity_state(&store, &account, "message:1:1")
            .unwrap()
            .unwrap();
        assert!(state.deleted);
        assert_eq!(state.revision_count, 1);
    }

    #[test]
    fn a_genuinely_newer_revision_after_a_delete_is_recorded() {
        let mut store = store();
        let account = account();
        let delete = Event::new(
            account,
            "dev-a",
            EventKind::MessageDelete,
            "message:1:1",
            None,
            Vec::new(),
            Hlc::new(5_000, 0),
        );
        ingest_batch_at(&mut store, &account, &[delete], 5_000).unwrap();
        let recreate = Event::new(
            account,
            "dev-a",
            EventKind::MessageNew,
            "message:1:1",
            Some("text"),
            b"new".to_vec(),
            Hlc::new(6_000, 0),
        );
        let outcome = ingest_batch_at(&mut store, &account, &[recreate], 6_000).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0));
        let state = query::entity_state(&store, &account, "message:1:1")
            .unwrap()
            .unwrap();
        assert_eq!(state.revision_count, 2);
        assert!(!state.deleted, "the newest op decides the projection");
    }

    #[test]
    fn presence_profile_and_archive_get_projection_rows() {
        let mut store = store();
        let account = account();
        let events = vec![
            presence(account, "dev-a", Hlc::new(1_000, 0), 2_000),
            profile(account, "dev-a", Hlc::new(1_001, 0), "username", b"@neo"),
            Event::new(
                account,
                "dev-a",
                EventKind::ArchiveObservation,
                "archive:900",
                Some("participant"),
                b"chat=-1001234567890;msg=7".to_vec(),
                Hlc::new(1_002, 0),
            ),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        assert_eq!(outcome.counts(), (3, 0, 0));

        let presence = query::presence_history(&store, &account, 900, 10).unwrap();
        assert_eq!(presence.len(), 1);
        assert_eq!(presence[0].status, EventKind::PresenceOnline.code());
        assert_eq!(presence[0].detail_ms, Some(2_000));

        let profiles = query::profile_history(&store, &account, 900, "username").unwrap();
        assert_eq!(profiles.len(), 1);
        assert_eq!(profiles[0].value, b"@neo");

        let archive = query::archive_observations(&store, &account, 900).unwrap();
        assert_eq!(archive.len(), 1);
        assert_eq!(archive[0].source, "participant");
    }

    #[test]
    fn presence_history_keeps_every_transition() {
        let mut store = store();
        let account = account();
        for (index, till) in [1_000i64, 2_000, 3_000].iter().enumerate() {
            let event = presence(account, "dev-a", Hlc::new(1_000 + index as u64, 0), *till);
            ingest_batch_at(&mut store, &account, &[event], 9_000).unwrap();
        }
        let history = query::presence_history(&store, &account, 900, 10).unwrap();
        assert_eq!(history.len(), 3);
        // Newest first.
        assert_eq!(history[0].detail_ms, Some(3_000));
        assert_eq!(history[2].detail_ms, Some(1_000));
        assert_eq!(
            query::presence_latest(&store, &account, 900)
                .unwrap()
                .unwrap()
                .detail_ms,
            Some(3_000)
        );
    }

    #[test]
    fn an_empty_batch_is_a_no_op() {
        let mut store = store();
        let account = account();
        let outcome = ingest_batch_at(&mut store, &account, &[], 5_000).unwrap();
        assert_eq!(outcome.counts(), (0, 0, 0));
        assert_eq!(outcome.results.len(), 0);
        assert_eq!(outcome.high_water, 0);
    }

    #[test]
    fn an_oversized_batch_is_refused_before_touching_the_store() {
        let mut store = store();
        let account = account();
        let events: Vec<Event> = (0..=MAX_BATCH_EVENTS)
            .map(|index| presence(account, "dev-a", Hlc::new(1_000 + index as u64, 0), 1))
            .collect();
        let error = ingest_batch_at(&mut store, &account, &events, 5_000).unwrap_err();
        assert!(error.to_string().contains("exceeds the limit"));
        assert_eq!(query::op_count(&store, &account).unwrap(), 0);
    }

    #[test]
    fn a_newer_write_from_another_device_is_kept_and_recorded_as_a_conflict() {
        let mut store = store();
        let account = account();
        // Device A is ahead on the clock.
        let newer = profile(account, "dev-a", Hlc::new(2_000, 0), "username", b"@new");
        ingest_batch_at(&mut store, &account, &[newer], 5_000).unwrap();

        // Device B captured an older value. It is *applied* (the journal never
        // loses a capture) and the pair is surfaced: an HLC-ordered merge would
        // have dropped it silently, which is H7.
        let older = profile(account, "dev-b", Hlc::new(1_000, 0), "username", b"@old");
        let outcome = ingest_batch_at(&mut store, &account, &[older], 5_001).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0), "a conflict is not a rejection");
        assert_eq!(outcome.conflicts, 1);
        assert_eq!(outcome.conflict_ids, vec![1]);
        assert_eq!(
            query::profile_history(&store, &account, 900, "username")
                .unwrap()
                .len(),
            2,
            "both values must stay in the journal"
        );

        // The same write from device A again (a new HLC, so not a duplicate) is not
        // a conflict: the same device's clock is monotone by construction.
        let own_later = profile(account, "dev-a", Hlc::new(2_500, 0), "username", b"@newer");
        let outcome = ingest_batch_at(&mut store, &account, &[own_later], 5_002).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0));
        assert_eq!(outcome.conflicts, 0);
    }

    #[test]
    fn a_newer_write_from_another_device_is_not_a_conflict() {
        let mut store = store();
        let account = account();
        let first = profile(account, "dev-a", Hlc::new(1_000, 0), "bio", b"old bio");
        ingest_batch_at(&mut store, &account, &[first], 5_000).unwrap();
        // Device B's observation is newer, so a merge would keep it: nothing to
        // surface.
        let second = profile(account, "dev-b", Hlc::new(2_000, 0), "bio", b"new bio");
        let outcome = ingest_batch_at(&mut store, &account, &[second], 5_001).unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0));
        assert_eq!(outcome.conflicts, 0);
    }

    #[test]
    fn conflicts_are_per_field_and_per_entity() {
        let mut store = store();
        let account = account();
        let events = vec![
            profile(account, "dev-a", Hlc::new(2_000, 0), "username", b"@a"),
            profile(account, "dev-a", Hlc::new(2_000, 1), "bio", b"bio a"),
        ];
        ingest_batch_at(&mut store, &account, &events, 5_000).unwrap();
        // A different field is a disjoint edit, not a conflict.
        let outcome = ingest_batch_at(
            &mut store,
            &account,
            &[profile(
                account,
                "dev-b",
                Hlc::new(1_000, 0),
                "first_name",
                b"neo",
            )],
            5_001,
        )
        .unwrap();
        assert_eq!(outcome.conflicts, 0);
        // ... while the same field is.
        let outcome = ingest_batch_at(
            &mut store,
            &account,
            &[profile(
                account,
                "dev-b",
                Hlc::new(1_500, 0),
                "bio",
                b"bio b",
            )],
            5_002,
        )
        .unwrap();
        assert_eq!(outcome.conflicts, 1);
        assert_eq!(outcome.conflict_ids, vec![1]);
    }

    #[test]
    fn presence_carries_no_field_and_therefore_no_conflicts() {
        let mut store = store();
        let account = account();
        ingest_batch_at(
            &mut store,
            &account,
            &[presence(account, "dev-a", Hlc::new(2_000, 0), 10)],
            5_000,
        )
        .unwrap();
        let outcome = ingest_batch_at(
            &mut store,
            &account,
            &[presence(account, "dev-b", Hlc::new(1_000, 0), 20)],
            5_001,
        )
        .unwrap();
        assert_eq!(outcome.counts(), (1, 0, 0));
        assert_eq!(outcome.conflicts, 0);
    }

    #[test]
    fn a_replayed_batch_does_not_duplicate_its_conflicts() {
        let mut store = store();
        let account = account();
        ingest_batch_at(
            &mut store,
            &account,
            &[profile(
                account,
                "dev-a",
                Hlc::new(2_000, 0),
                "username",
                b"@a",
            )],
            5_000,
        )
        .unwrap();
        let conflicting = profile(account, "dev-b", Hlc::new(1_000, 0), "username", b"@b");
        let first = ingest_batch_at(
            &mut store,
            &account,
            std::slice::from_ref(&conflicting),
            5_001,
        )
        .unwrap();
        assert_eq!(first.conflicts, 1);
        let second = ingest_batch_at(&mut store, &account, &[conflicting], 5_002).unwrap();
        assert_eq!(second.counts(), (0, 1, 0), "the replay is a duplicate");
        assert_eq!(
            second.conflicts, 0,
            "a duplicate must not re-record a conflict"
        );
        assert_eq!(
            crate::store::query::conflict_list(&store, &account, 0, 10, true)
                .unwrap()
                .len(),
            1
        );
    }

    #[test]
    fn single_event_ingest_reports_the_verdict() {
        let mut store = store();
        let account = account();
        let event = message_new(account, Hlc::new(1_000, 0), b"one");
        let verdict = ingest(&mut store, &account, &event).unwrap();
        assert_eq!(verdict, IngestResult::Applied { op_seq: 1 });
        let again = ingest(&mut store, &account, &event).unwrap();
        assert_eq!(again, IngestResult::Duplicate { op_seq: 1 });
    }

    #[test]
    fn outcome_json_is_stable() {
        let outcome = IngestOutcome {
            results: Vec::new(),
            applied: 2,
            duplicates: 1,
            rejected: 0,
            conflicts: 1,
            conflict_ids: vec![4],
            high_water: 7,
        };
        assert_eq!(
            outcome.to_json(),
            "{\"applied\":2,\"duplicates\":1,\"rejected\":0,\"conflicts\":1,\"high_water\":7}"
        );
    }

    #[test]
    fn presence_detail_zero_means_unknown() {
        assert_eq!(presence_detail(&0i64.to_be_bytes()), None);
        assert_eq!(presence_detail(&5i64.to_be_bytes()), Some(5));
        assert_eq!(presence_detail(&[1, 2]), None);
    }

    #[test]
    fn a_batch_and_a_cursor_can_share_one_transaction() {
        // The shape the server-side DB ingest relies on: the events and the read
        // cursor commit together, so "published but not recorded" is impossible.
        let mut store = store();
        let account = account();
        let event = message_new(account, Hlc::new(1_000, 0), b"one");
        let stored = store
            .transaction(|tx| {
                let outcome =
                    super::ingest_batch_in_tx(tx, &account, std::slice::from_ref(&event), 5_000)?;
                let cursor =
                    crate::store::cursor::advance_in_tx(tx, &account, "sync_outbox", 42, 5_000)?;
                Ok((outcome, cursor))
            })
            .unwrap();
        assert_eq!(stored.0.applied, 1);
        assert_eq!(stored.1.last_key, 42);
        assert_eq!(crate::store::query::op_count(&store, &account).unwrap(), 1);
        assert_eq!(
            crate::store::cursor::read(&store, &account, "sync_outbox")
                .unwrap()
                .unwrap()
                .last_key,
            42
        );
    }

    #[test]
    fn a_failed_batch_rolls_the_cursor_back_with_it() {
        // A storage failure inside the shared transaction must leave *neither* the
        // `ops` row nor the cursor behind: a cursor that moved without its events
        // is a hole in the journal.
        let mut store = store();
        let account = account();
        let event = message_new(account, Hlc::new(1_000, 0), b"one");
        let failure = store.transaction(|tx| {
            super::ingest_batch_in_tx(tx, &account, std::slice::from_ref(&event), 5_000)?;
            crate::store::cursor::advance_in_tx(tx, &account, "sync_outbox", 42, 5_000)?;
            Err::<(), crate::error::Error>(crate::error::Error::Storage("boom".to_string()))
        });
        assert!(failure.is_err());
        assert!(store.is_idle(), "the transaction must be closed");
        assert_eq!(crate::store::query::op_count(&store, &account).unwrap(), 0);
        assert_eq!(
            crate::store::cursor::read(&store, &account, "sync_outbox").unwrap(),
            None
        );
    }
}
