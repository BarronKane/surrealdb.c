//! Transactions that span calls.
//!
//! `sr_begin` yields an opaque handle. Statements run on it through
//! `sr_tx_query` are scoped together: invisible outside until `sr_commit`, and
//! discarded entirely by `sr_cancel`.
//!
//! The handle is what makes this more than `BEGIN; ...; COMMIT;` sent as one
//! query. That form works and rolls back correctly, but the whole transaction
//! has to be known before it is sent. A handle lets the caller run its own
//! code between statements -- read a balance, ask the engine something, decide
//! to abort -- and still have them commit or roll back as one.
//!
//! Underneath, `Surreal::begin` returns a transaction carrying a uuid; every
//! call made on it threads that id through, and the datastore runs those
//! statements inside the one transaction it names.

use std::ffi::{c_char, c_int, CStr};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use surrealdb::engine::any::Any;
use surrealdb::method::Transaction as sdbTransaction;
use surrealdb::types::{Object as sdbObject, Value as sdbValue};
use tokio::runtime::Runtime;

use crate::array::{Array, ArrayGen, MakeArray};
use crate::result::ArrayResult;
use crate::string::string_t;
use crate::value::Value;
use crate::{write_error, Surreal, SR_ERROR, SR_FATAL};

/// An open transaction.
///
/// Statements run through `sr_tx_query` are scoped to it and are not visible
/// outside until `sr_commit`. `sr_commit` and `sr_cancel` both consume the
/// handle, and one of them must be called: a handle that is simply forgotten
/// leaves the transaction open in the datastore until it times out.
pub struct Transaction {
    /// `Option` because `Transaction::commit` and `::cancel` take the SDK value
    /// by move, and this is reached through a raw pointer.
    inner: Option<sdbTransaction<Any>>,
    /// Shared with the connection, as a session fork is. A transaction is not
    /// a second connection: no engine, no runtime, no threads.
    rt: Arc<Runtime>,
    ps: Arc<AtomicBool>,
}

/// Poison check and panic guard, mirroring `with_surreal_async` on the
/// connection. A transaction shares its connection's poison flag, so an engine
/// that died under one is reported through the other.
fn with_tx_async<F>(tx: &Transaction, err_ptr: *mut string_t, fun: F) -> c_int
where
    F: std::future::Future<Output = Result<c_int, string_t>>,
{
    if tx.ps.load(Ordering::Acquire) {
        write_error(
            err_ptr,
            "connection is poisoned: an earlier operation returned SR_FATAL",
        );
        return SR_FATAL;
    }
    let _guard = tx.rt.enter();
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| tx.rt.block_on(fun))) {
        Ok(Ok(n)) => n,
        Ok(Err(e)) => {
            write_error(err_ptr, e);
            SR_ERROR
        }
        Err(_) => {
            tx.ps.store(true, Ordering::Release);
            write_error(err_ptr, "a panic escaped the database runtime");
            SR_FATAL
        }
    }
}

impl Transaction {
    /// Begin a transaction.
    ///
    /// Writes an open transaction to `tx_ptr`. Statements run on it through
    /// `sr_tx_query` are scoped together: none of them are visible to anything
    /// else until `sr_commit`, and `sr_cancel` discards the lot.
    ///
    /// ```c
    /// sr_transaction_t *tx = NULL;
    /// if (sr_begin(db, &err, &tx) < 0) { return 1; }
    ///
    /// sr_tx_query(tx, &err, &res, "UPDATE account:a SET bal -= 10;", NULL);
    /// sr_tx_query(tx, &err, &res, "UPDATE account:b SET bal += 10;", NULL);
    ///
    /// sr_commit(tx, &err);   // or sr_cancel(tx, &err) -- either frees `tx`
    /// ```
    ///
    /// Unlike writing `BEGIN; ...; COMMIT;` as one query, the caller decides
    /// what happens between statements -- which is the point of a transaction
    /// handle and the reason the single-query form is not a substitute.
    ///
    /// # The handle must be committed or cancelled
    ///
    /// Both consume it. Losing the pointer without calling either leaves the
    /// transaction open in the datastore until it times out, holding whatever
    /// it has locked.
    ///
    /// # It runs on its own session
    ///
    /// The transaction takes a forked session, copied from this connection's
    /// at the moment of the call, so it inherits the namespace, database and
    /// authentication in force then. Later `sr_use_ns` or `sr_use_db` on the
    /// connection does not move a transaction that is already open.
    ///
    /// # Safety
    ///
    /// - `err_ptr` must be a valid pointer or null
    /// - `tx_ptr` must be a valid pointer to receive the transaction
    #[export_name = "sr_begin"]
    pub extern "C" fn begin(
        db: &Surreal,
        err_ptr: *mut string_t,
        tx_ptr: *mut *mut Transaction,
    ) -> c_int {
        if tx_ptr.is_null() {
            write_error(err_ptr, "tx_ptr is null");
            return SR_ERROR;
        }
        if db.ps.load(Ordering::Acquire) {
            write_error(
                err_ptr,
                "connection is poisoned: an earlier operation returned SR_FATAL",
            );
            return SR_FATAL;
        }

        let _guard = db.rt.enter();
        let begun = db.rt.block_on(async {
            // `begin` takes the client by value; the clone forks a session that
            // carries this connection's namespace, database and auth across.
            db.db.clone().begin().await
        });

        match begun {
            Ok(inner) => {
                let tx = Transaction {
                    inner: Some(inner),
                    rt: Arc::clone(&db.rt),
                    ps: Arc::clone(&db.ps),
                };
                unsafe { tx_ptr.write(Box::into_raw(Box::new(tx))) };
                1
            }
            Err(e) => {
                write_error(err_ptr, &e.to_string());
                SR_ERROR
            }
        }
    }

