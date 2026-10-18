//! MySQL / MariaDB implementation of [`DbSource`].
//!
//! Same contract as the PostgreSQL driver: one statement built by
//! [`TableSpec::select_sql`] from the allowlist, `?` placeholders for the cursor and the
//! limit, TLS through rustls, and a [`class`] instead of a driver message in every
//! error.
//!
//! ```sql
//! SELECT `id`, `kind`, `entity_id`, `field`, `observed_ms`, `value`
//!   FROM `sync_outbox` WHERE `id` > ? ORDER BY `id` ASC LIMIT ?
//! ```
//!
//! # Dialect details that matter
//!
//! * **Backticks, not double quotes.** `"id"` in MySQL is a *string literal*, so a
//!   PostgreSQL-style statement would compare the cursor to the text `id` instead of
//!   reading a column. [`Engine`] decides the quoting, and
//!   `allowlist::tests::the_select_statement_names_only_allowlisted_columns` pins
//!   both forms.
//! * **No `?` reuse.** Actually MySQL *does* allow reusing a `?`, but the statement is
//!   still sent with two parameters, in order, like PostgreSQL's `$1`/`$2`.
//! * **`ssl_opts` is mandatory here.** `mysql` only negotiates TLS when `SslOpts` is
//!   set, so the connection is built with `SslOpts` even for `verify-full` — an
//!   absence of it would be `sslmode=disable` by accident, which `conninfo` refuses
//!   for PostgreSQL precisely so it cannot happen by accident here.

use std::time::{Duration, Instant};

use mysql::prelude::Queryable;
use mysql::{Conn, Opts, OptsBuilder, Row, SslOpts};

use crate::db::allowlist::TableSpec;
use crate::db::conninfo::ConnFacts;
use crate::db::ingest::{DEFAULT_CONNECT_TIMEOUT_SECS, MAX_ROW_VALUE_BYTES};
use crate::db::{class, DbRow, DbRowBody, DbSource, DbStatus, Engine, VaultState, MAX_BATCH_LIMIT};
use crate::error::{Error, Result};
use crate::secret::SecretBytes;
use crate::store::event::EventKind;

/// A connected MySQL reader.
pub struct MySqlSource {
    conn: Conn,
    spec: &'static TableSpec,
    facts: ConnFacts,
    latency_ms: u64,
    tables_ok: bool,
    last_error: Option<&'static str>,
    /// `mysql::Conn` has no `is_closed`, so the state is tracked here: it flips to
    /// `false` on the first failed statement, which is what makes the next pass
    /// reconnect instead of reusing a dead socket.
    connected: bool,
}

impl MySqlSource {
    /// Connect, `SELECT 1`, and probe the grant of the configured table.
    pub fn connect(
        facts: &ConnFacts,
        connection_string: &SecretBytes,
        spec: &'static TableSpec,
    ) -> Result<Self> {
        let text = std::str::from_utf8(connection_string.expose())
            .map_err(|_| Error::Database(class::CONFIG))?;
        let base = crate::db::conninfo::base_url(text);
        let opts: Opts = Opts::from_url(base).map_err(|_| Error::Database(class::CONFIG))?;
        let timeout = Duration::from_secs(
            facts
                .connect_timeout_secs
                .unwrap_or(DEFAULT_CONNECT_TIMEOUT_SECS),
        );
        let ssl = match &facts.root_cert {
            Some(path) => {
                SslOpts::default().with_root_cert_path(Some(std::path::PathBuf::from(path)))
            }
            // No CA named: rustls/mysql falls back to its bundled root store
            // (`webpki-roots`), which covers a publicly signed DB certificate.
            None => SslOpts::default(),
        };
        let builder = OptsBuilder::from_opts(opts)
            // Always `Some`: a `None` here would silently mean "no TLS".
            .ssl_opts(Some(ssl))
            // A unix socket would bypass the host/port in `db.status` and the TLS
            // policy with it, so TCP is the only accepted transport.
            .prefer_socket(false)
            .tcp_connect_timeout(Some(timeout));
        let started = Instant::now();
        let mut conn = Conn::new(builder).map_err(|error| Error::Database(classify(&error)))?;
        let latency_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
        conn.query_drop("SELECT 1")
            .map_err(|error| Error::Database(classify(&error)))?;
        let tables_ok = conn
            .query_drop(spec.probe_sql(Engine::MySql).as_str())
            .is_ok();
        Ok(Self {
            conn,
            spec,
            facts: facts.clone(),
            latency_ms,
            tables_ok,
            last_error: None,
            connected: true,
        })
    }
}

