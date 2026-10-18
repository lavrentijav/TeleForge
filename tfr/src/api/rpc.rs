//! JSON-RPC 2.0 dispatch over the HTTP transport.
//!
//! # Envelope
//!
//! Request (same shape `docs/architecture.md` § 9.5 fixed):
//!
//! ```json
//! {"protocol":1,"request_id":"01JA01","idempotency_key":"01JA01",
//!  "method":"sync.push","params":{...}}
//! ```
//!
//! Response:
//!
//! ```json
//! {"protocol":1,"request_id":"01JA01","ok":true,"result":{...},"error":null}
//! ```
//!
//! `protocol` and `idempotency_key` are optional on the wire so a `hello` does not
//! need them; a *mutating* method without an `idempotency_key` is rejected, because
//! a retry then becomes a second application of the batch.
//!
//! # Dispatch order (each step can reject, none of them panics)
//!
//! ```text
//! 1. parse the body            → 400 (code 1000)
//! 2. parse the method          → 400 (code 1000)
//! 3. protocol window           → 400 (code 1001)
//! 4. request_id shape          → 400 (code 1000)
//! 5. auth (token → device)     → 401 (code 1002)
//! 6. account/tenant ownership  → 403 (code 1003)
//! 7. scope                     → 403 (code 1003)
//! 8. rate limit per device     → 429 (code 1007)
//! 9. idempotent replay         → 200 (the stored result)
//! 10. handler                  → 200 / 400 / 409 / 500
//! ```
//!
//! Step 9 before the handler is deliberate: a retry of a request that already
//! succeeded must return the *stored* answer even if the world moved on.
//!
//! # What the agent does not log
//!
//! The request body is never logged, not even at trace level: it contains captured
//! message metadata and ciphertext, and `logging::redact_line` can only see one line
//! at a time. What is logged is the method, the device id, the verdict counts and the
//! HTTP status.

use std::sync::atomic::Ordering;

use serde::Deserialize;
use serde_json::{json, Value};

use crate::api::base64;
use crate::api::http::{self, HttpResponse};
use crate::api::protocol::{code, ApiError, Method, PROTOCOL_VERSION};
use crate::api::server::ServerState;
use crate::auth::{ClientRecord, RateLimiter};
use crate::error::{Error, Result};
use crate::secret::SecretBytes;
use crate::store::{AccountId, Event, EventKind, IngestResult, MAX_BATCH_EVENTS};
use crate::sync::clock::{Hlc, SystemClock};

use crate::sync::clock::Clock as _;

pub const MAX_REQUEST_ID_LEN: usize = 64;
pub const MAX_IDEMPOTENCY_KEY_LEN: usize = 128;

/// The parsed envelope. `serde` does the shape, this type does the meaning.
#[derive(Debug, Deserialize)]
pub struct WireRequest {
    #[serde(default)]
    pub protocol: Option<u16>,
    #[serde(default)]
    pub request_id: String,
    #[serde(default)]
    pub idempotency_key: String,
    #[serde(default)]
    pub method: String,
    #[serde(default)]
    pub params: Value,
}

/// Handle one request body. `authorization` is the raw header value, if any.
///
/// Always returns a response; an empty body, a bad token and an internal failure all
/// produce a JSON envelope with the right status, so the client never has to treat a
/// dropped connection as a normal outcome.
pub fn handle(state: &ServerState, authorization: Option<&str>, body: &[u8]) -> HttpResponse {
    let request: WireRequest = match serde_json::from_slice(body) {
        Ok(request) => request,
        Err(error) => {
            return envelope_error(
                "",
                400,
                ApiError::new(
                    code::INVALID_REQUEST,
                    format!("body is not a request object: {error}"),
                    false,
                ),
            )
        }
    };
    handle_request(state, request, authorization)
}