    /// Run a query inside a transaction.
    ///
    /// The same `sr_arr_res_t` array `sr_query` returns -- one entry per
    /// statement, each carrying either rows or its own error -- except that
    /// nothing it writes is visible outside the transaction until `sr_commit`.
    ///
    /// A statement that fails does not by itself roll the transaction back; the
    /// error is reported in that statement's slot and the caller decides
    /// whether to carry on or `sr_cancel`. That choice is the reason to hold a
    /// handle rather than send one `BEGIN; ...; COMMIT;` query.
    ///
    /// # Why there is no `sr_tx_create`, `sr_tx_select` and so on
    ///
    /// Deliberate, not an omission. The SDK's transaction does expose typed
    /// operations, and forwarding them would be straightforward -- but each one
    /// is `sr_tx_query` with the statement written for you, over bound
    /// variables. They would add entry points to the ABI without adding
    /// anything a caller cannot already do, and every one is a signature that
    /// has to be kept, documented and tested forever.
    ///
    /// The asymmetry with `sr_create` and friends on the connection is real and
    /// is the honest argument for adding them: a caller who starts there and
    /// then needs a transaction has to rewrite those calls as SQL. That is a
    /// convenience question rather than a capability one, and it is better
    /// answered a level up, where a wrapper can build the statement once and
    /// type the result properly, than by widening this ABI. Revisit if a
    /// consumer finds a case SQL cannot reach.
    ///
    /// # Safety
    ///
    /// - `err_ptr` must be a valid pointer or null
    /// - `res_ptr` must be a valid pointer to receive the results
    /// - `query` must be a valid null-terminated string
    /// - `vars` must be a valid pointer to an object, or null for none
    ///
    /// Free the results with `sr_arr_res_arr_free`.
    #[export_name = "sr_tx_query"]
    pub extern "C" fn tx_query(
        tx: &Transaction,
        err_ptr: *mut string_t,
        res_ptr: *mut *mut ArrayResult,
        query: *const c_char,
        vars: *const crate::value::Object,
    ) -> c_int {
        if res_ptr.is_null() {
            write_error(err_ptr, "res_ptr is null");
            return SR_ERROR;
        }
        if query.is_null() {
            write_error(err_ptr, "query is null");
            return SR_ERROR;
        }

        with_tx_async(tx, err_ptr, async {
            let Some(inner) = tx.inner.as_ref() else {
                return Err(string_t::from("transaction has already been completed"));
            };
            let sql = unsafe { CStr::from_ptr(query) }
                .to_str()
                .map_err(|e| string_t::from(e.to_string()))?;
            let vars: sdbObject = match vars.is_null() {
                true => sdbObject::default(),
                false => unsafe { &*vars }.clone().into(),
            };

            let mut res = inner
                .query(sql)
                .bind(vars)
                .await
                .map_err(|e| string_t::from(e.to_string()))?;
            let len = res.num_statements();

            let mut acc = Vec::with_capacity(len);
            for index in 0..len {
                let taken: Result<sdbValue, _> = res.take(index);
                acc.push(match taken {
                    Ok(sdbValue::Array(arr)) => ArrayResult::ok(arr.into()),
                    Ok(val) => {
                        let arr: Array = vec![Value::from(val)].into();
                        ArrayResult::ok(arr)
                    }
                    Err(e) => ArrayResult::err(e.to_string()),
                });
            }

            let ArrayGen { ptr, len } = acc.make_array();
            unsafe { res_ptr.write(ptr) }
            Ok(len)
        })
    }

    /// Commit a transaction and free the handle.
    ///
    /// Everything run through `sr_tx_query` becomes visible together, or not at
    /// all if this fails. The handle is consumed either way and must not be
    /// used again.
    ///
    /// # Safety
    ///
    /// - `tx` must be a valid pointer from `sr_begin`, not previously consumed
    /// - `err_ptr` must be a valid pointer or null
    #[export_name = "sr_commit"]
    pub extern "C" fn commit(tx: *mut Transaction, err_ptr: *mut string_t) -> c_int {
        Self::finish(tx, err_ptr, true)
    }

    /// Roll a transaction back and free the handle.
    ///
    /// Discards everything run through `sr_tx_query`. The handle is consumed
    /// and must not be used again. This is also how to abandon a transaction:
    /// there is no separate free.
    ///
    /// # Safety
    ///
    /// - `tx` must be a valid pointer from `sr_begin`, not previously consumed
    /// - `err_ptr` must be a valid pointer or null
    #[export_name = "sr_cancel"]
    pub extern "C" fn cancel(tx: *mut Transaction, err_ptr: *mut string_t) -> c_int {
        Self::finish(tx, err_ptr, false)
    }

    /// Shared by commit and cancel: both consume the handle, and both must free
    /// it even when the call itself fails -- otherwise a failed commit would
    /// leave the caller holding a handle that can no longer be used for
    /// anything.
    fn finish(tx: *mut Transaction, err_ptr: *mut string_t, commit: bool) -> c_int {
        if tx.is_null() {
            write_error(err_ptr, "transaction is null");
            return SR_ERROR;
        }
        let mut boxed = unsafe { Box::from_raw(tx) };

        let Some(inner) = boxed.inner.take() else {
            write_error(err_ptr, "transaction has already been completed");
            return SR_ERROR;
        };

        let rt = Arc::clone(&boxed.rt);
        let _guard = rt.enter();
        let res = rt.block_on(async {
            if commit {
                inner.commit().await
            } else {
                inner.cancel().await
            }
        });

        match res {
            Ok(_) => 1,
            Err(e) => {
                write_error(err_ptr, &e.to_string());
                SR_ERROR
            }
        }
    }
}
