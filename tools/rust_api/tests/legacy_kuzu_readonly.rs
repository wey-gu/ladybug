//! Genuine Kuzu 0.11/storage-39 writer fixtures; never a rewritten LBUG header.
use lbug::{Connection, Database, Error, LogicalType, SystemConfig, Value};
use std::cell::Cell;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

const CLEAN: &[u8] = include_bytes!("testdata/kuzu39/clean.db");
const MAIN: &[u8] = include_bytes!("testdata/kuzu39/wal.db");
const WAL: &[u8] = include_bytes!("testdata/kuzu39/wal.db.wal");

fn config() -> SystemConfig {
    SystemConfig::default()
        .buffer_pool_size(128 * 1024 * 1024)
        .max_num_threads(2)
        .max_db_size(1024 * 1024 * 1024)
        .read_only(true)
        .allow_legacy_kuzu_read_only(true)
}

fn fixture(with_wal: bool) -> (tempfile::TempDir, PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("旧库-🌍.db");
    std::fs::write(&path, if with_wal { MAIN } else { CLEAN }).unwrap();
    if with_wal {
        std::fs::write(sidecar(&path, ".wal"), WAL).unwrap();
    }
    (dir, path)
}

fn sidecar(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_owned();
    value.push(suffix);
    value.into()
}

fn bytes(dir: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    std::fs::read_dir(dir)
        .unwrap()
        .map(|entry| {
            let path = entry.unwrap().path();
            (
                path.file_name().unwrap().into(),
                std::fs::read(path).unwrap(),
            )
        })
        .collect()
}

fn verify_rows(database: &Database, with_wal: bool) {
    let connection = Connection::new(database).unwrap();
    let rows: Vec<_> = connection.query("MATCH (n:Item) RETURN n.id,n.text,n.tags,n.number,CAST(n.day AS STRING),CAST(n.moment AS STRING) ORDER BY n.id").unwrap().collect();
    assert_eq!(rows.len(), if with_wal { 261 } else { 260 });
    for (i, row) in rows.iter().take(260).enumerate() {
        assert_eq!(row[0], Value::String(format!("item-{i:04}")));
        assert_eq!(
            row[1],
            Value::String(if i == 259 {
                "旧库真实写入 🌍".repeat(1100)
            } else {
                format!("body-{i}")
            })
        );
        assert_eq!(
            row[2],
            Value::List(
                LogicalType::String,
                vec![
                    Value::String("中文".into()),
                    Value::String(String::new()),
                    Value::String("e\u{301}".into())
                ]
            )
        );
        assert_eq!(row[3], Value::Int64(i.try_into().unwrap()));
        assert_eq!(row[4], Value::String("2025-07-14".into()));
        assert_eq!(row[5], Value::String("2025-07-14 12:13:14.123456".into()));
        let exact: Vec<_> = connection
            .query(&format!(
                "MATCH (n:Item {{id:'item-{i:04}'}}) RETURN n.number"
            ))
            .unwrap()
            .collect();
        assert_eq!(exact, vec![vec![Value::Int64(i.try_into().unwrap())]]);
    }
    if with_wal {
        assert_eq!(rows[260][0], Value::String("wal-only".into()));
        assert_eq!(rows[260][1], Value::String("committed WAL record".into()));
        assert_eq!(rows[260][3], Value::Int64(999));
    }
    let edges: Vec<_> = connection.query("MATCH (s:Item)-[r:Link]->(t:Item) RETURN s.id,t.id,r.kind,r.raw_context,r.extraction_metadata ORDER BY s.id,t.id,r.raw_context").unwrap().collect();
    assert_eq!(edges.len(), if with_wal { 519 } else { 518 });
    let mut occurrences = BTreeMap::new();
    for row in &edges {
        if row[2] == Value::String("wal-edge".into()) {
            assert_eq!(row[0], Value::String("item-0259".into()));
            assert_eq!(row[1], Value::String("wal-only".into()));
            assert_eq!(row[3], Value::String("WAL context".into()));
            continue;
        }
        let Value::String(source) = &row[0] else {
            panic!("source key")
        };
        let i: usize = source.strip_prefix("item-").unwrap().parse().unwrap();
        assert_eq!(row[1], Value::String(format!("item-{:04}", i + 1)));
        assert_eq!(row[2], Value::String("parallel".into()));
        let null = matches!(row[3], Value::Null(_));
        if null {
            assert_eq!(row[4], Value::String(String::new()));
        } else {
            assert_eq!(row[3], Value::String("上下文".into()));
            assert_eq!(row[4], Value::String("opaque:not JSON".into()));
        }
        *occurrences.entry((i, null)).or_insert(0) += 1;
    }
    assert_eq!(occurrences.len(), 518);
    assert!(occurrences.values().all(|count| *count == 1));
    let reverse: Vec<_> = connection.query("MATCH (t:Item)<-[r:Link]-(s:Item) RETURN s.id,t.id,r.kind,r.raw_context,r.extraction_metadata ORDER BY s.id,t.id,r.raw_context").unwrap().collect();
    assert_eq!(reverse, edges);
    assert!(connection.query("CREATE (:Item {id:'forbidden'})").is_err());
}

