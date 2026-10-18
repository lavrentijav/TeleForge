//! `--self-test`: the installer's gate.
//!
//! The desktop client runs this *before* it commits a new agent version, so the
//! contract is:
//!
//! * cheap (no network, no vault unlock, no TDLib call beyond a constant check);
//! * machine-readable (one JSON object on stdout, nothing else);
//! * non-zero exit when anything the *next* version depends on is broken;
//! * never panics: a check that fails becomes a `fail` line, not a crash.
//!
//! What is checked, and why each one exists:
//!
//! | Check | Failure it catches |
//! |---|---|
//! | `config` | a malformed `agent.toml` the daemon would refuse anyway |
//! | `permissions` | a data dir group/other can read — the salt sits next to ciphertext (M14) |
//! | `store.schema` | a `data.db` written by a different build: missing table/index, WAL off, `busy_timeout=0` (H6), schema version mismatch |
//! | `store.txn` | a connection that cannot complete `BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK` |
//! | `keystore.kdf` | a build where Argon2id is a stub or missing (C1 would silently come back) |
//! | `keystore.aead` | a build where the AEAD does not authenticate |
//! | `fsperm` | a platform where `0600` cannot be enforced *or verified* (warn, not fail) |
//! | `tdlib` | a TDLib older than the minimum (constant self-consistency only) |
//! | `release-root-key` | a binary shipped without the compiled-in release key |
//!
//! Exit codes: `0` all checks pass or warn, [`crate::error::exit::SELF_TEST_FAILED`]
//! if any check fails, [`crate::error::exit::CONFIG`] if the report itself cannot
//! be produced.

use crate::error::{Error, Result};
use crate::fsperm;
use crate::keystore;
use crate::store;

/// Outcome of one check.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Pass,
    /// Not a failure, but the operator must know (e.g. a host without POSIX modes).
    Warn,
    Fail,
}

impl Status {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Pass => "pass",
            Self::Warn => "warn",
            Self::Fail => "fail",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Check {
    pub name: String,
    pub status: Status,
    pub detail: String,
}

impl Check {
    pub fn pass(name: &str, detail: impl Into<String>) -> Self {
        Self {
            name: name.to_string(),
            status: Status::Pass,
            detail: detail.into(),
        }
    }

    pub fn warn(name: &str, detail: impl Into<String>) -> Self {
        Self {
            name: name.to_string(),
            status: Status::Warn,
            detail: detail.into(),
        }
    }

    pub fn fail(name: &str, detail: impl Into<String>) -> Self {
        Self {
            name: name.to_string(),
            status: Status::Fail,
            detail: detail.into(),
        }
    }
}

/// Where a failure should send the operator.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExitHint {
    /// Nothing failed.
    Ok,
    /// A check failed: the binary must not be installed.
    SelfTestFailed,
}

#[derive(Clone, Debug, Default)]
pub struct Report {
    pub checks: Vec<Check>,
}

impl Report {
    pub fn push(&mut self, check: Check) {
        self.checks.push(check);
    }

    pub fn failures(&self) -> Vec<&Check> {
        self.checks
            .iter()
            .filter(|check| check.status == Status::Fail)
            .collect()
    }

    pub fn warnings(&self) -> Vec<&Check> {
        self.checks
            .iter()
            .filter(|check| check.status == Status::Warn)
            .collect()
    }

    pub fn ok(&self) -> bool {
        self.failures().is_empty()
    }

    pub fn exit_hint(&self) -> ExitHint {
        if self.ok() {
            ExitHint::Ok
        } else {
            ExitHint::SelfTestFailed
        }
    }

    pub fn exit_code(&self) -> i32 {
        match self.exit_hint() {
            ExitHint::Ok => crate::error::exit::OK,
            ExitHint::SelfTestFailed => crate::error::exit::SELF_TEST_FAILED,
        }
    }

