//! Statement observation for hosts that mirror committed writes elsewhere.
//!
//! A host registers one [`StatementObserver`] on a [`Database`]. After any
//! statement run through [`Connection::query`] or [`Connection::execute`]
//! succeeds, the observer is called on the same connection, before the call
//! returns. Inside an explicit transaction the observer can therefore record
//! the statement in that same transaction. An observer error is returned to
//! the caller as the statement's error, so a transaction caller rolls back.
//!
//! Statements the observer itself runs on the connection are not observed
//! again.

use std::cell::Cell;
use std::sync::Arc;

use crate::connection::Connection;
use crate::error::Error;
use crate::value::Value;

/// Receives every successfully executed statement.
pub trait StatementObserver: Send + Sync {
    /// `statement` is the query text; `params` are the bound parameters of a
    /// prepared statement (empty for [`Connection::query`]).
    fn statement_succeeded(
        &self,
        connection: &Connection<'_>,
        statement: &str,
        params: &[(String, Value)],
    ) -> Result<(), Error>;
}

/// The observer slot a [`Database`] holds.
pub(crate) type ObserverSlot = std::sync::RwLock<Option<Arc<dyn StatementObserver>>>;

thread_local! {
    static OBSERVING: Cell<bool> = const { Cell::new(false) };
}

/// Run `observer` unless this thread is already inside an observer call.
pub(crate) fn notify(
    observer: &dyn StatementObserver,
    connection: &Connection<'_>,
    statement: &str,
    params: &[(String, Value)],
) -> Result<(), Error> {
    if OBSERVING.with(Cell::get) {
        return Ok(());
    }
    OBSERVING.with(|flag| flag.set(true));
    struct Reset;
    impl Drop for Reset {
        fn drop(&mut self) {
            OBSERVING.with(|flag| flag.set(false));
        }
    }
    let _reset = Reset;
    observer.statement_succeeded(connection, statement, params)
}

/// Whether the current thread is inside an observer call (its own statements
/// are not observed).
pub(crate) fn observing() -> bool {
    OBSERVING.with(Cell::get)
}