#[test]
fn genuine_old_checkpoint_and_committed_wal_are_readonly_and_opt_in() {
    for with_wal in [false, true] {
        let (dir, path) = fixture(with_wal);
        let before = bytes(dir.path());
        for read_only in [false, true] {
            let error = Database::new(
                &path,
                config()
                    .allow_legacy_kuzu_read_only(false)
                    .read_only(read_only),
            )
            .unwrap_err();
            assert!(error.to_string().contains("not a valid Lbug"), "{error}");
            assert_eq!(bytes(dir.path()), before);
        }
        for _ in 0..2 {
            let db = Database::new(&path, config()).unwrap();
            verify_rows(&db, with_wal);
            drop(db);
            assert_eq!(bytes(dir.path()), before);
        }
    }
}

#[test]
fn unsafe_configurations_and_recovery_callbacks_are_refused_before_mutation() {
    let (dir, path) = fixture(true);
    let before = bytes(dir.path());
    for cfg in [
        config().read_only(false),
        config().throw_on_wal_replay_failure(false),
        config().enable_checksums(false),
    ] {
        assert!(Database::new(&path, cfg)
            .unwrap_err()
            .to_string()
            .contains("requires strict read-only"));
        assert_eq!(bytes(dir.path()), before);
    }
    let called = Cell::new(false);
    assert!(Database::new_with_recovery(&path, config(), || {
        called.set(true);
        true
    })
    .is_err());
    assert!(
        Database::new_with_malformed_wal_recovery(&path, config(), |_| {
            called.set(true);
            true
        })
        .is_err()
    );
    assert!(!called.get());
    assert_eq!(bytes(dir.path()), before);
}

#[test]
fn unknown_headers_foreign_or_malformed_wal_remain_errors() {
    for mode in [
        "magic",
        "old-version",
        "future-version",
        "foreign-wal",
        "truncated-wal",
        "checksum-wal",
    ] {
        let (dir, path) = fixture(matches!(mode, "truncated-wal" | "checksum-wal"));
        if mode == "foreign-wal" {
            // CLEAN is an independently created old database, with its own UUID.
            std::fs::write(sidecar(&path, ".wal"), WAL).unwrap();
        } else if mode == "truncated-wal" {
            std::fs::write(sidecar(&path, ".wal"), &WAL[..WAL.len() - 7]).unwrap();
        } else if mode == "checksum-wal" {
            let mut wal = WAL.to_vec();
            let position = wal
                .windows(b"committed WAL record".len())
                .position(|window| window == b"committed WAL record")
                .expect("fixture WAL contains the committed payload");
            wal[position] ^= 1;
            std::fs::write(sidecar(&path, ".wal"), wal).unwrap();
        } else {
            let mut input = CLEAN.to_vec();
            if mode == "magic" {
                input[..4].copy_from_slice(b"????");
            } else {
                input[4..12].copy_from_slice(
                    &(if mode == "old-version" {
                        38_u64
                    } else {
                        40_u64
                    })
                    .to_le_bytes(),
                );
            }
            std::fs::write(&path, input).unwrap();
        }
        let before = bytes(dir.path());
        let error = Database::new(&path, config()).unwrap_err();
        if mode == "checksum-wal" {
            assert!(matches!(&error, Error::MalformedWal(_)), "{error}");
            assert!(
                error.to_string().contains("Checksum verification failed"),
                "{error}"
            );
        }
        if mode == "foreign-wal" {
            assert!(!matches!(&error, Error::MalformedWal(_)), "{error}");
            assert!(
                error
                    .to_string()
                    .contains("does not match the current database"),
                "{error}"
            );
        }
        assert_eq!(bytes(dir.path()), before);
    }
}

#[test]
fn ordinary_current_writer_and_reader_keep_their_default_contract() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("current.db");
    let normal = config().allow_legacy_kuzu_read_only(false).read_only(false);
    let db = Database::new(&path, normal.clone()).unwrap();
    Connection::new(&db)
        .unwrap()
        .query("CREATE NODE TABLE Item(id STRING PRIMARY KEY)")
        .unwrap();
    Connection::new(&db)
        .unwrap()
        .query("CREATE (:Item {id:'current'})")
        .unwrap();
    drop(db);
    let db = Database::new(&path, normal.read_only(true)).unwrap();
    let rows: Vec<_> = Connection::new(&db)
        .unwrap()
        .query("MATCH (n:Item) RETURN n.id")
        .unwrap()
        .collect();
    assert_eq!(rows, vec![vec![Value::String("current".into())]]);
}
