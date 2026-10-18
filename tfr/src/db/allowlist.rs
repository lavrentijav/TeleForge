//! The allowlist of tables and columns the agent may ever name in a `SELECT`.
//!
//! # Why an allowlist and not a "we will not select secrets" promise
//!
//! `docs/transport.md` § 2.6: the agent's DB user must have no access to
//! `personality_core`, to anything holding `openai_api_key`, to `pgConnString` or to
//! `ssh*` material. A grant is the real boundary, but the *statement* the agent
//! sends is the second one: a query builder that interpolates a table name from
//! configuration is one typo away from `SELECT * FROM personality_core`, and the
//! typo would look like a legitimate read to the database.
//!
//! So the set of tables is closed ([`ALLOWLIST`]), the set of columns per table is
//! closed ([`TableSpec::columns`]), and everything else is refused **before** any
//! SQL is built or any connection is used. [`FORBIDDEN_TABLES`] and
//! [`FORBIDDEN_COLUMNS`] exist on top of that, because "refused" and "refused
//! because it is a secret table" deserve different messages in an incident report.
//!
//! # What is selectable, and what is only allowlisted
//!
//! A [`TableSpec`] has a [`Mapping`]:
//!
//! * [`Mapping::Outbox`] — rows are events already (`kind`, `entity_id`, `field`,
//!   `value`); the ingest can read them today;
//! * [`Mapping::Planned`] — the table and its columns are declared (so a grant can be
//!   prepared, and so `db.status.tables_ok` can check it), but this build refuses to
//!   *select* it as the ingest source: turning `messages.text`/`views`/`media` into
//!   events needs a mapping decision (`docs/client-integration.md` § 12.1/§ 12.9),
//!   and inventing one here would write the wrong field names into the journal.
//!
//! § 2.6 says the column names in the docs are an example ("Имена таблиц/колонок
//! выше — пример"). That is why the *selectable* set is exactly one documented shape
//! (the outbox § 2.6 recommends) and not a guess at the cloud schema.

use crate::db::Engine;
use crate::error::{Error, Result};

/// How a row of a table becomes an event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mapping {
    /// The table *is* the outbox: one row = one event, with the kind, entity, field
    /// and value given explicitly by the row.
    Outbox,
    /// Allowlisted, but this build has no row-to-event mapping. The `&'static str`
    /// names what is missing, and it is what a refusal says.
    Planned(&'static str),
}

/// One allowlisted table.
#[derive(Clone, Copy, Debug)]
pub struct TableSpec {
    pub name: &'static str,
    /// The monotone column the cursor advances on.
    pub key_column: &'static str,
    /// Exactly the columns the agent may read. Never `*`.
    pub columns: &'static [&'static str],
    pub mapping: Mapping,
}

impl TableSpec {
    /// True when the ingest can read this table today.
    pub fn is_selectable(&self) -> bool {
        matches!(self.mapping, Mapping::Outbox)
    }

