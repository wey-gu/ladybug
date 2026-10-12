mod startup_wal_recovery_tests {
    //! Real-engine, process-isolated startup input and retained-lock regressions.
    use lbug::{Connection, Database, Error, SystemConfig};
    use std::cell::{Cell, RefCell};
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use std::process::Command;

    fn config() -> SystemConfig {
        SystemConfig::default()
            .buffer_pool_size(32 * 1024 * 1024)
            .max_db_size(256 * 1024 * 1024)
            .max_num_threads(2)
            .auto_checkpoint(false)
            .enable_checksums(false)
    }

    fn wal(path: &Path) -> PathBuf {
        PathBuf::from(format!("{}.wal", path.display()))
    }

    fn child(path: &Path, mode: &str) {
        let output = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "startup_wal_recovery_tests::fixture_child",
                "--nocapture",
            ])
            .env("NMEM_5813_FIXTURE", path)
            .env("NMEM_5813_FIXTURE_MODE", mode)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }

    #[test]
    fn fixture_child() {
        let Some(path) = std::env::var_os("NMEM_5813_FIXTURE") else {
            return;
        };
        let path = PathBuf::from(path);
        if std::env::var("NMEM_5813_FIXTURE_MODE").unwrap() == "locked" {
            let error = Database::new(&path, config()).unwrap_err();
            assert!(
                error.to_string().contains("Could not set lock on file"),
                "{error}"
            );
            return;
        }
        let database = Database::new(&path, config()).unwrap();
        let connection = Connection::new(&database).unwrap();
        connection
            .query("CREATE NODE TABLE Memory(id STRING, content STRING, PRIMARY KEY(id))")
            .unwrap();
        connection
            .query("CREATE (:Memory {id:'durable', content:'checkpoint sentinel'})")
            .unwrap();
        connection.query("CHECKPOINT").unwrap();
        connection
            .query("CREATE (:Memory {id:'recent', content:'committed WAL sentinel'})")
            .unwrap();
        assert!(std::fs::metadata(wal(&path)).unwrap().len() > 0);
        // Model a process exit after COMMIT without running checkpoint destructors.
        std::process::exit(0);
    }

    fn seed() -> (tempfile::TempDir, PathBuf) {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("graph.db");
        child(&path, "seed");
        (directory, path)
    }

    fn damage(path: &Path) {
        // A complete committed prefix plus an unknown persisted record enum.
        std::fs::OpenOptions::new()
            .append(true)
            .open(wal(path))
            .unwrap()
            .write_all(&[255])
            .unwrap();
    }

    fn memory_ids(database: &Database) -> Vec<String> {
        let connection = Connection::new(database).unwrap();
        connection
            .query("MATCH (m:Memory) RETURN m.id ORDER BY m.id")
            .unwrap()
            .map(|row| match &row[0] {
                lbug::Value::String(id) => id.clone(),
                value => panic!("{value:?}"),
            })
            .collect()
    }

    #[test]
    fn valid_wal_replays_without_running_malformed_recovery() {
        let (_directory, path) = seed();
        let calls = Cell::new(0);
        let database = Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            calls.set(calls.get() + 1);
            false
        })
        .unwrap();
        assert_eq!(calls.get(), 0);
        assert_eq!(memory_ids(&database), ["durable", "recent"]);
    }

    #[test]
    fn malformed_record_is_typed_and_preserves_main_and_wal() {
        let (_directory, path) = seed();
        damage(&path);
        let main_before = std::fs::read(&path).unwrap();
        let wal_before = std::fs::read(wal(&path)).unwrap();
        let error = Database::new(&path, config().read_only(true)).unwrap_err();
        assert!(matches!(error, Error::MalformedWal(_)), "{error}");
        assert_eq!(std::fs::read(&path).unwrap(), main_before);
        assert_eq!(std::fs::read(wal(&path)).unwrap(), wal_before);
    }

    #[test]
    fn malformed_recovery_snapshot_retains_native_lock_and_checkpoint() {
        let (_directory, path) = seed();
        damage(&path);
        let main_before = std::fs::read(&path).unwrap();
        let wal_before = std::fs::read(wal(&path)).unwrap();
        let retained = path.with_extension("retained-wal");
        let calls = Cell::new(0);
        let identity = RefCell::new(None);
        let mut database = Database::new_with_malformed_wal_recovery(&path, config(), |snapshot| {
            calls.set(calls.get() + 1);
            assert!(snapshot.owns_named_main());
            *identity.borrow_mut() = Some(snapshot.main_identity().unwrap());
            child(&path, "locked");
            let mut bytes = vec![0; usize::try_from(snapshot.main_size()).unwrap()];
            snapshot.read_main(0, &mut bytes).unwrap();
            assert_eq!(bytes, main_before);
            child(&path, "locked");
            std::fs::rename(wal(&path), &retained).unwrap();
            true
        })
        .unwrap();
        assert_eq!(calls.get(), 1);
        assert_eq!(Some(database.identity().unwrap()), identity.into_inner());
        child(&path, "locked");
        assert_eq!(std::fs::read(retained).unwrap(), wal_before);
        // Only the checkpoint is promised; committed uncheckpointed changes can
        // be omitted and must be disclosed by the containing recovery owner.
        assert_eq!(memory_ids(&database), ["durable"]);
    }

    #[test]
    fn unknown_header_never_runs_malformed_recovery() {
        let (_directory, path) = seed();
        std::fs::write(wal(&path), b"unknown header must remain intact").unwrap();
        let before = std::fs::read(wal(&path)).unwrap();
        let calls = Cell::new(0);
        let error = Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            calls.set(calls.get() + 1);
            true
        })
        .unwrap_err();
        assert_eq!(calls.get(), 0);
        assert!(!matches!(error, Error::MalformedWal(_)));
        assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
    }

    #[test]
    fn wrong_database_wal_never_gains_malformed_recovery_authority() {
        let (_first, path) = seed();
        let (_second, other) = seed();
        let mut bytes = std::fs::read(wal(&other)).unwrap();
        bytes.push(255);
        std::fs::write(wal(&path), &bytes).unwrap();
        let main_before = std::fs::read(&path).unwrap();
        let calls = Cell::new(0);
        let error = Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            calls.set(calls.get() + 1);
            true
        })
        .unwrap_err();
        assert_eq!(calls.get(), 0);
        assert!(!matches!(error, Error::MalformedWal(_)), "{error}");
        assert!(error.to_string().contains("does not match"), "{error}");
        assert_eq!(std::fs::read(&path).unwrap(), main_before);
        assert_eq!(std::fs::read(wal(&path)).unwrap(), bytes);
    }

    #[test]
    fn checksum_configuration_mismatch_never_gains_malformed_authority() {
        let (_directory, path) = seed();
        let mut bytes = std::fs::read(wal(&path)).unwrap();
        // Native no-checksum WAL header: persisted UUID then one bool byte.
        assert_eq!(bytes[16], 0);
        bytes[16] = 1;
        bytes.push(255);
        std::fs::write(wal(&path), &bytes).unwrap();
        let calls = Cell::new(0);
        let error = Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            calls.set(calls.get() + 1);
            true
        })
        .unwrap_err();
        assert_eq!(calls.get(), 0);
        assert!(!matches!(error, Error::MalformedWal(_)), "{error}");
        assert!(error.to_string().contains("enableChecksums"), "{error}");
        assert_eq!(std::fs::read(wal(&path)).unwrap(), bytes);
    }

    #[test]
    fn recovery_snapshot_detects_displaced_main_namespace() {
        let (_directory, path) = seed();
        damage(&path);
        let before = std::fs::read(&path).unwrap();
        let moved = path.with_extension("retained-original");
        let error = Database::new_with_malformed_wal_recovery(&path, config(), |snapshot| {
            assert!(snapshot.owns_named_main());
            std::fs::rename(&path, &moved).unwrap();
            assert!(!snapshot.owns_named_main());
            false
        })
        .unwrap_err();
        assert!(matches!(error, Error::BeforeRecoveryFailed));
        assert_eq!(std::fs::read(moved).unwrap(), before);
        assert!(wal(&path).exists());
    }

    #[test]
    fn invalid_checkpoint_never_authorizes_wal_isolation() {
        let (_directory, path) = seed();
        damage(&path);
        // Keep the persisted header, truncate referenced checkpoint payload pages.
        assert!(std::fs::metadata(&path).unwrap().len() > 4096);
        std::fs::OpenOptions::new()
            .write(true)
            .open(&path)
            .unwrap()
            .set_len(4096)
            .unwrap();
        let main = std::fs::read(&path).unwrap();
        let before = std::fs::read(wal(&path)).unwrap();
        let calls = Cell::new(0);
        assert!(
            Database::new_with_malformed_wal_recovery(&path, config(), |_| {
                calls.set(calls.get() + 1);
                false
            })
            .is_err()
        );
        assert_eq!(calls.get(), 0);
        assert_eq!(std::fs::read(&path).unwrap(), main);
        assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
    }
}
