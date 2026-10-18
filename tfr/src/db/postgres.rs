//! PostgreSQL implementation of [`DbSource`].
//!
//! # The statement
//!
//! Exactly one query shape, built by [`TableSpec::select_sql`] from the allowlist, so
//! this file never concatenates anything from configuration:
//!
//! ```sql
//! SELECT "id", "kind", "entity_id", "field", "observed_ms", "value"
//!   FROM "sync_outbox" WHERE "id" > $1 ORDER BY "id" ASC LIMIT $2
//! ```
//!
//! `$1`/`$2` are bound, so a cursor or a limit can never become SQL.
//!
//! # TLS
//!
//! `sslmode=verify-full` by default (§ 2.6 п. 6 of `docs/transport.md`) through
//! rustls/`ring`, with the CA bundle named by `sslrootcert` or, when the string names
//! none, the Mozilla root store. rustls always verifies the chain *and* the name, so
//! the agent cannot be talked into skipping verification: a mode weaker than
//! `verify-full` is reported in `db.status.tls` and warned about, but the connection is
//! still verified. `sslmode=disable` never reaches this file — `conninfo` refuses it.
//!
//! # Why the driver gets the URL, not the fields
//!
//! The password must not be copied into a Rust struct that a `Debug` could print, so
//! the base URL (query string stripped, because `postgres::Config` rejects parameters
//! it does not know) is handed to `postgres::Config::from_str`, and the *policy* fields
//! (TLS mode, timeout, application name) are applied on top from [`ConnFacts`].

use std::time::{Duration, Instant};

use postgres::error::SqlState;
use postgres::{Client, Config, Row};
use rustls::RootCertStore;
use tokio_postgres_rustls::MakeRustlsConnect;

use crate::db::allowlist::TableSpec;
use crate::db::conninfo::ConnFacts;
use crate::db::ingest::{DEFAULT_CONNECT_TIMEOUT_SECS, MAX_ROW_VALUE_BYTES};
use crate::db::{class, DbRow, DbRowBody, DbSource, DbStatus, Engine, VaultState, MAX_BATCH_LIMIT};
use crate::error::{Error, Result};
use crate::secret::SecretBytes;
use crate::store::event::EventKind;

/// A connected PostgreSQL reader.
pub struct PostgresSource {
    client: Client,
    spec: &'static TableSpec,
    facts: ConnFacts,
    latency_ms: u64,
    tables_ok: bool,
    last_error: Option<&'static str>,
}

impl PostgresSource {
    /// Connect, `SELECT 1`, and probe the grant of the configured table.
    pub fn connect(
        facts: &ConnFacts,
        connection_string: &SecretBytes,
        spec: &'static TableSpec,
    ) -> Result<Self> {
        let text = std::str::from_utf8(connection_string.expose())
            .map_err(|_| Error::Database(class::CONFIG))?;
        let base = crate::db::conninfo::base_url(text);
        // The driver parses user/password/host/dbname itself, so the password is
        // never copied into a struct of ours.
        let mut config: Config = base.parse().map_err(|_| Error::Database(class::CONFIG))?;
        config.ssl_mode(postgres::config::SslMode::Require);
        config.application_name("teleforge-agent");
        config.connect_timeout(Duration::from_secs(
            facts
                .connect_timeout_secs
                .unwrap_or(DEFAULT_CONNECT_TIMEOUT_SECS),
        ));

        let started = Instant::now();
        let mut client = config
            .connect(tls_connector(facts)?)
            .map_err(|error| Error::Database(classify(&error)))?;

        // `SELECT 1`: § 2.6 п. 7 "проверка при установке". It also populates the
        // latency the status reports.
        client
            .simple_query("SELECT 1")
            .map_err(|error| Error::Database(classify(&error)))?;
        let latency_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);

        // The grant probe: `WHERE 1 = 0` reads no row but fails on a missing column
        // grant, which is the failure that matters.
        let tables_ok = client
            .query(spec.probe_sql(Engine::Postgres).as_str(), &[])
            .is_ok();

        Ok(Self {
            client,
            spec,
            facts: facts.clone(),
            latency_ms,
            tables_ok,
            last_error: None,
        })
    }
}