impl DbSource for MySqlSource {
    fn status(&self) -> DbStatus {
        DbStatus {
            connected: self.connected,
            latency_ms: self.latency_ms,
            engine: Some(Engine::MySql.as_str()),
            database: Some(self.facts.database.clone()),
            reader_user: Some(self.facts.user.clone()),
            tls: Some(self.facts.tls_label()),
            cursor: None,
            lag_ms: None,
            last_read_ms: None,
            tables_ok: self.tables_ok,
            vault: VaultState::Unlocked.as_str(),
            last_error: self.last_error,
            last_ok_at: None,
            fails: 0,
            batches: 0,
            applied: 0,
            duplicates: 0,
            skipped: 0,
        }
    }

    fn next_batch(&mut self, cursor: i64, limit: usize) -> Result<Vec<DbRow>> {
        if limit == 0 || limit > MAX_BATCH_LIMIT {
            return Err(Error::Database(class::TOO_MANY_EVENTS));
        }
        let started = Instant::now();
        let sql = self.spec.select_sql(Engine::MySql);
        let rows: Vec<Row> = match self.conn.exec(
            sql.as_str(),
            (cursor, u64::try_from(limit).unwrap_or(u64::MAX)),
        ) {
            Ok(rows) => rows,
            Err(error) => {
                let class = classify(&error);
                self.connected = false;
                self.last_error = Some(class);
                return Err(Error::Database(class));
            }
        };
        self.latency_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
        let mut out = Vec::with_capacity(rows.len());
        for row in &rows {
            out.push(decode_row(row, self.spec)?);
        }
        Ok(out)
    }
}

/// Turn one MySQL row into a [`DbRow`], by column *name* so the statement order cannot
/// silently shift what a field means.
fn decode_row(row: &Row, spec: &'static TableSpec) -> Result<DbRow> {
    let key: i64 = row
        .get(spec.key_column)
        .ok_or(Error::Database(class::DECODE))?;
    let observed_ms: i64 = row.get("observed_ms").unwrap_or(0);
    let malformed = DbRow {
        key,
        observed_ms,
        body: DbRowBody::Malformed,
    };
    let Some(kind_text) = row.get::<String, _>("kind") else {
        return Ok(malformed);
    };
    let Some(kind) = EventKind::parse(&kind_text) else {
        return Ok(malformed);
    };
    let Some(entity_id) = row.get::<String, _>("entity_id") else {
        return Ok(malformed);
    };
    let field: Option<String> = row.get("field");
    // `BLOB` or `TEXT`: the outbox may store either, and both are just bytes here.
    let value: Vec<u8> = match row.get::<Vec<u8>, _>("value") {
        Some(bytes) => bytes,
        None => match row.get::<String, _>("value") {
            Some(text) => text.into_bytes(),
            None => return Ok(malformed),
        },
    };
    if value.len() > MAX_ROW_VALUE_BYTES {
        return Ok(malformed);
    }
    Ok(DbRow {
        key,
        observed_ms,
        body: DbRowBody::Event {
            kind,
            entity_id,
            field,
            value,
        },
    })
}

/// Map a driver error to a [`class`].
///
/// `mysql::Error` carries the server's error number and a message that can contain the
/// statement — which contains no user data (only allowlisted column names and bound
/// parameters) but can contain a host name. Neither is forwarded.
fn classify(error: &mysql::Error) -> &'static str {
    match error {
        // The server answered and refused the login.
        mysql::Error::MySqlError(server) => match server.code {
            // 1045 access denied, 1044 access denied for the database, 1698 access
            // denied for the socket user.
            1045 | 1044 | 1698 => class::AUTH,
            // 1049 unknown database, 1142/1143/1146 no such table / no privilege:
            // the grant or the schema is wrong.
            1049 | 1142 | 1143 | 1146 => class::QUERY,
            _ => class::QUERY,
        },
        // Before the server said anything: socket, TLS, timeout, DNS.
        _ => class::CONNECT,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::allowlist;

    #[test]
    fn the_statement_uses_mysql_quoting_and_placeholders() {
        let spec = allowlist::ensure_for_ingest("sync_outbox").unwrap();
        let sql = spec.select_sql(Engine::MySql);
        assert!(sql.contains("WHERE `id` > ?"), "{sql}");
        assert!(sql.contains("LIMIT ?"), "{sql}");
        assert!(sql.contains("FROM `sync_outbox`"), "{sql}");
        // A double-quoted identifier would be a string literal in MySQL.
        assert!(!sql.contains('"'), "{sql}");
    }

    #[test]
    fn the_probe_reads_no_rows_in_every_dialect() {
        let spec = allowlist::ensure_for_ingest("sync_outbox").unwrap();
        for engine in [Engine::MySql, Engine::Postgres] {
            assert!(spec.probe_sql(engine).ends_with("WHERE 1 = 0"));
        }
    }
}
