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
        if std::env::var("NMEM_5813_FIXTURE_MODE").unwrap() == "prefix-checksummed" {
            let database = Database::new(&path, config().enable_checksums(true)).unwrap();
            let connection = Connection::new(&database).unwrap();
            connection
                .query("CREATE NODE TABLE Memory(id STRING, content STRING, PRIMARY KEY(id))")
                .unwrap();
            connection
                .query("CREATE (:Memory {id:'durable', content:'checkpoint sentinel'})")
                .unwrap();
            connection.query("CHECKPOINT").unwrap();
            let mut ends = Vec::new();
            for ordinal in 1..=3 {
                connection.query(&format!("CREATE (:Memory {{id:'recent{ordinal}', content:'committed value {ordinal}'}})")).unwrap();
                ends.push(std::fs::metadata(wal(&path)).unwrap().len().to_string());
            }
            std::fs::write(path.parent().unwrap().join("commit-ends"), ends.join("\n")).unwrap();
            std::process::exit(0);
        }
        if std::env::var("NMEM_5813_FIXTURE_MODE").unwrap() == "large-record-checksummed" {
            let database = Database::new(&path, config().enable_checksums(true)).unwrap();
            let connection = Connection::new(&database).unwrap();
            connection
                .query("CREATE NODE TABLE Memory(id STRING, content STRING, PRIMARY KEY(id))")
                .unwrap();
            connection
                .query("CREATE (:Memory {id:'durable', content:'checkpoint sentinel'})")
                .unwrap();
            connection.query("CHECKPOINT").unwrap();
            // One committed record larger than the 4 KiB checksum entry buffer.
            connection
                .query(&format!(
                    "CREATE (:Memory {{id:'large', content:'{}'}})",
                    large_content()
                ))
                .unwrap();
            connection
                .query("CREATE (:Memory {id:'after-large', content:'later commit'})")
                .unwrap();
            assert!(std::fs::metadata(wal(&path)).unwrap().len() > 4096);
            // Model an unclean exit: the shutdown checkpoint never runs.
            std::process::exit(0);
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

    fn large_content() -> String {
        (0..600).map(|n| format!("chunk{n:04} ")).collect()
    }

    #[test]
    fn checksummed_record_larger_than_entry_buffer_replays_after_unclean_exit() {
        // https://github.com/nowledge-co/mem/issues/8009: growing the checksum
        // entry buffer dropped the bytes already accumulated for the record, so
        // every committed record above 4 KiB was persisted with a stale head and
        // could never be replayed.
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("graph.db");
        child(&path, "large-record-checksummed");
        let database = Database::new(&path, config().enable_checksums(true))
            .expect("a complete checksummed WAL must replay after an unclean exit");
        assert_eq!(memory_ids(&database), ["after-large", "durable", "large"]);
        let connection = Connection::new(&database).unwrap();
        let content: Vec<_> = connection
            .query("MATCH (m:Memory {id:'large'}) RETURN m.content")
            .unwrap()
            .map(|row| row[0].to_string())
            .collect();
        assert_eq!(content, [large_content()]);
    }

    #[test]
    fn undecodable_record_body_reports_verified_prefix_instead_of_internal_assertion() {
        // A record head that decodes as a valid record type but carries an
        // unsupported physical type used to raise KU_UNREACHABLE in
        // common/types/types.cpp before its checksum was reached. That
        // InternalException bypassed malformed-WAL classification, so copied
        // recovery was refused with no verified committed prefix.
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("graph.db");
        child(&path, "prefix-checksummed");
        let ends: Vec<u64> = std::fs::read_to_string(directory.path().join("commit-ends"))
            .unwrap()
            .lines()
            .map(|v| v.parse().unwrap())
            .collect();
        let mut bytes = std::fs::read(wal(&path)).unwrap();
        let last = usize::try_from(ends[1]).unwrap()..usize::try_from(ends[2]).unwrap();
        // LogicalType STRING (50), PhysicalType STRING (20), internal category.
        let at = bytes[last.clone()]
            .windows(3)
            .position(|window| window == [50, 20, 0])
            .expect("serialized STRING vector type in the last transaction")
            + last.start;
        bytes[at + 1] = 19; // Not a fixed-size or variable-size physical type.
        std::fs::write(wal(&path), &bytes).unwrap();
        let original_main = std::fs::read(&path).unwrap();
        let observed = RefCell::new(None);
        let error = Database::new_with_malformed_wal_recovery(
            &path,
            config().enable_checksums(true),
            |snapshot| {
                *observed.borrow_mut() = snapshot.verified_prefix();
                false
            },
        )
        .unwrap_err();
        assert!(
            matches!(error, Error::MalformedWalRecoveryFailed(_)),
            "{error}"
        );
        let prefix = observed.into_inner().expect("native verified boundary");
        assert_eq!(
            prefix.stop_reason,
            lbug::WalPrefixStopReason::UndecodableRecord
        );
        assert_eq!(prefix.commit_end, ends[1]);
        assert_eq!(prefix.transaction_count, 2);
        assert_eq!(prefix.wal_size, bytes.len() as u64);
        assert!(prefix.failed_record_start >= ends[1]);
        assert!(prefix.failed_record_start < ends[2]);
        assert!(!prefix.zero_suffix);
        assert_eq!(std::fs::read(&path).unwrap(), original_main);
        assert_eq!(std::fs::read(wal(&path)).unwrap(), bytes);
        let candidate = directory.path().join("candidate");
        std::fs::create_dir(&candidate).unwrap();
        let copied = candidate.join("graph.db");
        std::fs::write(&copied, &original_main).unwrap();
        std::fs::write(
            wal(&copied),
            &bytes[..usize::try_from(prefix.commit_end).unwrap()],
        )
        .unwrap();
        let database = Database::new(&copied, config().enable_checksums(true)).unwrap();
        assert_eq!(memory_ids(&database), ["durable", "recent1", "recent2"]);
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
    fn complete_uncommitted_tail_keeps_ordinary_strict_crash_recovery() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("graph.db");
        child(&path, "prefix-checksummed");
        let ends: Vec<u64> = std::fs::read_to_string(directory.path().join("commit-ends"))
            .unwrap()
            .lines()
            .map(|v| v.parse().unwrap())
            .collect();
        let mut bytes = std::fs::read(wal(&path)).unwrap();
        let probe_dir = directory.path().join("probe");
        std::fs::create_dir(&probe_dir).unwrap();
        let probe = probe_dir.join("graph.db");
        std::fs::copy(&path, &probe).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 1;
        std::fs::write(wal(&probe), &bytes).unwrap();
        let observed = RefCell::new(None);
        assert!(Database::new_with_malformed_wal_recovery(
            &probe,
            config().enable_checksums(true),
            |snapshot| {
                *observed.borrow_mut() = snapshot.verified_prefix();
                false
            }
        )
        .is_err());
        let report = observed.into_inner().unwrap();
        assert_eq!(
            report.stop_reason,
            lbug::WalPrefixStopReason::ChecksumMismatch
        );
        assert_eq!(report.commit_end, ends[1]);
        // The native parser supplies the last COMMIT's start; no record-size
        // assumptions enter this fully checksummed, unfinished transaction.
        bytes[last] ^= 1;
        bytes.truncate(usize::try_from(report.failed_record_start).unwrap());
        std::fs::write(wal(&path), &bytes).unwrap();
        let database = Database::new(&path, config().enable_checksums(true))
            .expect("complete uncommitted records must roll back during ordinary strict open");
        assert_eq!(memory_ids(&database), ["durable", "recent1", "recent2"]);
        assert_eq!(std::fs::metadata(wal(&path)).unwrap().len(), ends[1]);
        let connection = Connection::new(&database).unwrap();
        connection
            .query("CREATE (:Memory {id:'after-recovery', content:'new strict write'})")
            .unwrap();
        connection.query("CHECKPOINT").unwrap();
        drop(connection);
        drop(database);
        let reopened =
            Database::new(&path, config().enable_checksums(true).read_only(true)).unwrap();
        assert_eq!(
            memory_ids(&reopened),
            ["after-recovery", "durable", "recent1", "recent2"]
        );
    }

    #[test]
    fn checksummed_prefix_preserves_exact_complete_transactions_on_copies() {
        for damage_kind in ["zero", "torn-checksum", "torn-record", "middle"] {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("graph.db");
            child(&path, "prefix-checksummed");
            let ends: Vec<u64> = std::fs::read_to_string(directory.path().join("commit-ends"))
                .unwrap()
                .lines()
                .map(|v| v.parse().unwrap())
                .collect();
            let mut bytes = std::fs::read(wal(&path)).unwrap();
            let (kept, end) = match damage_kind {
                "zero" => {
                    bytes.extend([0; 4096]);
                    (3, ends[2])
                }
                "torn-checksum" => {
                    bytes.truncate(bytes.len() - 1);
                    (2, ends[1])
                }
                "torn-record" => {
                    bytes.truncate(usize::try_from(ends[1] + (ends[2] - ends[1]) / 2).unwrap());
                    (2, ends[1])
                }
                _ => {
                    bytes[usize::try_from(ends[1]).unwrap() - 1] ^= 1;
                    (1, ends[0])
                }
            };
            std::fs::write(wal(&path), &bytes).unwrap();
            let original_main = std::fs::read(&path).unwrap();
            let observed = RefCell::new(None);
            let error = Database::new_with_malformed_wal_recovery(
                &path,
                config().enable_checksums(true),
                |snapshot| {
                    *observed.borrow_mut() = snapshot.verified_prefix();
                    false
                },
            )
            .unwrap_err();
            assert!(
                matches!(error, Error::MalformedWalRecoveryFailed(_)),
                "{error}"
            );
            let prefix = observed.into_inner().expect("native verified boundary");
            assert_eq!(prefix.commit_end, end, "{damage_kind}");
            assert_eq!(prefix.transaction_count, kept);
            assert_eq!(prefix.wal_size, bytes.len() as u64);
            assert!(prefix.failed_record_start >= end);
            assert!(prefix.failed_record_start < prefix.wal_size);
            assert_eq!(prefix.zero_suffix, damage_kind == "zero");
            assert_eq!(
                prefix.stop_reason,
                match damage_kind {
                    "zero" => lbug::WalPrefixStopReason::InvalidRecordType,
                    "middle" => lbug::WalPrefixStopReason::ChecksumMismatch,
                    _ => lbug::WalPrefixStopReason::UnexpectedEof,
                }
            );
            assert_eq!(std::fs::read(&path).unwrap(), original_main);
            assert_eq!(std::fs::read(wal(&path)).unwrap(), bytes);
            let candidate = directory.path().join("candidate");
            std::fs::create_dir(&candidate).unwrap();
            let copied = candidate.join("graph.db");
            std::fs::write(&copied, &original_main).unwrap();
            std::fs::write(wal(&copied), &bytes[..usize::try_from(end).unwrap()]).unwrap();
            {
                let database = Database::new(&copied, config().enable_checksums(true)).unwrap();
                let connection = Connection::new(&database).unwrap();
                let rows: Vec<_> = connection
                    .query("MATCH (m:Memory) RETURN m.id, m.content ORDER BY m.id")
                    .unwrap()
                    .map(|row| (row[0].to_string(), row[1].to_string()))
                    .collect();
                let mut expected = vec![("durable".into(), "checkpoint sentinel".into())];
                expected.extend(
                    (1..=kept).map(|n| (format!("recent{n}"), format!("committed value {n}"))),
                );
                assert_eq!(rows, expected, "{damage_kind}");
                connection
                    .query("CREATE (:Memory {id:'after-recovery', content:'strict new commit'})")
                    .unwrap();
                connection.query("CHECKPOINT").unwrap();
            }
            let reopened =
                Database::new(&copied, config().enable_checksums(true).read_only(true)).unwrap();
            let mut expected = vec!["after-recovery".to_owned(), "durable".to_owned()];
            expected.extend((1..=kept).map(|n| format!("recent{n}")));
            assert_eq!(memory_ids(&reopened), expected);
            assert_eq!(std::fs::read(&path).unwrap(), original_main);
            assert_eq!(std::fs::read(wal(&path)).unwrap(), bytes);
        }
    }

    #[test]
    fn typed_prefix_action_replays_only_the_exact_admitted_bytes() {
        for substitute in [false, true] {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("graph.db");
            child(&path, "prefix-checksummed");
            let mut bytes = std::fs::read(wal(&path)).unwrap();
            bytes.extend([0; 4096]);
            std::fs::write(wal(&path), &bytes).unwrap();
            let original_main = std::fs::read(&path).unwrap();
            let substitute_bytes = if substitute {
                let alternate = directory.path().join("alternate");
                std::fs::create_dir(&alternate).unwrap();
                let alternate = alternate.join("graph.db");
                std::fs::write(&alternate, &original_main).unwrap();
                {
                    let database =
                        Database::new(&alternate, config().enable_checksums(true)).unwrap();
                    let connection = Connection::new(&database).unwrap();
                    connection
                        .query("CALL force_checkpoint_on_close=false")
                        .unwrap();
                    for n in 1..=3 {
                        connection.query(&format!("CREATE (:Memory {{id:'recent{n}', content:'different value {n}'}})")).unwrap();
                    }
                }
                Some(std::fs::read(wal(&alternate)).unwrap())
            } else {
                None
            };
            let observed = RefCell::new(None);
            let result = Database::new_with_wal_recovery_actions(
                &path,
                config().enable_checksums(true),
                |_| true,
                |snapshot| {
                    let prefix = snapshot.verified_prefix().unwrap();
                    assert_eq!(prefix.transaction_count, 3);
                    assert_eq!(prefix.prefix_sha256.len(), 64);
                    let mut copied = bytes[..usize::try_from(prefix.commit_end).unwrap()].to_vec();
                    if let Some(alternate) = substitute_bytes.as_ref() {
                        assert_eq!(
                            alternate.len(),
                            copied.len(),
                            "same identity and COMMIT extent"
                        );
                        copied.clone_from(alternate);
                    }
                    std::fs::write(wal(&path), &copied).unwrap();
                    *observed.borrow_mut() = Some(prefix);
                    lbug::WalRecoveryAction::VerifiedPrefix
                },
            );
            assert!(observed.into_inner().is_some());
            if substitute {
                let error = result.expect_err("a valid same-identity substitute must never replay");
                assert!(
                    error
                        .to_string()
                        .contains("differs from the native verified prefix"),
                    "{error}"
                );
                assert_eq!(std::fs::read(&path).unwrap(), original_main);
            } else {
                let database = result.unwrap();
                assert_eq!(
                    memory_ids(&database),
                    ["durable", "recent1", "recent2", "recent3"]
                );
            }
        }
    }

    #[test]
    fn prefix_observation_never_admits_foreign_or_unchecksummed_wal() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("graph.db");
        child(&path, "prefix-checksummed");
        let other = tempfile::tempdir().unwrap();
        let foreign = other.path().join("graph.db");
        child(&foreign, "prefix-checksummed");
        let mut foreign_bytes = std::fs::read(wal(&foreign)).unwrap();
        foreign_bytes.extend([0; 4096]);
        std::fs::write(wal(&path), &foreign_bytes).unwrap();
        let called = Cell::new(false);
        let result = Database::new_with_malformed_wal_recovery(
            &path,
            config().enable_checksums(true),
            |_| {
                called.set(true);
                false
            },
        );
        assert!(result.is_err());
        assert!(!called.get());
        assert_eq!(std::fs::read(wal(&path)).unwrap(), foreign_bytes);
        let (_original, path) = seed();
        damage(&path);
        let observed = RefCell::new(None);
        let _error = Database::new_with_malformed_wal_recovery(&path, config(), |snapshot| {
            *observed.borrow_mut() = snapshot.verified_prefix();
            false
        })
        .unwrap_err();
        assert!(observed.into_inner().is_none());
    }

    #[test]
    fn callback_failures_preserve_their_phase_and_panic_cause() {
        for mode in 0..4 {
            let (_directory, path) = seed();
            damage(&path);
            let before = std::fs::read(wal(&path)).unwrap();
            let error = Database::new_with_wal_recovery_admission(
                &path,
                config(),
                |_| match mode {
                    0 => false,
                    1 => panic!("synthetic admission panic"),
                    _ => true,
                },
                |_| match mode {
                    2 => false,
                    3 => panic!("synthetic recovery panic"),
                    _ => unreachable!(),
                },
            )
            .unwrap_err();
            assert!(
                match mode {
                    0 => matches!(error, Error::WalAdmissionFailed),
                    1 => matches!(error, Error::WalAdmissionPanicked),
                    2 => matches!(error, Error::MalformedWalRecoveryFailed(_)),
                    _ => matches!(error, Error::MalformedWalRecoveryPanicked(_)),
                },
                "{error}"
            );
            assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
            if mode >= 2 {
                assert!(
                    error
                        .to_string()
                        .contains("Corrupted wal file. Read out invalid WAL record type."),
                    "original WAL cause lost: {error}"
                );
                assert!(matches!(
                    std::error::Error::source(&error)
                        .unwrap()
                        .downcast_ref::<Error>(),
                    Some(Error::MalformedWal(_))
                ));
            }
        }
    }

    #[test]
    fn missing_main_header_does_not_silently_skip_valid_wal_in_lenient_mode() {
        let (_directory, path) = seed();
        let before = std::fs::read(wal(&path)).unwrap();
        std::fs::OpenOptions::new()
            .write(true)
            .open(&path)
            .unwrap()
            .set_len(0)
            .unwrap();
        let error = Database::new(&path, config().throw_on_wal_replay_failure(false)).unwrap_err();
        assert!(!matches!(error, Error::MalformedWal(_)), "{error}");
        assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
    }

    #[test]
    fn callback_cannot_report_success_with_active_wal_and_preserves_original_cause() {
        let (_directory, path) = seed();
        damage(&path);
        let before = std::fs::read(wal(&path)).unwrap();
        let error =
            Database::new_with_malformed_wal_recovery(&path, config(), |_| true).unwrap_err();
        assert!(
            !matches!(error, Error::MalformedWal(_)),
            "post-callback failure is not new isolation authority: {error}"
        );
        assert!(error.to_string().contains("active WAL present"), "{error}");
        assert!(
            error
                .to_string()
                .contains("Corrupted wal file. Read out invalid WAL record type."),
            "original cause lost: {error}"
        );
        assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
    }

    #[test]
    fn lenient_foreign_wal_still_refuses_before_truncation() {
        for read_only in [false, true] {
            let (_directory, path) = seed();
            let (_other, foreign) = seed();
            let main_before = std::fs::read(&path).unwrap();
            let foreign_wal = std::fs::read(wal(&foreign)).unwrap();
            std::fs::write(wal(&path), &foreign_wal).unwrap();
            let error = Database::new(
                &path,
                config()
                    .throw_on_wal_replay_failure(false)
                    .read_only(read_only),
            )
            .unwrap_err();
            assert!(!matches!(error, Error::MalformedWal(_)), "{error}");
            assert_eq!(std::fs::read(&path).unwrap(), main_before);
            assert_eq!(std::fs::read(wal(&path)).unwrap(), foreign_wal);
        }
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
        // Keep the operator recovery classifier's pinned wording while native
        // admission continues to use typed causes independently of this text.
        assert!(
            error
                .to_string()
                .contains("Corrupted wal file. Read out invalid WAL record type."),
            "legacy operator recovery wording lost: {error}"
        );
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
        assert!(matches!(error, Error::MalformedWalRecoveryFailed(_)));
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
        let error = Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            calls.set(calls.get() + 1);
            false
        })
        .unwrap_err();
        assert!(!matches!(error, Error::MalformedWal(_)), "{error}");
        assert!(
            error.to_string().contains("original WAL failure:")
                && error
                    .to_string()
                    .contains("Corrupted wal file. Read out invalid WAL record type."),
            "checkpoint failure lost the original WAL cause: {error}"
        );
        assert_eq!(calls.get(), 0);
        assert_eq!(std::fs::read(&path).unwrap(), main);
        assert_eq!(std::fs::read(wal(&path)).unwrap(), before);
    }
}