    pub fn to_json(&self) -> String {
        let escape = crate::api::protocol::json_escape;
        let checks: Vec<String> = self
            .checks
            .iter()
            .map(|check| {
                format!(
                    "{{\"name\":\"{}\",\"status\":\"{}\",\"detail\":\"{}\"}}",
                    escape(&check.name),
                    check.status.as_str(),
                    escape(&check.detail)
                )
            })
            .collect();
        let failures: Vec<String> = self
            .failures()
            .iter()
            .map(|check| format!("\"{}\"", escape(&check.name)))
            .collect();
        format!(
            concat!(
                "{{\"agent_version\":\"{}\",\"protocol\":{},\"ok\":{},\"exit_code\":{},",
                "\"failures\":[{}],\"checks\":[{}]}}"
            ),
            crate::AGENT_VERSION,
            crate::PROTOCOL_VERSION,
            self.ok(),
            self.exit_code(),
            failures.join(","),
            checks.join(",")
        )
    }
}

/// Run every check. `tenant` is optional: without it the database checks run
/// against an in-memory store (which proves the schema loads but not that the
/// installed file is healthy); with it they run against
/// `<data_dir>/tenants/<tenant>/sync/data.db`.
pub fn run(data_dir: &str, tenant: Option<&str>) -> Report {
    let mut report = Report::default();

    // Permissions of the data dir itself. A directory that does not exist yet is
    // not a failure: the installer runs `self-test` before the first `serve`.
    if !std::path::Path::new(data_dir).exists() {
        report.push(Check::warn(
            "permissions",
            format!("{data_dir} does not exist yet; `serve` will create it 0700"),
        ));
    } else {
        match fsperm::assert_private(data_dir) {
            Ok(()) if fsperm::posix_modes_enforced() => {
                report.push(Check::pass("permissions", format!("{data_dir} is 0700")));
            }
            Ok(()) => report.push(Check::warn(
                "permissions",
                "POSIX modes are not available on this platform; the DACL is set but cannot be verified",
            )),
            Err(error) => report.push(Check::fail("permissions", error.to_string())),
        }
    }

    // The passive store: schema, pragmas and one transaction roundtrip.
    match tenant {
        Some(tenant) => {
            let path = crate::vault::sync_db_path(data_dir, tenant);
            match store::Store::open(&path) {
                Ok(mut open) => {
                    match store::self_check(&open) {
                        Ok(check) => {
                            let problems = check.problems();
                            if problems.is_empty() {
                                report.push(Check::pass(
                                    "store.schema",
                                    format!("{} is schema {}", path, store::SCHEMA_VERSION),
                                ));
                            } else {
                                report.push(Check::fail("store.schema", problems.join("; ")));
                            }
                        }
                        Err(error) => report.push(Check::fail("store.schema", error.to_string())),
                    }
                    report.push(transaction_check(&mut open));
                }
                Err(error) => report.push(Check::fail(
                    "store.schema",
                    format!("cannot open {path}: {error}"),
                )),
            }
        }
        None => match store::Store::open_in_memory() {
            Ok(mut open) => {
                match store::self_check(&open) {
                    Ok(check) => {
                        // An in-memory store cannot do WAL; every other property
                        // still applies, so only that problem is tolerated.
                        let problems: Vec<String> = check
                            .problems()
                            .into_iter()
                            .filter(|problem| !problem.contains("journal_mode"))
                            .collect();
                        if problems.is_empty() {
                            report.push(Check::pass(
                                "store.schema",
                                format!("in-memory schema {} loads", store::SCHEMA_VERSION),
                            ));
                        } else {
                            report.push(Check::fail("store.schema", problems.join("; ")));
                        }
                    }
                    Err(error) => report.push(Check::fail("store.schema", error.to_string())),
                }
                report.push(transaction_check(&mut open));
            }
            Err(error) => report.push(Check::fail("store.schema", error.to_string())),
        },
    }

    // Migration machinery: the version row and every object v2 added. A store that
    // opens but is silently still v1 would answer `sync.conflicts` with "no
    // conflicts" forever, which is worse than refusing to start.
    report.push(migration_check());

    // KDF and AEAD: the two primitives whose absence re-opens C1.
    match keystore::probe() {
        Ok(()) => report.push(Check::pass(
            "keystore.kdf",
            "argon2id floor cost derives deterministically",
        )),
        Err(error) => report.push(Check::fail("keystore.kdf", error.to_string())),
    }
    match keystore_roundtrip() {
        Ok(()) => report.push(Check::pass(
            "keystore.aead",
            "xchacha20-poly1305 seals, opens, and rejects a wrong key",
        )),
        Err(error) => report.push(Check::fail("keystore.aead", error.to_string())),
    }

    // Recovery: the second wrap of the account DEK. Checked with the real
    // primitives, because a recovery path that is a stub would only be discovered
    // by the user who lost their passphrase.
    report.push(recovery_check(data_dir));

    // Filesystem enforcement capability.
    if fsperm::posix_modes_enforced() {
        report.push(Check::pass(
            "fsperm",
            "POSIX modes are enforced and verified",
        ));
    } else {
        report.push(Check::warn(
            "fsperm",
            "no POSIX mode bits on this platform; private files rely on the DACL",
        ));
    }

    match crate::tdlib::check_version(crate::tdlib::MIN_TDLIB_VERSION) {
        Ok(()) => report.push(Check::pass(
            "tdlib",
            "minimum version constant is self-consistent",
        )),
        Err(error) => report.push(Check::fail("tdlib", error.to_string())),
    }

    match crate::crypto::sig::compiled_root_pubkey() {
        Ok(_) => report.push(Check::pass("release-root-key", "compiled in")),
        // A `cargo build` without the release key is a development build: warning
        // here keeps `cargo test` meaningful, while a shipped (non-debug) binary
        // that cannot verify manifests still fails the gate.
        Err(error) if cfg!(debug_assertions) => report.push(Check::warn(
            "release-root-key",
            format!("{error} (development build)"),
        )),
        Err(error) => report.push(Check::fail("release-root-key", error.to_string())),
    }

    report
}