    /// Columns named by the SELECT, key column first.
    pub fn select_columns(&self) -> Vec<&'static str> {
        let mut all = vec![self.key_column];
        for column in self.columns {
            if *column != self.key_column {
                all.push(column);
            }
        }
        all
    }

    /// `SELECT <allowlisted columns> FROM <table> WHERE <key> > <cursor> ORDER BY
    /// <key> ASC LIMIT <n>`, in the engine's dialect.
    ///
    /// Built from `&'static str`s that came out of this module, so there is no
    /// interpolation of caller input: the only thing a caller chooses is *which
    /// spec*, and the specs are the allowlist. The cursor and the limit are bound
    /// parameters (`$1`/`$2` on PostgreSQL, `?` on MySQL).
    pub fn select_sql(&self, engine: Engine) -> String {
        let columns = self
            .select_columns()
            .iter()
            .map(|column| quote_identifier(column, engine))
            .collect::<Vec<_>>()
            .join(", ");
        let table = quote_identifier(self.name, engine);
        let key = quote_identifier(self.key_column, engine);
        let (first, second) = match engine {
            Engine::Postgres => ("$1", "$2"),
            Engine::MySql => ("?", "?"),
        };
        format!(
            "SELECT {columns} FROM {table} WHERE {key} > {first} ORDER BY {key} ASC LIMIT {second}"
        )
    }

    /// `SELECT <columns> FROM <table> WHERE 1 = 0`: the cheapest statement that
    /// proves the grant covers exactly these columns, without reading a row.
    ///
    /// This is the `db-check` of § 2.6 п. 7 ("`SELECT 1` и `SELECT <cursor> … LIMIT
    /// 1`"): `WHERE 1 = 0` additionally fails when a *column* is not granted, which
    /// is the failure mode that matters.
    pub fn probe_sql(&self, engine: Engine) -> String {
        let columns = self
            .select_columns()
            .iter()
            .map(|column| quote_identifier(column, engine))
            .collect::<Vec<_>>()
            .join(", ");
        format!(
            "SELECT {columns} FROM {} WHERE 1 = 0",
            quote_identifier(self.name, engine)
        )
    }

    /// Refuse a column that is not in the spec, and any column that is a known
    /// secret regardless of the spec.
    pub fn ensure_column(&self, column: &str) -> Result<()> {
        ensure_not_secret_column(column)?;
        if self.columns.contains(&column) || column == self.key_column {
            return Ok(());
        }
        Err(Error::Database(crate::db::class::TABLE_NOT_ALLOWED))
    }
}

/// Quote an identifier for the engine. The input is always a compile-time constant
/// from this module — never configuration, never a wire value.
fn quote_identifier(name: &str, engine: Engine) -> String {
    match engine {
        Engine::Postgres => format!("\"{name}\""),
        Engine::MySql => format!("`{name}`"),
    }
}

/// Tables the agent may name. Adding one is a security-relevant change: it needs the
/// § 2.6 grant and the row mapping.
pub const ALLOWLIST: [TableSpec; 7] = [
    // The one shape this build reads. § 2.6: "Рекомендуемый вариант — outbox-таблица,
    // которую заполняет клиент или триггер", and it is the only shape whose mapping
    // does not need a modelling decision: the row already says what it is.
    TableSpec {
        name: "sync_outbox",
        key_column: "id",
        columns: &["kind", "entity_id", "field", "observed_ms", "value"],
        mapping: Mapping::Outbox,
    },
    // Allowlisted with the columns § 2.6 grants. Selecting them needs the
    // per-column mapping of § 12.9.
    TableSpec {
        name: "messages",
        key_column: "id",
        columns: &[
            "chat_id",
            "message_id",
            "from_id",
            "date",
            "text",
            "entities",
            "edit_date",
            "views",
            "media",
            "reply_to_msg_id",
            "grouped_id",
            "topic_id",
            "flags",
        ],
        mapping: Mapping::Planned("one event per message column (§ 12.9)"),
    },
    TableSpec {
        name: "deleted_messages",
        key_column: "id",
        // § 2.6 grants `SELECT` on the whole table and names no columns; only the key
        // is known, so only the key is declared.
        columns: &[],
        mapping: Mapping::Planned("the delete-column list (§ 12.9)"),
    },
    TableSpec {
        name: "edited_messages",
        key_column: "id",
        columns: &[],
        mapping: Mapping::Planned("the edit-column list (§ 12.9)"),
    },
    TableSpec {
        name: "online_events",
        key_column: "id",
        columns: &[],
        mapping: Mapping::Planned("the presence payload (§ 12.9)"),
    },
    TableSpec {
        name: "peer_archive_name",
        key_column: "peer_id",
        columns: &["observed_at", "name"],
        mapping: Mapping::Planned("profile field names (§ 12.9)"),
    },
    TableSpec {
        name: "archive_observations",
        key_column: "peer_id",
        columns: &["chat_id", "message_id", "kind", "observed_at"],
        mapping: Mapping::Planned("archive source names (§ 12.9)"),
    },
];