impl DbSource for PostgresSource {
    fn status(&self) -> DbStatus {
        DbStatus {
            connected: !self.client.is_closed(),
            latency_ms: self.latency_ms,
            engine: Some(Engine::Postgres.as_str()),
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
        let limit = i64::try_from(limit).map_err(|_| Error::Database(class::TOO_MANY_EVENTS))?;
        let started = Instant::now();
        let sql = self.spec.select_sql(Engine::Postgres);
        let rows: Vec<Row> = self
            .client
            .query(sql.as_str(), &[&cursor, &limit])
            .map_err(|error| Error::Database(classify(&error)))?;
        self.latency_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
        let mut out = Vec::with_capacity(rows.len());
        for row in &rows {
            out.push(decode_row(row, self.spec)?);
        }
        Ok(out)
    }
}

/// Turn one PostgreSQL row into a [`DbRow`].
///
/// A row that cannot be decoded becomes [`DbRowBody::Malformed`] rather than an
/// error, so one bad row cannot stop the cursor: the ingest counts it as `skipped`
/// and moves on. The *key* is different — without it there is nothing to advance to,
/// so a key that is not an `i64` is a configuration error for the whole table.
fn decode_row(row: &Row, spec: &'static TableSpec) -> Result<DbRow> {
    let key: i64 = row
        .try_get(spec.key_column)
        .map_err(|_| Error::Database(class::DECODE))?;
    let observed_ms: i64 = row.try_get("observed_ms").unwrap_or(0);
    let kind_text: String = match row.try_get("kind") {
        Ok(text) => text,
        Err(_) => {
            return Ok(DbRow {
                key,
                observed_ms,
                body: DbRowBody::Malformed,
            })
        }
    };
    let Some(kind) = EventKind::parse(&kind_text) else {
        return Ok(DbRow {
            key,
            observed_ms,
            body: DbRowBody::Malformed,
        });
    };
    let entity_id: String = match row.try_get("entity_id") {
        Ok(entity) => entity,
        Err(_) => {
            return Ok(DbRow {
                key,
                observed_ms,
                body: DbRowBody::Malformed,
            })
        }
    };
    let field: Option<String> = row.try_get("field").ok().flatten();
    let value: Vec<u8> = match row.try_get::<_, Vec<u8>>("value") {
        Ok(bytes) => bytes,
        // A `TEXT` column is the other sensible choice for an outbox, so accept it.
        Err(_) => match row.try_get::<_, String>("value") {
            Ok(text) => text.into_bytes(),
            Err(_) => {
                return Ok(DbRow {
                    key,
                    observed_ms,
                    body: DbRowBody::Malformed,
                })
            }
        },
    };
    if value.len() > MAX_ROW_VALUE_BYTES {
        return Ok(DbRow {
            key,
            observed_ms,
            body: DbRowBody::Malformed,
        });
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

/// rustls connector that always verifies the chain and the host name.
fn tls_connector(facts: &ConnFacts) -> Result<MakeRustlsConnect> {
    let mut roots = RootCertStore::empty();
    match &facts.root_cert {
        Some(path) => {
            let pem = std::fs::read(path)?;
            let mut reader = std::io::BufReader::new(&pem[..]);
            let mut added = 0usize;
            for certificate in rustls_pemfile::certs(&mut reader) {
                let certificate = certificate.map_err(|_| Error::Database(class::CONFIG))?;
                roots
                    .add(certificate)
                    .map_err(|_| Error::Database(class::CONFIG))?;
                added += 1;
            }
            if added == 0 {
                return Err(Error::Database(class::CONFIG));
            }
        }
        None => roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned()),
    }
    let config = rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    Ok(MakeRustlsConnect::new(config))
}

/// Map a driver error to a [`class`]. Never keeps the driver's message: it can echo
/// the connection string.
fn classify(error: &postgres::Error) -> &'static str {
    if let Some(db) = error.as_db_error() {
        let code = db.code();
        // 28P01 `invalid_password` / 28000 `invalid_authorization_specification`:
        // the server answered, so this is a credential problem, not connectivity.
        if code == &SqlState::INVALID_PASSWORD
            || code == &SqlState::INVALID_AUTHORIZATION_SPECIFICATION
        {
            return class::AUTH;
        }
        // 42501 `insufficient_privilege`, 42P01 `undefined_table`, 42703
        // `undefined_column`: the grant of § 2.6 does not cover what we asked for.
        // Every other SQLSTATE is also a query problem from the ingest's point of
        // view — the connection itself worked.
        return class::QUERY;
    }
    // No server message: the socket, the TLS handshake or the timeout failed.
    class::CONNECT
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::allowlist;

    #[test]
    fn the_statement_is_built_from_the_allowlist_and_a_bound_cursor() {
        let spec = allowlist::ensure_for_ingest("sync_outbox").unwrap();
        let sql = spec.select_sql(Engine::Postgres);
        // Bound parameters, quoted identifiers, no interpolation of values.
        assert!(sql.contains("WHERE \"id\" > $1"));
        assert!(sql.contains("LIMIT $2"));
        assert!(sql.contains("FROM \"sync_outbox\""));
    }

    #[test]
    fn an_allowed_table_is_probed_without_reading_a_row() {
        let spec = allowlist::ensure_for_ingest("sync_outbox").unwrap();
        assert!(spec.probe_sql(Engine::Postgres).ends_with("WHERE 1 = 0"));
    }

    #[test]
    fn tls_configuration_accepts_the_bundled_roots() {
        let facts = crate::db::conninfo::parse("postgres://u:p@db.example.com/tf")
            .expect("a default URL must parse");
        let connector = tls_connector(&facts);
        assert!(connector.is_ok(), "the Mozilla root store must build");
    }

    #[test]
    fn a_missing_ca_file_is_a_config_error_not_a_panic() {
        let facts =
            crate::db::conninfo::parse("postgres://u:p@db.example.com/tf?sslrootcert=/nope/ca.pem")
                .expect("the URL itself is well formed");
        let error = tls_connector(&facts).err().expect("must fail");
        assert!(
            matches!(error, Error::Io(_) | Error::Database(class::CONFIG)),
            "{error}"
        );
    }
}