/// `BEGIN IMMEDIATE` → `ROLLBACK`, then a real write/read/rollback sequence.
/// This is the C4 property, checked at runtime as well as in `store::conn::tests`.
fn transaction_check(store: &mut store::Store) -> Check {
    let account = match store::AccountId::new([0x5a; 16]) {
        Ok(account) => account,
        Err(error) => return Check::fail("store.txn", error.to_string()),
    };
    if !store.is_idle() {
        return Check::fail(
            "store.txn",
            "the connection is already inside a transaction",
        );
    }
    match store.transaction(|tx| {
        let _ = tx.next_op_seq(&account, 0)?;
        Err::<(), _>(Error::Invalid("self-test rollback probe".to_string()))
    }) {
        Ok(()) => Check::fail("store.txn", "a failing transaction reported success"),
        Err(_) => {
            if store.is_idle() {
                Check::pass("store.txn", "ROLLBACK leaves the connection usable")
            } else {
                Check::fail(
                    "store.txn",
                    "a failed transaction left the connection open (the C4 bug)",
                )
            }
        }
    }
}

/// Do the v2 objects exist, and does the version row say so?
fn migration_check() -> Check {
    use crate::store::migrate;
    use crate::store::schema::{SCHEMA_VERSION, V2_ADDED_COLUMNS};
    let open = match store::Store::open_in_memory() {
        Ok(open) => open,
        Err(error) => return Check::fail("store.migrate", error.to_string()),
    };
    let version = match migrate::read_version(open.connection()) {
        Ok(version) => version,
        Err(error) => return Check::fail("store.migrate", error.to_string()),
    };
    if version != SCHEMA_VERSION {
        return Check::fail(
            "store.migrate",
            format!("schema_version is {version}, this build writes {SCHEMA_VERSION}"),
        );
    }
    for (table, column) in V2_ADDED_COLUMNS {
        match migrate::column_exists(open.connection(), table, column) {
            Ok(true) => {}
            Ok(false) => {
                return Check::fail(
                    "store.migrate",
                    format!("{table}.{column} was added in v2 but is missing"),
                )
            }
            Err(error) => return Check::fail("store.migrate", error.to_string()),
        }
    }
    if !migrate::table_exists(open.connection(), "conflicts").unwrap_or(false) {
        return Check::fail("store.migrate", "the v2 `conflicts` table is missing");
    }
    Check::pass(
        "store.migrate",
        format!("schema {SCHEMA_VERSION} objects are present and the version row agrees"),
    )
}