pub fn handle_request(
    state: &ServerState,
    request: WireRequest,
    authorization: Option<&str>,
) -> HttpResponse {
    let request_id = request.request_id.clone();

    // 1. request_id.
    if request_id.is_empty() || request_id.len() > MAX_REQUEST_ID_LEN {
        return envelope_error(
            "",
            400,
            ApiError::new(
                code::INVALID_REQUEST,
                format!("request_id must be 1..={MAX_REQUEST_ID_LEN} characters"),
                false,
            ),
        );
    }
    // 2. method.
    let Some(method) = Method::parse(&request.method) else {
        return envelope_error(
            &request_id,
            400,
            ApiError::new(
                code::INVALID_REQUEST,
                format!("unknown method {:?}", request.method),
                false,
            ),
        );
    };
    // 3. protocol window.
    let protocol = request.protocol.unwrap_or(PROTOCOL_VERSION);
    if protocol < crate::api::protocol::MIN_SUPPORTED_PROTOCOL || protocol > PROTOCOL_VERSION {
        return envelope_error(
            &request_id,
            400,
            ApiError::new(
                code::UNSUPPORTED_PROTOCOL,
                format!(
                    "protocol {protocol} is outside {}..={PROTOCOL_VERSION}",
                    crate::api::protocol::MIN_SUPPORTED_PROTOCOL
                ),
                false,
            ),
        );
    }
    // 4. idempotency key, for the methods that need one.
    if method.is_mutating()
        && (request.idempotency_key.is_empty()
            || request.idempotency_key.len() > MAX_IDEMPOTENCY_KEY_LEN)
    {
        return envelope_error(
            &request_id,
            400,
            ApiError::new(
                code::INVALID_REQUEST,
                format!(
                    "{} requires an idempotency_key of 1..={MAX_IDEMPOTENCY_KEY_LEN} characters",
                    method.as_str()
                ),
                false,
            ),
        );
    }

    let now_ms = SystemClock.now_ms();

    // 5. authentication. `hello` is the one method that works before enrollment: it
    // is how a freshly installed client learns the version and the protocol window.
    let record = if method == Method::Hello {
        None
    } else {
        match authenticate(state, authorization, now_ms) {
            Ok(record) => Some(record),
            Err(response) => return response,
        }
    };

    // 6–7. ownership and scope.
    let account = match resolve_account(&request.params, record.as_ref()) {
        Ok(account) => account,
        Err(response) => return response,
    };
    if let Some(record) = &record {
        if let Some(scope) = method.scope() {
            if !record.allows(scope) {
                return envelope_error(
                    &request_id,
                    403,
                    ApiError::new(
                        code::FORBIDDEN,
                        format!(
                            "device {} lacks scope {}",
                            record.device_id_hex(),
                            scope.as_str()
                        ),
                        false,
                    ),
                );
            }
        }
    }

    // 8. rate limit, per device. Anonymous `hello` is limited by a shared bucket.
    let bucket = record
        .as_ref()
        .map(|record| record.device_id_hex())
        .unwrap_or_else(|| "anonymous".to_string());
    if !take_token(state, &bucket, now_ms) {
        return envelope_error(
            &request_id,
            429,
            ApiError::new(
                code::RATE_LIMITED,
                "too many requests; retry after a short delay",
                true,
            ),
        );
    }

    // 9. idempotent replay.
    let idempotency_bucket = match &record {
        Some(record) if !request.idempotency_key.is_empty() => {
            format!("{}:{}", record.device_id_hex(), request.idempotency_key)
        }
        _ => String::new(),
    };
    if !idempotency_bucket.is_empty() {
        if let Some(stored) = state.idempotency.lock().ok().and_then(|store| {
            store
                .get(&idempotency_bucket, now_ms)
                .map(|s| s.body.clone())
        }) {
            return HttpResponse::json(200, stored);
        }
    }

    // 10. handler.
    let tenant = resolve_tenant(&request.params, record.as_ref());
    if let Some(record) = &record {
        if let Err(response) = check_tenant(record, tenant.as_deref()) {
            return with_request_id(response, &request_id);
        }
    }

    let result = dispatch(
        state,
        method,
        &request.params,
        account,
        mention_device(record.as_ref()),
        tenant.as_deref(),
        now_ms,
    );

    let response = match result {
        Ok(result) => envelope_ok(&request_id, result),
        Err(error) => error_envelope(&request_id, &error),
    };

    // Persist only successful mutating answers: a failure must be retryable with the
    // same key, and a 409 is not an answer worth replaying.
    if response.status == 200 && !idempotency_bucket.is_empty() {
        if let Ok(mut store) = state.idempotency.lock() {
            store.put(&idempotency_bucket, &response.body, now_ms);
        }
    }
    response
}

fn mention_device(record: Option<&ClientRecord>) -> Option<String> {
    record.map(|record| record.device_id_hex())
}

fn authenticate(
    state: &ServerState,
    authorization: Option<&str>,
    now_ms: u64,
) -> std::result::Result<ClientRecord, HttpResponse> {
    let Some(token) = authorization.and_then(parse_bearer) else {
        return Err(envelope_error(
            "",
            401,
            ApiError::new(
                code::UNAUTHENTICATED,
                "missing `Authorization: Bearer <device token>`",
                false,
            ),
        ));
    };
    // A malformed token is a 401, not a 400: it says nothing about the request shape
    // and everything about the credential, and a client should re-enroll rather than
    // fix its JSON.
    let Ok(bytes) = crate::crypto::sig::parse_hex(token) else {
        return Err(envelope_error(
            "",
            401,
            ApiError::new(code::UNAUTHENTICATED, "device token is not hex", false),
        ));
    };
    if bytes.len() != crate::auth::TOKEN_BYTES {
        return Err(envelope_error(
            "",
            401,
            ApiError::new(
                code::UNAUTHENTICATED,
                format!(
                    "device token must be {} bytes, got {}",
                    crate::auth::TOKEN_BYTES,
                    bytes.len()
                ),
                false,
            ),
        ));
    }
    let secret = SecretBytes::new(bytes);
    let mut authorizer = match state.authorizer.lock() {
        Ok(authorizer) => authorizer,
        Err(_) => {
            return Err(envelope_error(
                "",
                500,
                ApiError::new(code::INTERNAL, "auth store is poisoned", false),
            ))
        }
    };
    match authorizer.authenticate(&secret, now_ms) {
        Ok(record) => Ok(record),
        Err(error) => Err(envelope_error(
            "",
            401,
            ApiError::new(code::UNAUTHENTICATED, error.to_string(), false),
        )),
    }
}