/// Tables that hold secrets. Refused by name, with a message that says *why*, so the
/// refusal is not mistaken for a typo.
pub const FORBIDDEN_TABLES: [&str; 6] = [
    "personality_core",
    "account_secrets",
    "config",
    "settings",
    "pgconnstring",
    "ssh_keys",
];

/// Column names that must never be read, on any table (§ 2.6 п. 4).
///
/// Matched case-insensitively and as a substring, because the schema of the C++ tree
/// spells the same thing several ways (`openai_api_key`, `openaiApiKey`,
/// `pgConnString`), and a check that only matches one spelling is a check that can
/// be bypassed by renaming.
pub const FORBIDDEN_COLUMNS: [&str; 6] = [
    "openai_api_key",
    "openaiapikey",
    "pgconnstring",
    "password",
    "ssh_private_key",
    "api_key",
];

/// Find an allowlisted table by exact name.
pub fn find(name: &str) -> Option<&'static TableSpec> {
    ALLOWLIST.iter().find(|spec| spec.name == name)
}

fn is_forbidden_table(name: &str) -> bool {
    let lowered = name.to_ascii_lowercase();
    FORBIDDEN_TABLES.contains(&lowered.as_str()) || lowered.contains("personality")
}

fn is_forbidden_column(column: &str) -> bool {
    let lowered = column.to_ascii_lowercase();
    FORBIDDEN_COLUMNS
        .iter()
        .any(|forbidden| lowered == *forbidden || lowered.contains(*forbidden))
}

/// Refuse a secret table or column. Separate from [`ensure_table`] so that callers
/// that build a query from configuration get the *specific* message.
pub fn ensure_not_secret_table(name: &str) -> Result<()> {
    if is_forbidden_table(name) {
        return Err(Error::Database(crate::db::class::TABLE_NOT_ALLOWED));
    }
    Ok(())
}

pub fn ensure_not_secret_column(column: &str) -> Result<()> {
    if is_forbidden_column(column) {
        return Err(Error::Database(crate::db::class::TABLE_NOT_ALLOWED));
    }
    Ok(())
}

/// The spec of `name`, or a refusal. Default-deny: a table that is merely unknown and
/// a table that is explicitly forbidden are both refused, and the secret case is
/// reported with the same class so that a status field never distinguishes them.
pub fn ensure_table(name: &str) -> Result<&'static TableSpec> {
    ensure_not_secret_table(name)?;
    find(name).ok_or(Error::Database(crate::db::class::TABLE_NOT_ALLOWED))
}