/// Create an envelope with a recovery code, unlock it with that code, and confirm a
/// wrong code is refused.
///
/// Runs against a scratch directory inside `data_dir` (removed afterwards) so the
/// probe exercises the real file layout, `0600` writer and JSON parse rather than
/// only the in-memory primitives. It never touches a tenant's files.
fn recovery_check(data_dir: &str) -> Check {
    use crate::crypto::kdf::KdfCost;
    use crate::keystore::{Argon2Kdf, PrimitiveAead, SecretStore, RECOVERY_CODE_LEN};
    use crate::secret::SecretBytes;
    use crate::sync::clock::Clock;

    if !std::path::Path::new(data_dir).exists() {
        return Check::warn(
            "keystore.recovery",
            format!("{data_dir} does not exist yet; the recovery probe needs a writable directory"),
        );
    }
    let scratch = format!("{data_dir}/.self-test/recovery");
    let store = SecretStore::new(&scratch);
    let account = match store::AccountId::new([0x6b; 16]) {
        Ok(account) => account,
        Err(error) => return Check::fail("keystore.recovery", error.to_string()),
    };
    let passphrase = SecretBytes::new(b"self-test recovery passphrase".to_vec());
    let outcome = store
        .create_with_recovery(
            &Argon2Kdf,
            &PrimitiveAead::new(),
            &account,
            &passphrase,
            KdfCost::floor(),
            crate::sync::clock::SystemClock.now_ms(),
        )
        .and_then(|(envelope, unlocked, code)| {
            let wrong = SecretBytes::new(vec![0u8; RECOVERY_CODE_LEN]);
            let wrong_result = store.unlock_with_recovery(&account, &wrong, &envelope);
            let right = store.unlock_with_recovery(&account, &code, &envelope)?;
            if wrong_result.is_ok() {
                return Err(Error::Crypto(
                    "the recovery path accepted a wrong code".to_string(),
                ));
            }
            if right.dek().expose() != unlocked.dek().expose() {
                return Err(Error::Crypto(
                    "the recovery path unwrapped a different DEK".to_string(),
                ));
            }
            Ok(())
        });
    // Best effort: a probe must not leave a directory behind, but failing to remove
    // it is not a reason to fail the install.
    // Remove the whole scratch tree, not just the leaf: `.self-test` is ours and an
    // empty directory left in the data dir is noise the operator will ask about.
    let _ = std::fs::remove_dir_all(format!("{data_dir}/.self-test"));
    match outcome {
        Ok(()) => Check::pass(
            "keystore.recovery",
            "a recovery code unwraps the DEK and a wrong code is refused",
        ),
        Err(error) => Check::fail("keystore.recovery", error.to_string()),
    }
}