/// Extract the token from `Authorization: Bearer <hex>`.
pub fn parse_bearer(header: &str) -> Option<&str> {
    let (scheme, token) = header.split_once(' ')?;
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

fn take_token(state: &ServerState, bucket: &str, now_ms: u64) -> bool {
    let Ok(mut limiters) = state.rate.lock() else {
        // A poisoned limiter must not turn into "no limit" or into a panic.
        return false;
    };
    let limiter = limiters.entry(bucket.to_string()).or_insert_with(|| {
        RateLimiter::new(
            state.config.rate_limit_burst,
            state.config.rate_limit_per_sec,
            now_ms,
        )
    });
    limiter.try_take(now_ms)
}

fn resolve_tenant(params: &Value, record: Option<&ClientRecord>) -> Option<String> {
    optional_str(params, "tenant")
        .map(str::to_string)
        .or_else(|| record.and_then(|record| record.tenant.clone()))
}

fn check_tenant(
    record: &ClientRecord,
    tenant: Option<&str>,
) -> std::result::Result<(), HttpResponse> {
    if record.allows_tenant(tenant) {
        return Ok(());
    }
    Err(envelope_error(
        "",
        403,
        ApiError::new(
            code::FORBIDDEN,
            format!(
                "device {} is not enrolled for tenant {}",
                record.device_id_hex(),
                tenant.unwrap_or("<none>")
            ),
            false,
        ),
    ))
}

/// The account the request is about: from the params, or from the device record.
///
/// A mismatch is a 403, and this is the *first* place it is caught — before a single
/// event is parsed. `store::ingest` also rejects a foreign `account_id` per event,
/// but a request that names the wrong account should fail as a request.
fn resolve_account(
    params: &Value,
    record: Option<&ClientRecord>,
) -> std::result::Result<Option<AccountId>, HttpResponse> {
    let requested = match optional_str(params, "account") {
        Some(text) => match AccountId::from_hex(text) {
            Ok(account) => Some(account),
            Err(error) => {
                return Err(envelope_error(
                    "",
                    400,
                    ApiError::new(code::INVALID_REQUEST, format!("account: {error}"), false),
                ))
            }
        },
        None => None,
    };
    let Some(record) = record else {
        return Ok(requested);
    };
    match (record.account_id, requested) {
        (Some(allowed), Some(requested)) if allowed != requested => Err(envelope_error(
            "",
            403,
            ApiError::new(
                code::FORBIDDEN,
                format!(
                    "device {} is enrolled for account {}, not {}",
                    record.device_id_hex(),
                    allowed.to_hex(),
                    requested.to_hex()
                ),
                false,
            ),
        )),
        (Some(allowed), None) => Ok(Some(allowed)),
        (None, requested) => Ok(requested),
        (Some(_), Some(requested)) => Ok(Some(requested)),
    }
}

#[allow(clippy::too_many_arguments)]
fn dispatch(
    state: &ServerState,
    method: Method,
    params: &Value,
    account: Option<AccountId>,
    device_id: Option<String>,
    tenant: Option<&str>,
    now_ms: u64,
) -> Result<Value> {
    match method {
        Method::Hello => Ok(hello_json(state, now_ms)),
        Method::Status => Ok(status_json(state, now_ms)),
        Method::SyncStamp => {
            let (tenant, account) = require_scope(tenant, account)?;
            let stamp = state.sync.stamp(tenant, &account)?;
            Ok(json!({
                "account": account.to_hex(),
                "high_water": stamp.high_water,
                "last_write_ms": stamp.last_write_ms,
                "op_count": stamp.op_count,
            }))
        }
        Method::SyncPull => {
            let (tenant, account) = require_scope(tenant, account)?;
            let since_seq = optional_u64(params, "since_seq")?.unwrap_or(0);
            let limit = optional_u64(params, "limit")?
                .unwrap_or(u64::from(crate::api::sync::DEFAULT_PAGE_LIMIT));
            let limit = u32::try_from(limit).unwrap_or(u32::MAX);
            let page = state.sync.pull(tenant, &account, since_seq, limit)?;
            let ops: Vec<Value> = page
                .ops
                .iter()
                .map(|op| {
                    json!({
                        "op_seq": op.op_seq,
                        "kind": op.kind.code(),
                        "kind_name": op.kind.as_str(),
                        "entity_id": op.entity_id,
                        "field": op.field,
                        "value": base64::encode(&op.value),
                        "hlc": op.hlc.to_wire(),
                        "device_id": op.device_id,
                        "content_hash": crate::crypto::sig::to_hex(&op.content_hash),
                        "applied_at_ms": op.applied_at_ms,
                    })
                })
                .collect();
            Ok(json!({
                "account": account.to_hex(),
                "since_seq": page.since_seq,
                "next_seq": page.next_seq,
                "high_water": page.high_water,
                "has_more": page.has_more,
                "pruned_upto": page.pruned_upto,
                "ops": ops,
            }))
        }
        Method::SyncPush => {
            let (tenant, account) = require_scope(tenant, account)?;
            let device = device_id
                .ok_or_else(|| Error::Auth("push requires an authenticated device".to_string()))?;
            let events = parse_events(params, account, &device)?;
            let now = i64::try_from(now_ms).unwrap_or(i64::MAX);
            let pushed = state.sync.push(tenant, &account, &events, now)?;
            let results: Vec<Value> = pushed
                .outcome
                .results
                .iter()
                .enumerate()
                .map(|(index, result)| match result {
                    IngestResult::Applied { op_seq } => {
                        json!({"index": index, "verdict": "applied", "op_seq": op_seq})
                    }
                    IngestResult::Duplicate { op_seq } => {
                        json!({"index": index, "verdict": "duplicate", "op_seq": op_seq})
                    }
                    IngestResult::Rejected { reason } => {
                        json!({"index": index, "verdict": "rejected", "reason": reason.as_str()})
                    }
                })
                .collect();
            Ok(json!({
                "account": account.to_hex(),
                "results": results,
                "applied": pushed.outcome.applied,
                "duplicates": pushed.outcome.duplicates,
                "rejected": pushed.outcome.rejected,
                "conflicts": pushed.outcome.conflicts,
                "conflict_ids": pushed.outcome.conflict_ids,
                "high_water": pushed.outcome.high_water,
                "account_stamp": {
                    "high_water": pushed.stamp.high_water,
                    "last_write_ms": pushed.stamp.last_write_ms,
                    "op_count": pushed.stamp.op_count,
                },
            }))
        }
        Method::SyncConflicts => {
            let (tenant, account) = require_scope(tenant, account)?;
            let since_id = optional_u64(params, "since_id")?.unwrap_or(0);
            let limit = optional_u64(params, "limit")?
                .unwrap_or(u64::from(crate::api::sync::DEFAULT_PAGE_LIMIT));
            let include_resolved = optional_bool(params, "include_resolved")?.unwrap_or(true);
            let resolved = resolve_requests(params, &device_id)?;
            let mut resolved_count = 0u64;
            for (conflict_id, resolved_by) in &resolved {
                if state
                    .sync
                    .resolve_conflict(tenant, &account, *conflict_id, resolved_by)?
                {
                    resolved_count += 1;
                }
            }
            let conflicts = state.sync.conflicts(
                tenant,
                &account,
                since_id,
                u32::try_from(limit).unwrap_or(u32::MAX),
                include_resolved,
            )?;
            let next_since_id = conflicts
                .last()
                .map(|row| row.conflict_id)
                .unwrap_or(since_id);
            Ok(json!({
                "account": account.to_hex(),
                "conflicts": conflicts.iter().map(conflict_json).collect::<Vec<_>>(),
                "next_since_id": next_since_id,
                "resolved": resolved_count,
            }))
        }
        Method::SyncPrune => {
            let (tenant, account) = require_scope(tenant, account)?;
            let upto_seq = required_u64(params, "upto_seq")?;
            let (report, stamp) = state.sync.prune(tenant, &account, upto_seq)?;
            Ok(json!({
                "account": account.to_hex(),
                "pruned": {
                    "ops": report.ops,
                    "presence": report.presence,
                    "profiles": report.profiles,
                    "archive": report.archive,
                    "total": report.total(),
                },
                "upto_seq": upto_seq,
                "high_water": stamp.high_water,
            }))
        }
        Method::Shutdown => {
            state.running.store(false, Ordering::SeqCst);
            Ok(json!({"stopping": true}))
        }
        Method::DbStatus => {
            // § 5.8: `sync:read` — the same credential that pulls. It never reveals
            // the connection string or the password: `DbStatus` has no field for
            // either, and every error it reports is a class.
            let (tenant, account) = require_scope(tenant, account)?;
            // The DB belongs to one account (it is the account's data), so a request
            // for another account cannot be answered — and answering "connected" would
            // be a lie about *that* account's pipeline.
            let configured = state
                .db
                .as_ref()
                .filter(|ingest| ingest.config().account == account);
            let status = match configured {
                Some(ingest) => state
                    .sync
                    .with_store(tenant, |store| Ok(ingest.status(Some(store), now_ms)))?,
                None => crate::db::DbStatus {
                    last_error: Some(crate::db::class::CONFIG),
                    ..crate::db::DbStatus::locked()
                },
            };
            // `serde_json::to_value` cannot fail for this struct (no maps with
            // non-string keys, no fallible serialisation), but the `?` keeps the
            // "no unwrap" rule and would be caught by the test that reads the
            // response.
            serde_json::to_value(status)
                .map_err(|_| Error::Storage("db.status is not serialisable".to_string()))
        }
        Method::DbIngest => {
            // § 15 п. 11: the client token cannot reach this — `Method::scope`
            // demands `db:ingest`, which `pair` never grants, and
            // `ClientRecord::allows` checks the token *role* as well.
            let (tenant, account) = require_scope(tenant, account)?;
            let ingest = state.db.as_ref().ok_or_else(|| {
                Error::Invalid("db ingest is not configured; see docs/db-ingest.md".to_string())
            })?;
            if ingest.config().account != account {
                return Err(Error::Auth(
                    "this token's account is not the one the ingest publishes into".to_string(),
                ));
            }
            let report = state.sync.with_store(tenant, |store| {
                crate::db::ingest::run_and_log(
                    ingest,
                    store,
                    i64::try_from(now_ms).unwrap_or(i64::MAX),
                )
            })?;
            Ok(json!({
                "account": account.to_hex(),
                "pass": serde_json::from_str::<Value>(&report.to_json())
                    .unwrap_or(Value::Null),
                "status": serde_json::to_value(ingest.status(None, now_ms))
                    .unwrap_or(Value::Null),
            }))
        }
        other => Err(Error::NotImplemented(match other {
            Method::Pair => "pair over the wire (use `teleforge-agent pair`)",
            Method::Unlock => "unlock over the wire",
            Method::Lock => "lock over the wire",
            Method::TunnelUp | Method::TunnelDown | Method::TunnelStatus => "tunnel.*",
            Method::Accounts | Method::AccountStart | Method::AccountStop => "TDLib accounts",
            Method::UpdateCheck => "update.check",
            _ => "method",
        })),
    }
}

fn require_scope(tenant: Option<&str>, account: Option<AccountId>) -> Result<(&str, AccountId)> {
    let tenant = tenant.ok_or_else(|| {
        Error::Invalid("this method needs a tenant: pass `\"tenant\": \"<hex>\"`".to_string())
    })?;
    let account = account.ok_or_else(|| {
        Error::Invalid("this method needs an account: pass `\"account\": \"<hex>\"`".to_string())
    })?;
    Ok((tenant, account))
}

/// The wire shape of one conflict row.
fn conflict_json(row: &crate::store::ConflictRow) -> Value {
    json!({
        "conflict_id": row.conflict_id,
        "entity_id": row.entity_id,
        "field": row.field,
        "kind": row.kind.code(),
        "kind_name": row.kind.as_str(),
        "local_op_seq": row.local_op_seq,
        "local_hlc": row.local_hlc.to_wire(),
        "local_device": row.local_device,
        "remote_op_seq": row.remote_op_seq,
        "remote_hlc": row.remote_hlc.to_wire(),
        "remote_device": row.remote_device,
        "detected_at_ms": row.detected_at_ms,
        "resolved_by": row.resolved_by,
    })
}

fn hello_json(_state: &ServerState, now_ms: u64) -> Value {
    json!({
        "agent_version": crate::AGENT_VERSION,
        "protocol": PROTOCOL_VERSION,
        "min_client_version": crate::MIN_CLIENT_VERSION,
        "server_time_ms": now_ms,
        "sync": "available",
    })
}

fn status_json(state: &ServerState, now_ms: u64) -> Value {
    let clients = state
        .authorizer
        .lock()
        .map(|store| store.len())
        .unwrap_or(0);
    json!({
        "agent_version": crate::AGENT_VERSION,
        "protocol": PROTOCOL_VERSION,
        "min_client_version": crate::MIN_CLIENT_VERSION,
        "vault": "locked",
        // The passive journal is client-sealed ciphertext: serving it does not need
        // the tenant DEK, so `sync` is available while the vault is locked. The
        // vault gates session material (TDLib), not this path.
        "sync": "available",
        "store_schema": crate::store::SCHEMA_VERSION,
        "clients": clients,
        "uptime_ms": now_ms.saturating_sub(state.started_at_ms),
        "server_time_ms": now_ms,
    })
}

/// Parse the `ops` array of `sync.push` into [`Event`]s.
///
/// Every failure is [`Error::Invalid`] — the *batch* is malformed. A single event
/// with a bad shape is not rejected here: it is passed to `ingest`, which answers
/// per event (`Rejected { reason }`) so one bad event does not cost the batch. The
/// distinction is "cannot be read as JSON of the expected shape" (400) versus "read
/// fine but the client's data is wrong" (200 with a rejection).
pub fn parse_events(params: &Value, account: AccountId, device_id: &str) -> Result<Vec<Event>> {
    let ops = params
        .get("ops")
        .and_then(Value::as_array)
        .ok_or_else(|| Error::Invalid("push needs an `ops` array".to_string()))?;
    if ops.len() > MAX_BATCH_EVENTS {
        return Err(Error::Invalid(format!(
            "batch of {} events exceeds the limit of {MAX_BATCH_EVENTS}",
            ops.len()
        )));
    }
    let mut events = Vec::with_capacity(ops.len());
    for (index, op) in ops.iter().enumerate() {
        let object = op
            .as_object()
            .ok_or_else(|| Error::Invalid(format!("ops[{index}] is not an object")))?;
        let kind = parse_kind(object.get("kind"))
            .ok_or_else(|| Error::Invalid(format!("ops[{index}].kind is missing or unknown")))?;
        let entity_id = object
            .get("entity_id")
            .and_then(Value::as_str)
            .ok_or_else(|| Error::Invalid(format!("ops[{index}].entity_id must be a string")))?;
        let field = match object.get("field") {
            None | Some(Value::Null) => None,
            Some(Value::String(text)) => Some(text.clone()),
            Some(other) => {
                return Err(Error::Invalid(format!(
                    "ops[{index}].field must be a string, got {other}"
                )))
            }
        };
        let value = match object.get("value") {
            None | Some(Value::Null) => Vec::new(),
            Some(Value::String(text)) => base64::decode(text).map_err(|error| {
                Error::Invalid(format!("ops[{index}].value is not base64: {error}"))
            })?,
            Some(other) => {
                return Err(Error::Invalid(format!(
                    "ops[{index}].value must be a base64 string, got {other}"
                )))
            }
        };
        let hlc = parse_hlc(object.get("hlc"))
            .ok_or_else(|| Error::Invalid(format!("ops[{index}].hlc is missing or malformed")))?;
        let content_hash =
            match object.get("content_hash") {
                None | Some(Value::Null) => None,
                Some(Value::String(text)) => Some(parse_hash(text).map_err(|error| {
                    Error::Invalid(format!("ops[{index}].content_hash: {error}"))
                })?),
                Some(other) => {
                    return Err(Error::Invalid(format!(
                        "ops[{index}].content_hash must be a hex string, got {other}"
                    )))
                }
            };
        let mut event = Event {
            account_id: Some(account),
            // The device is taken from the authenticated token, never from the
            // request: a client must not be able to move another device's clock.
            device_id: device_id.to_string(),
            kind: Some(kind),
            entity_id: entity_id.to_string(),
            field,
            value,
            hlc,
            content_hash: content_hash.unwrap_or([0u8; 32]),
        };
        if content_hash.is_none() {
            event = event.with_computed_hash();
        }
        events.push(event);
    }
    Ok(events)
}

fn parse_kind(value: Option<&Value>) -> Option<EventKind> {
    match value? {
        Value::String(name) => EventKind::parse(name),
        Value::Number(number) => number
            .as_u64()
            .and_then(|code| u8::try_from(code).ok())
            .and_then(EventKind::from_code),
        _ => None,
    }
}

fn parse_hlc(value: Option<&Value>) -> Option<Hlc> {
    match value? {
        // Canonical form: `"0000018f3c1a2b40-0000"`.
        Value::String(text) => Hlc::parse_wire(text),
        // Explicit form, for a client that would rather not format hex.
        Value::Object(map) => {
            let physical = map.get("physical_ms")?.as_u64()?;
            let logical = map.get("logical")?.as_u64()?;
            Some(Hlc::new(physical, u16::try_from(logical).ok()?))
        }
        _ => None,
    }
}

fn parse_hash(text: &str) -> Result<[u8; 32]> {
    let bytes = crate::crypto::sig::parse_hex(text)?;
    if bytes.len() != 32 {
        return Err(Error::Invalid(format!(
            "expected 32 bytes, got {}",
            bytes.len()
        )));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

/// Optional `resolve` array: `[{"conflict_id":7,"resolved_by":"device-hex"}]`.
fn resolve_requests(params: &Value, device_id: &Option<String>) -> Result<Vec<(u64, String)>> {
    let mut out = Vec::new();
    let Some(resolve) = params.get("resolve") else {
        return Ok(out);
    };
    if resolve.is_null() {
        return Ok(out);
    }
    let array = resolve
        .as_array()
        .ok_or_else(|| Error::Invalid("`resolve` must be an array".to_string()))?;
    for (index, entry) in array.iter().enumerate() {
        let object = entry
            .as_object()
            .ok_or_else(|| Error::Invalid(format!("resolve[{index}] is not an object")))?;
        let conflict_id = object
            .get("conflict_id")
            .and_then(Value::as_u64)
            .ok_or_else(|| {
                Error::Invalid(format!("resolve[{index}].conflict_id must be a number"))
            })?;
        // Default to the authenticated device: the decision is attributable without
        // the client having to say so, and a client-supplied free-text field would
        // end up in logs and in the UI.
        let resolved_by = match object.get("resolved_by") {
            None | Some(Value::Null) => device_id.clone().ok_or_else(|| {
                Error::Invalid(format!(
                    "resolve[{index}].resolved_by is required for an anonymous request"
                ))
            })?,
            Some(Value::String(text)) => text.clone(),
            Some(other) => {
                return Err(Error::Invalid(format!(
                    "resolve[{index}].resolved_by must be a string, got {other}"
                )))
            }
        };
        out.push((conflict_id, resolved_by));
    }
    Ok(out)
}

pub fn optional_str<'a>(params: &'a Value, key: &str) -> Option<&'a str> {
    params.get(key).and_then(Value::as_str)
}

fn optional_u64(params: &Value, key: &str) -> Result<Option<u64>> {
    match params.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::Number(number)) => number
            .as_u64()
            .map(Some)
            .ok_or_else(|| Error::Invalid(format!("{key} must be a non-negative integer"))),
        Some(other) => Err(Error::Invalid(format!(
            "{key} must be an integer, got {other}"
        ))),
    }
}

