use lbug::{Database, SystemConfig};
use std::cell::Cell;
use std::path::Path;
use std::process::Command;

fn config() -> SystemConfig {
    SystemConfig::default()
        .buffer_pool_size(16 * 1024 * 1024)
        .max_db_size(256 * 1024 * 1024)
}

fn assert_child_open(path: &Path, busy: bool) {
    let result = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "child_open", "--nocapture"])
        .env("LBUG_TEST_RECOVERY_PATH", path)
        .env("LBUG_TEST_EXPECT_BUSY", if busy { "1" } else { "0" })
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stdout)
    );
}

#[test]
fn child_open() {
    let Some(path) = std::env::var_os("LBUG_TEST_RECOVERY_PATH") else {
        return;
    };
    let result = Database::new(Path::new(&path), config());
    if std::env::var("LBUG_TEST_EXPECT_BUSY").unwrap() == "1" {
        assert!(
            result
                .unwrap_err()
                .to_string()
                .contains("Could not set lock on file : ")
        );
    } else {
        assert!(result.is_ok(), "{result:?}");
    }
}

#[test]
fn callback_runs_once_with_lock_held_through_database_lifetime() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("graph.db");
    let calls = Cell::new(0);
    let db = Database::new_with_recovery(&path, config(), || {
        calls.set(calls.get() + 1);
        assert_child_open(&path, true);
        true
    })
    .unwrap();
    assert_eq!(calls.get(), 1);
    assert_child_open(&path, true);
    drop(db);
    assert_child_open(&path, false);
}

#[test]
fn callback_panic_skips_wal_replay_and_releases_lock() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("graph.db");
    drop(Database::new(&path, config()).unwrap());
    let wal = dir.path().join("graph.db.wal");
    std::fs::write(&wal, b"invalid WAL retained for proof").unwrap();
    let result =
        Database::new_with_recovery(&path, config(), || panic!("synthetic callback failure"));
    assert!(
        result
            .unwrap_err()
            .to_string()
            .contains("Before-recovery callback failed")
    );
    assert_eq!(
        std::fs::read(&wal).unwrap(),
        b"invalid WAL retained for proof"
    );
    std::fs::remove_file(&wal).unwrap();
    assert_child_open(&path, false);
}

#[test]
fn readonly_callback_is_rejected_without_running_recovery() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("graph.db");
    drop(Database::new(&path, config()).unwrap());
    let calls = Cell::new(0);
    let result = Database::new_with_recovery(&path, config().read_only(true), || {
        calls.set(1);
        true
    });
    assert!(result.is_err());
    assert_eq!(calls.get(), 0);
}

#[test]
fn callback_failure_is_typed_and_skips_replay() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("graph.db");
    drop(Database::new(&path, config()).unwrap());
    let wal = dir.path().join("graph.db.wal");
    std::fs::write(&wal, b"invalid WAL retained for proof").unwrap();
    let error = Database::new_with_recovery(&path, config(), || false).unwrap_err();
    assert!(matches!(error, lbug::Error::BeforeRecoveryFailed));
    assert_eq!(
        std::fs::read(&wal).unwrap(),
        b"invalid WAL retained for proof"
    );
    std::fs::remove_file(&wal).unwrap();
    assert_child_open(&path, false);
}