fn keystore_roundtrip() -> Result<()> {
    use crate::secret::SecretBytes;
    let key = SecretBytes::generate(crate::crypto::kdf::KEY_LEN)?;
    let sealed = crate::crypto::aead::seal(
        &key,
        1,
        crate::crypto::aead::AeadAlgorithm::XChaCha20Poly1305,
        b"tf/self-test",
        b"roundtrip",
    )?;
    let opened = crate::crypto::aead::open(&key, b"tf/self-test", &sealed)?;
    if opened.expose() != b"roundtrip" {
        return Err(Error::Crypto(
            "AEAD roundtrip changed the plaintext".to_string(),
        ));
    }
    let wrong = SecretBytes::generate(crate::crypto::kdf::KEY_LEN)?;
    if crate::crypto::aead::open(&wrong, b"tf/self-test", &sealed).is_ok() {
        return Err(Error::Crypto(
            "AEAD accepted a wrong key; it is not authenticating".to_string(),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TestDir;

    #[test]
    fn a_fresh_directory_passes_without_a_tenant() {
        let directory = TestDir::new("selftest");
        let report = run(&directory.path().to_string_lossy(), None);
        let failures: Vec<&str> = report
            .failures()
            .iter()
            .map(|check| check.name.as_str())
            .collect();
        assert!(report.ok(), "unexpected failures: {failures:?}");
        assert_eq!(report.exit_code(), crate::error::exit::OK);
        let names: Vec<&str> = report
            .checks
            .iter()
            .map(|check| check.name.as_str())
            .collect();
        for expected in [
            "permissions",
            "store.schema",
            "store.migrate",
            "store.txn",
            "keystore.kdf",
            "keystore.aead",
            "keystore.recovery",
            "fsperm",
            "tdlib",
            "release-root-key",
        ] {
            assert!(names.contains(&expected), "missing check {expected}");
        }
    }

    #[test]
    fn a_tenant_makes_the_store_check_target_the_real_file() {
        let directory = TestDir::new("selftest-tenant");
        let data_dir = directory.path().to_string_lossy().to_string();
        let tenant = "00112233445566778899aabbccddeeff";
        let report = run(&data_dir, Some(tenant));
        assert!(report.ok(), "{:?}", report.failures());
        let schema = report
            .checks
            .iter()
            .find(|check| check.name == "store.schema")
            .expect("store.schema check");
        assert_eq!(schema.status, Status::Pass);
        assert!(schema.detail.contains(tenant));
        assert!(std::path::Path::new(&crate::vault::sync_db_path(&data_dir, tenant)).exists());
    }

    #[test]
    fn a_missing_data_dir_warns_instead_of_failing() {
        let directory = TestDir::new("selftest-missing");
        let missing = directory.join("not-created-yet");
        let report = run(&missing.to_string_lossy(), None);
        assert!(report.ok());
        assert!(report
            .warnings()
            .iter()
            .any(|check| check.detail.contains("does not exist")));
    }

    #[test]
    fn a_group_readable_data_dir_fails_the_permissions_check() {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let directory = TestDir::new("selftest-perms");
            std::fs::set_permissions(directory.path(), std::fs::Permissions::from_mode(0o755))
                .unwrap();
            let report = run(&directory.path().to_string_lossy(), None);
            assert!(!report.ok());
            assert_eq!(report.exit_code(), crate::error::exit::SELF_TEST_FAILED);
        }
        // On Windows there are no POSIX modes to widen; the DACL path is covered
        // by `fsperm::posix_modes_enforced` being reported as a warning instead.
    }

    #[test]
    fn report_json_lists_failures_and_the_exit_code() {
        let mut report = Report::default();
        report.push(Check::pass("store.schema", "ok"));
        assert!(report.to_json().contains("\"ok\":true"));
        assert_eq!(report.exit_hint(), ExitHint::Ok);

        report.push(Check::fail("keystore.kdf", "argon2 missing"));
        let json = report.to_json();
        assert!(json.contains("\"ok\":false"));
        assert!(json.contains("\"exit_code\":7"));
        assert!(json.contains("\"failures\":[\"keystore.kdf\"]"));
        assert!(json.contains("\"status\":\"fail\""));
        assert_eq!(report.exit_code(), crate::error::exit::SELF_TEST_FAILED);
        assert_eq!(report.exit_hint(), ExitHint::SelfTestFailed);
        assert_eq!(report.failures().len(), 1);
    }

    #[test]
    fn status_names_are_stable() {
        assert_eq!(Status::Pass.as_str(), "pass");
        assert_eq!(Status::Warn.as_str(), "warn");
        assert_eq!(Status::Fail.as_str(), "fail");
        let mut report = Report::default();
        report.push(Check::warn("fsperm", "no posix modes"));
        assert!(report.ok(), "a warning must not fail the install");
        assert_eq!(report.warnings().len(), 1);
        assert!(report.to_json().contains("\"status\":\"warn\""));
    }

    #[test]
    fn json_escaping_is_applied_to_details() {
        let mut report = Report::default();
        report.push(Check::fail("store.schema", "path \"with\" quotes\\and"));
        let json = report.to_json();
        assert!(json.contains("\\\"with\\\""));
        assert!(json.contains("\\\\and"));
    }

    #[test]
    fn the_transaction_probe_reports_a_usable_connection() {
        let mut store = store::Store::open_in_memory().unwrap();
        let check = transaction_check(&mut store);
        assert_eq!(check.status, Status::Pass, "detail: {}", check.detail);
        assert!(store.is_idle());
    }
}