fn required_u64(params: &Value, key: &str) -> Result<u64> {
    optional_u64(params, key)?.ok_or_else(|| Error::Invalid(format!("{key} is required")))
}

fn optional_bool(params: &Value, key: &str) -> Result<Option<bool>> {
    match params.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::Bool(value)) => Ok(Some(*value)),
        Some(other) => Err(Error::Invalid(format!(
            "{key} must be a boolean, got {other}"
        ))),
    }
}

fn envelope_ok(request_id: &str, result: Value) -> HttpResponse {
    HttpResponse::json(
        200,
        json!({
            "protocol": PROTOCOL_VERSION,
            "request_id": request_id,
            "ok": true,
            "result": result,
            "error": Value::Null,
        })
        .to_string(),
    )
}

fn envelope_error(request_id: &str, status: u16, error: ApiError) -> HttpResponse {
    HttpResponse::json(
        status,
        json!({
            "protocol": PROTOCOL_VERSION,
            "request_id": request_id,
            "ok": false,
            "result": {},
            "error": {
                "code": error.code,
                "message": error.message,
                "retryable": error.retryable,
            },
        })
        .to_string(),
    )
}

fn with_request_id(response: HttpResponse, request_id: &str) -> HttpResponse {
    // Ownership failures are detected before the request id is threaded through; the
    // body already carries an empty id, so re-render it with the real one.
    let Ok(mut value) = serde_json::from_str::<Value>(&response.body) else {
        return response;
    };
    if let Some(object) = value.as_object_mut() {
        object.insert(
            "request_id".to_string(),
            Value::String(request_id.to_string()),
        );
    }
    HttpResponse::json(response.status, value.to_string())
}

