use lbug::{Database, SystemConfig};
use std::io::{BufRead, BufReader, Read, Write};
use std::process::{Command, Stdio};

fn config() -> SystemConfig {
    SystemConfig::default()
        .buffer_pool_size(16 * 1024 * 1024)
        .max_db_size(256 * 1024 * 1024)
}

#[test]
fn holder() {
    let Some(path) = std::env::var_os("LBUG_TEST_SIDECARS_PATH") else {
        return;
    };
    let db = Database::new(std::path::Path::new(&path), config()).unwrap();
    println!("LOCK_HELD");
    std::io::stdout().flush().unwrap();
    let mut byte = [0];
    let _ = std::io::stdin().read_exact(&mut byte);
    drop(db);
}

#[test]
fn busy_normal_open_preserves_wal_and_spill() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("graph.db");
    drop(Database::new(&path, config()).unwrap());
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "holder", "--nocapture"])
        .env("LBUG_TEST_SIDECARS_PATH", &path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    let mut line = String::new();
    loop {
        assert!(output.read_line(&mut line).unwrap() > 0);
        if line.trim() == "LOCK_HELD" {
            break;
        }
        line.clear();
    }
    let wal = dir.path().join("graph.db.wal");
    let spill = dir.path().join("graph.db.tmp");
    std::fs::write(&wal, b"").unwrap();
    std::fs::write(&spill, b"active spill must survive a blocked open").unwrap();
    let before = [&path, &wal, &spill].map(|p| std::fs::read(p).unwrap());
    let result = Database::new(&path, config());
    let after = [&path, &wal, &spill].map(std::fs::read);
    drop(child.stdin.take());
    assert!(child.wait().unwrap().success());
    assert!(result
        .unwrap_err()
        .to_string()
        .contains("Could not set lock on file : "));
    for (index, bytes) in after.into_iter().enumerate() {
        assert_eq!(
            bytes.unwrap(),
            before[index],
            "artifact {index} changed during Busy"
        );
    }
}