/// [`ensure_table`] plus "this build can actually read it".
pub fn ensure_for_ingest(name: &str) -> Result<&'static TableSpec> {
    let spec = ensure_table(name)?;
    match spec.mapping {
        Mapping::Outbox => Ok(spec),
        Mapping::Planned(_) => Err(Error::Database(crate::db::class::UNSUPPORTED_TABLE)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::class;

    fn class_of(result: Result<&'static TableSpec>) -> &'static str {
        match result {
            Ok(spec) => panic!("expected a refusal, got {:?}", spec.name),
            Err(Error::Database(class)) => class,
            Err(other) => panic!("expected a database error, got {other}"),
        }
    }

    #[test]
    fn the_allowlist_is_the_only_way_in() {
        assert!(find("sync_outbox").is_some());
        assert_eq!(
            class_of(ensure_table("personality_core")),
            class::TABLE_NOT_ALLOWED
        );
        assert_eq!(
            class_of(ensure_table("user_notes")),
            class::TABLE_NOT_ALLOWED
        );
        assert_eq!(class_of(ensure_table("")), class::TABLE_NOT_ALLOWED);
    }

    #[test]
    fn a_secret_table_is_refused_before_anything_else() {
        // § 2.6 п. 4, the three names that matter, plus a case variant.
        for name in [
            "personality_core",
            "Personality_Core",
            "pgConnString",
            "ssh_keys",
        ] {
            let result = ensure_table(name);
            assert!(result.is_err(), "{name} must be refused");
            assert_eq!(class_of(result), class::TABLE_NOT_ALLOWED);
        }
        // ... and even when a *grant* exists, the query builder never sees it.
        assert!(find("personality_core").is_none());
    }

    #[test]
    fn secret_columns_are_refused_on_every_table() {
        for column in ["openai_api_key", "openaiApiKey", "pgConnString", "api_key"] {
            let result = ensure_not_secret_column(column);
            assert!(result.is_err(), "{column} must be refused");
        }
        let spec = find("sync_outbox").expect("the outbox is allowlisted");
        assert!(spec.ensure_column("openai_api_key").is_err());
        // A column that is not a secret but not in the spec is refused too.
        assert!(spec.ensure_column("notes").is_err());
        assert!(spec.ensure_column("value").is_ok());
        assert!(spec.ensure_column("id").is_ok());
    }

    #[test]
    fn a_planned_table_is_allowlisted_but_not_selectable() {
        let messages = find("messages").expect("messages is allowlisted");
        assert!(!messages.is_selectable());
        assert_eq!(
            class_of(ensure_for_ingest("messages")),
            class::UNSUPPORTED_TABLE
        );
        assert!(ensure_for_ingest("sync_outbox").is_ok());
    }

    #[test]
    fn the_select_statement_names_only_allowlisted_columns() {
        let spec = ensure_for_ingest("sync_outbox").unwrap();
        let postgres = spec.select_sql(Engine::Postgres);
        assert_eq!(
            postgres,
            "SELECT \"id\", \"kind\", \"entity_id\", \"field\", \"observed_ms\", \"value\" \
             FROM \"sync_outbox\" WHERE \"id\" > $1 ORDER BY \"id\" ASC LIMIT $2"
        );
        let mysql = spec.select_sql(Engine::MySql);
        assert_eq!(
            mysql,
            "SELECT `id`, `kind`, `entity_id`, `field`, `observed_ms`, `value` \
             FROM `sync_outbox` WHERE `id` > ? ORDER BY `id` ASC LIMIT ?"
        );
        // No `*`, and no secret column anywhere in the statement.
        for sql in [postgres, mysql] {
            assert!(!sql.contains('*'), "no wildcard select: {sql}");
            assert!(!sql.to_ascii_lowercase().contains("openai"));
            assert!(!sql.to_ascii_lowercase().contains("password"));
        }
    }

    #[test]
    fn the_probe_reads_no_rows() {
        let spec = ensure_for_ingest("sync_outbox").unwrap();
        let sql = spec.probe_sql(Engine::Postgres);
        assert!(sql.ends_with("WHERE 1 = 0"), "{sql}");
        assert!(sql.starts_with("SELECT \"id\""), "{sql}");
    }

    #[test]
    fn every_spec_has_a_non_empty_name_and_key_column() {
        for spec in ALLOWLIST {
            assert!(!spec.name.is_empty());
            assert!(!spec.key_column.is_empty());
            assert!(
                !spec.columns.contains(&spec.key_column),
                "{} must not repeat its key column in `columns`",
                spec.name
            );
            // Selecting a table means selecting its key column; a planned table is
            // allowed to declare no other columns yet.
            assert!(spec.select_columns().contains(&spec.key_column));
            if spec.is_selectable() {
                assert!(
                    !spec.columns.is_empty(),
                    "{} is selectable and must declare its columns",
                    spec.name
                );
            }
        }
        // Exactly one selectable shape in this build, and it is the outbox.
        assert_eq!(
            ALLOWLIST.iter().filter(|spec| spec.is_selectable()).count(),
            1
        );
    }
}