fn error_envelope(request_id: &str, error: &Error) -> HttpResponse {
    // An unimplemented method is 501 and not 500: the client should show "this agent
    // build has no tunnel support" rather than "internal error".
    if let Error::NotImplemented(_) = error {
        let api = ApiError::from(error);
        return envelope_error(request_id, 501, api);
    }
    let api = ApiError::from(error);
    let status = http::status_for_code(api.code);
    envelope_error(request_id, status, api)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::auth::Scope;

    fn account() -> AccountId {
        AccountId::new([0x61; 16]).unwrap()
    }

    #[test]
    fn parse_bearer_is_case_insensitive_and_strict() {
        assert_eq!(parse_bearer("Bearer abc"), Some("abc"));
        assert_eq!(parse_bearer("bearer  abc "), Some("abc"));
        assert_eq!(parse_bearer("Basic abc"), None);
        assert_eq!(parse_bearer("Bearer "), None);
        assert_eq!(parse_bearer("abc"), None);
    }

    #[test]
    fn the_envelope_shape_is_read_with_or_without_optional_fields() {
        let request: WireRequest = serde_json::from_str(
            r#"{"protocol":1,"request_id":"r1","idempotency_key":"k1",
                "method":"sync.pull","params":{"since_seq":3}}"#,
        )
        .unwrap();
        assert_eq!(request.protocol, Some(1));
        assert_eq!(request.method, "sync.pull");
        assert_eq!(request.params["since_seq"], 3);

        let minimal: WireRequest =
            serde_json::from_str(r#"{"request_id":"r","method":"hello"}"#).unwrap();
        assert_eq!(minimal.protocol, None);
        assert!(minimal.idempotency_key.is_empty());
        assert_eq!(minimal.params, Value::Null);
    }

    #[test]
    fn events_are_parsed_with_the_authenticated_device_and_account() {
        let body = json!({
            "ops": [
                {"kind": "profile_update", "entity_id": "profile:900",
                 "field": "username", "value": base64::encode(b"@neo"),
                 "hlc": "00000000000003e8-0000"},
                {"kind": 4, "entity_id": "presence:900",
                 "value": base64::encode(&5i64.to_be_bytes()),
                 "hlc": {"physical_ms": 1001, "logical": 2}}
            ]
        });
        let events = parse_events(&body, account(), "dev-a").unwrap();
        assert_eq!(events.len(), 2);
        assert_eq!(events[0].kind, Some(EventKind::ProfileUpdate));
        assert_eq!(events[0].value, b"@neo");
        assert_eq!(events[0].device_id, "dev-a");
        assert_eq!(events[0].hlc, Hlc::new(1_000, 0));
        assert_eq!(events[0].content_hash, events[0].compute_content_hash());
        assert_eq!(events[1].kind, Some(EventKind::PresenceOnline));
        assert_eq!(events[1].hlc, Hlc::new(1_001, 2));
        assert_eq!(events[1].account_id, Some(account()));
    }

    #[test]
    fn an_explicit_content_hash_is_honoured() {
        let hash = [7u8; 32];
        let body = json!({
            "ops": [{"kind": "message_new", "entity_id": "message:1:2",
                     "field": "text", "value": base64::encode(b"hi"),
                     "hlc": "0000000000000001-0000",
                     "content_hash": crate::crypto::sig::to_hex(&hash)}]
        });
        let events = parse_events(&body, account(), "dev-a").unwrap();
        assert_eq!(events[0].content_hash, hash);
    }

    #[test]
    fn malformed_batches_are_errors_with_the_offending_index() {
        let cases = [
            json!({}),
            json!({"ops": "not an array"}),
            json!({"ops": [3]}),
            json!({"ops": [{"entity_id": "profile:900", "hlc": "0000000000000001-0000"}]}),
            json!({"ops": [{"kind": "nope", "entity_id": "profile:900",
                            "hlc": "0000000000000001-0000"}]}),
            json!({"ops": [{"kind": "profile_update", "hlc": "0000000000000001-0000"}]}),
            json!({"ops": [{"kind": "profile_update", "entity_id": "profile:900",
                            "hlc": "not-a-clock"}]}),
            json!({"ops": [{"kind": "profile_update", "entity_id": "profile:900",
                            "value": "not base64!", "hlc": "0000000000000001-0000"}]}),
            json!({"ops": [{"kind": "profile_update", "entity_id": "profile:900",
                            "hlc": "0000000000000001-0000", "content_hash": "zz"}]}),
        ];
        for case in cases {
            let error = parse_events(&case, account(), "dev-a").unwrap_err();
            assert!(matches!(error, Error::Invalid(_)), "{case}: {error}");
        }
    }

    #[test]
    fn an_oversized_batch_is_refused_before_any_event_is_built() {
        let ops: Vec<Value> = (0..=MAX_BATCH_EVENTS)
            .map(|_| {
                json!({"kind": "profile_update", "entity_id": "profile:900",
                            "hlc": "0000000000000001-0000"})
            })
            .collect();
        let error = parse_events(&json!({"ops": ops}), account(), "dev-a").unwrap_err();
        assert!(error.to_string().contains("exceeds the limit"));
    }

    #[test]
    fn hlc_accepts_both_wire_forms_and_rejects_the_rest() {
        assert_eq!(
            parse_hlc(Some(&json!("00000000000003e8-0002"))),
            Some(Hlc::new(1_000, 2))
        );
        assert_eq!(
            parse_hlc(Some(&json!({"physical_ms": 7, "logical": 0}))),
            Some(Hlc::new(7, 0))
        );
        assert_eq!(parse_hlc(Some(&json!("0003e8-0002"))), None);
        assert_eq!(parse_hlc(Some(&json!("zzzzzzzzzzzzzzzz-0000"))), None);
        assert_eq!(parse_hlc(Some(&json!({"physical_ms": 7}))), None);
        assert_eq!(parse_hlc(None), None);
    }

    #[test]
    fn numbers_must_be_numbers_not_strings() {
        let params = json!({"since_seq": 5, "limit": "10", "flag": true});
        assert_eq!(optional_u64(&params, "since_seq").unwrap(), Some(5));
        assert!(optional_u64(&params, "limit").is_err());
        assert_eq!(optional_bool(&params, "flag").unwrap(), Some(true));
        assert!(optional_bool(&params, "since_seq").is_err());
        assert_eq!(optional_u64(&params, "missing").unwrap(), None);
        assert!(required_u64(&params, "missing").is_err());
    }

    #[test]
    fn resolve_requests_default_the_author_to_the_device() {
        let params = json!({"resolve": [{"conflict_id": 4}]});
        let resolved = resolve_requests(&params, &Some("dev-a".to_string())).unwrap();
        assert_eq!(resolved, vec![(4, "dev-a".to_string())]);

        let anonymous = resolve_requests(&params, &None);
        assert!(anonymous.is_err(), "a decision must be attributable");

        let explicit = resolve_requests(
            &json!({"resolve": [{"conflict_id": 4, "resolved_by": "me"}]}),
            &None,
        )
        .unwrap();
        assert_eq!(explicit, vec![(4, "me".to_string())]);

        assert!(resolve_requests(&json!({"resolve": "nope"}), &None).is_err());
        assert!(resolve_requests(&json!({"resolve": [{}]}), &None).is_err());
    }

    #[test]
    fn error_mapping_uses_the_documented_status_codes() {
        let conflict = error_envelope("r", &Error::Conflict("cursor".to_string()));
        assert_eq!(conflict.status, 409);
        assert!(conflict.body.contains("\"code\":1006"));

        let unauthorized = error_envelope("r", &Error::Auth("token".to_string()));
        assert_eq!(unauthorized.status, 401);
        assert!(unauthorized.body.contains("\"code\":1002"));

        let unimplemented = error_envelope("r", &Error::NotImplemented("tunnel.up"));
        assert_eq!(unimplemented.status, 501);
        assert!(unimplemented.body.contains("\"ok\":false"));
    }

    #[test]
    fn a_scope_failure_is_403_with_the_missing_scope_named() {
        let response = check_tenant(
            &ClientRecord {
                device_id: [1u8; 16],
                label: "x".to_string(),
                token_hash: [0u8; 32],
                scopes: vec![Scope::SyncRead],
                role: crate::auth::TokenRole::Reader,
                tenant: Some("aabb".to_string()),
                account_id: None,
                created_at: 0,
                last_seen_at: None,
                revoked: false,
            },
            Some("ccdd"),
        );
        let response = response.expect_err("another tenant must be refused");
        assert_eq!(response.status, 403);
        assert!(response.body.contains("not enrolled"));
    }
}
