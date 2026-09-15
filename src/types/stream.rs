//! Live query notification streams.
//!
//! TODO(upstream): on surrealdb 3.2.4 a killed live query never ends its stream
//! on the embedded path, so `SR_CLOSED` is unreachable there and
//! `sr_stream_next` can park forever. Two fixes are in flight; this module is
//! written for the fixed behaviour and the tests pin the broken one.
//!
//! # The defect
//!
//! `KILL` and `REMOVE TABLE` build their terminal notification with the session
//! id hardcoded to `None` (`core/src/expr/statements/kill.rs`,
//! `.../remove/table.rs`). The SDK's local router drops exactly that shape
//! before routing it:
//!
//! ```text
//! // surrealdb 3.2.4, src/engine/local/native.rs (and wasm.rs)
//! let Some(session_id) = notification.session.map(|x| x.into_inner()) else {
//!     continue                       // <- the Killed notification dies here
//! };
//! ```
//!
//! Everything below the gate is already correct: the router does not filter by
//! action, `Stream<Value>::poll_next` maps `Action::Killed` to
//! `Poll::Ready(None)`, and this module maps that `None` to `SR_CLOSED`. Only
//! the missing session id stands between them. `REMOVE TABLE` is the worse
//! case, because no caller asked for it -- an unrelated schema change orphans
//! every stream on that table.
//!
//! `RpcStream` is unaffected: it reads the datastore's broker channel directly
//! and never passes through the session gate, which is why a KILLED
//! notification is observable there and nowhere else.
//!
//! # What lands when
//!
//! - **surrealdb/surrealdb#7520** -- carries the owning session id on `Killed`
//!   notifications, taken from the subscription rather than from whoever ran the
//!   `KILL`, so root killing another user's live query still notifies the right
//!   stream. When this ships, `sr_stream_next` and `sr_stream_next_timeout`
//!   start reporting `SR_CLOSED` for a killed stream, and
//!   `TEST(Stream, KillDoesNotYetEndTheStream)` and its `REMOVE TABLE` twin
//!   start failing. **That failure is the signal to invert them**, not a
//!   regression.
//! - **surrealdb/surrealdb#7521** -- the WebSocket client's notification decoder
//!   knows only three of the five `Action` variants and rejects its own wire
//!   format for the other two, discarding the frame with no log line. Same
//!   symptom, unrelated cause, and it applies to the *remote* endpoints this
//!   library supports via `protocol-ws` rather than to the embedded path. The
//!   tripwire tests below run on `mem://`, so they pin #7520 only; the
//!   WebSocket path has no coverage here.
//!
//!   `Action::Error` is dropped by the same decoder. That is latent -- nothing
//!   emits it yet -- but this crate's `sr_action` already carries
//!   `SR_ACTION_ERROR`, so nothing here needs to change when it starts working.
//!
//! - **Unfiled** -- `KILL` resolves to `NONE` rather than to its own query id,
//!   so core's `QueryType::Kill` dispatch never matches and
//!   `RpcProtocol::handle_kill` never fires. Neither PR above touches it. See
//!   the note on `SurrealRpcInner::handle_kill` in `rpc.rs`.
//!
//! None of the three is worked around here. All are producer-side fixes
//! upstream, and a shim in this layer would be a thing to remove later.
//!
//! # Until then
//!
//! Prefer `sr_stream_next_timeout`. It cannot tell "quiet" from "dead" either,
//! but it always returns, which is the difference between a slow reader and a
//! thread that can only be retired by ending the process.

use std::ffi::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::time::{Duration, Instant};

use async_channel::Receiver;
use futures::StreamExt;
use surrealdb::method::Stream as sdbStream;
use surrealdb::types::{Value as sdbValue, Notification as PublicNotification};
use tokio::runtime::Handle;

use crate::SR_ERROR;
use crate::{notification::Notification, SR_CLOSED, SR_AGAIN};

use super::array::MakeArray;

/// Run a stream call, converting a panic into SR_ERROR.
///
/// These are `extern "C"`, so an escaping panic is a non-unwinding abort: the
/// process dies with no diagnostic and the C caller gets no chance to clean up.
/// That is a bad trade for a library embedded in someone else's application,
/// and these functions are the ones that drive a tokio runtime, which is where
/// a panic is most plausible. The panic message still reaches stderr before
/// this returns, so a library bug stays visible rather than being swallowed.
fn guard(f: impl FnOnce() -> c_int) -> c_int {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(SR_ERROR)
}

/// Stream for receiving live query notifications
///
/// May be sent across threads, but must not be aliased.
/// Use `sr_stream_next_timeout` to receive notifications and `sr_stream_kill`
/// to close. `sr_stream_next` blocks without a bound and is correct only once
/// surrealdb/surrealdb#7520 ships; see the module note.
pub struct Stream {
    inner: sdbStream<sdbValue>,
    rt: Handle,
}

impl Stream {
    pub fn new(inner: sdbStream<sdbValue>, rt: Handle) -> Stream {
        Stream { inner, rt }
    }
}

impl Stream {
    /// Get the next notification, blocking until one arrives
    ///
    /// Returns 1 and writes to `notification_ptr` when a notification is
    /// received, SR_CLOSED when the stream has ended, and SR_ERROR on a stream
    /// error. It never returns SR_AGAIN: a blocking call has nothing to report
    /// until it has something.
    ///
    /// # Not recommended on surrealdb 3.2.4
    ///
    /// This call is correct once surrealdb/surrealdb#7520 ships. Against the
    /// version currently pinned it is not, and the failure mode is the worst
    /// kind: a killed live query never reports its end, so a reader parked here
    /// waits for a notification that cannot arrive. Nothing releases it --
    /// `sr_stream_kill` frees the very stream the reader is borrowing, so
    /// another thread cannot free it either, and the process has to die.
    ///
    /// `REMOVE TABLE` puts a stream in that state too, and no caller asked for
    /// it, so "only block when you know an event is coming" is not a discipline
    /// a caller can actually keep.
    ///
    /// Use `sr_stream_next_timeout` until the dependency is bumped past the
    /// fix. See the module note for what changes when it is.
    #[export_name = "sr_stream_next"]
    pub extern "C" fn next(&mut self, notification_ptr: *mut Notification) -> c_int {
        guard(|| match self.rt.block_on(self.inner.next()) {
            Some(Ok(n)) => Self::deliver(n, notification_ptr),
            Some(Err(_)) => SR_ERROR,
            None => SR_CLOSED,
        })
    }

    /// Get the next notification, waiting no longer than `timeout_ms`
    ///
    /// Returns 1 and writes to `notification_ptr` when a notification is
    /// received, SR_AGAIN when the wait expired with the stream still open,
    /// SR_CLOSED when the stream has ended, and SR_ERROR on a stream error.
    ///
    /// `timeout_ms` follows `poll(2)`: negative waits indefinitely and is exactly
    /// `sr_stream_next`, zero polls once and returns immediately, and a positive
    /// value waits up to that many milliseconds.
    ///
    /// # This is the call to use on surrealdb 3.2.4
    ///
    /// A killed live query cannot be observed to end on the pinned version (see
    /// the module note), so a bounded wait is the only read that is guaranteed
    /// to return. It still cannot tell "nothing happening" from "this query is
    /// dead" -- that distinction needs the upstream fix -- but a caller keeps
    /// control and can check a shutdown flag, which an unbounded read on a dead
    /// stream cannot.
    ///
    /// A long bound is cheap: the wait is a real timer, not a poll loop, so
    /// `sr_stream_next_timeout(s, &n, 3600000)` costs what blocking for an hour
    /// would have cost, and still returns.
    ///
    /// # SR_AGAIN is not SR_CLOSED
    ///
    /// SR_AGAIN means nothing has arrived yet and the stream is still live, so
    /// call again. SR_CLOSED means the stream has ended and calling again is
    /// pointless. Collapsing the two turns a merely slow notification into an
    /// abandoned stream, or an ended stream into a spin.
    ///
    /// The split falls on the sign, so `r > 0` is a notification, `r == 0` is
    /// "not yet" and `r < 0` is "stop", which is the check most callers want.
    ///
    /// An expired wait consumes nothing. Notifications queue in a channel, and a
    /// poll that finds it empty leaves any later arrival in place, so polling in
    /// a loop cannot lose an event.
    ///
    /// # Teardown
    ///
    /// A stream borrows the runtime owned by the connection it was opened on, so
    /// teardown is ordered: call `sr_stream_kill` first, then
    /// `sr_surreal_disconnect`. `sr_stream_kill` is also the only way to stop the
    /// underlying live query cleanly -- `sr_kill` does not (see its own note).
    #[export_name = "sr_stream_next_timeout"]
    pub extern "C" fn next_timeout(
        &mut self,
        notification_ptr: *mut Notification,
        timeout_ms: c_int,
    ) -> c_int {
        if timeout_ms < 0 {
            return self.next(notification_ptr);
        }
        // Cloned so the future below can borrow `inner` mutably without also
        // holding a borrow of `rt`.
        let rt = self.rt.clone();
        let wait = Duration::from_millis(timeout_ms as u64);
        guard(|| {
        // The timeout is constructed inside `block_on`, not outside it: building
        // one registers with the runtime's timer driver, and doing that from a
        // plain C thread panics with "there is no reactor running".
        //
        // tokio polls the inner future before the timer, so a zero duration is
        // still one real poll rather than an unconditional expiry.
        let inner = &mut self.inner;
        match rt.block_on(async { tokio::time::timeout(wait, inner.next()).await }) {
            Ok(Some(Ok(n))) => Self::deliver(n, notification_ptr),
            Ok(Some(Err(_))) => SR_ERROR,
            Ok(None) => SR_CLOSED,
            Err(_elapsed) => SR_AGAIN,
        }
        })
    }

    /// Hand one notification to C.
    fn deliver(
        n: surrealdb::Notification<sdbValue>,
        notification_ptr: *mut Notification,
    ) -> c_int {
        let notif = Notification {
            query_id: crate::uuid::Uuid::from(n.query_id),
            action: crate::notification::Action::from(n.action),
            data: crate::value::Value::from(n.data),
        };
        unsafe { notification_ptr.write(notif) }
        1
    }

    /// Free a stream
    ///
    /// Releases the reader and its resources. The stream must not be used after
    /// calling this function.
    ///
    /// # This does not retire the live query
    ///
    /// Despite the name, the subscription stays registered in the datastore.
    /// Dropping the SDK stream does spawn a kill, but it is fire-and-forget with
    /// its result discarded, and on the pinned version the subscription is
    /// observably still listed by `INFO FOR TABLE` afterwards.
    ///
    /// Use `sr_kill` for that, and call both -- neither does the other's job:
    ///
    /// ```c
    /// sr_kill(db, &err, query_id);   // retires the subscription
    /// sr_stream_kill(stream);        // frees the local reader
    /// ```
    ///
    /// This runs on the runtime owned by the connection the stream was opened on,
    /// so it must be called before `sr_surreal_disconnect` on that connection.
    #[export_name = "sr_stream_kill"]
    pub extern "C" fn kill(stream: *mut Stream) {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let boxed = unsafe { Box::from_raw(stream) };
            let handle = boxed.rt.clone();
            handle.block_on(async { drop(boxed) });
        }));
    }
}

/// Stream for receiving RPC live query notifications
///
/// Wraps a `Receiver<PublicNotification>` from the datastore's notification channel.
/// Uses synchronous blocking receives, so no async drop is required.
pub struct RpcStream {
    rx: Receiver<PublicNotification>,
}

impl RpcStream {
    /// Create a new RpcStream from a notification receiver
    pub fn new(rx: Receiver<PublicNotification>) -> Self {
        RpcStream { rx }
    }
    /// Get the next notification, blocking until one arrives
    ///
    /// # Blocking and shutdown
    ///
    /// This call blocks until a notification is available. It is intended to be
    /// driven from a dedicated thread rather than a latency-sensitive one; see
    /// `sr_rpc_stream_next_timeout` for a bounded or non-blocking wait.
    ///
    /// To retire that thread, free the connection the stream came from. Doing so
    /// drops the sending half of the notification channel, and a reader parked
    /// inside this function returns SR_CLOSED. The stream itself stays valid and
    /// must still be freed.
    #[export_name = "sr_rpc_stream_next"]
    pub extern "C" fn next(&mut self, res_ptr: *mut *mut u8) -> c_int {
        guard(|| {
            let notification = match self.rx.recv_blocking() {
                Ok(n) => n,
                Err(_) => return SR_CLOSED,
            };
            Self::encode(notification, res_ptr)
        })
    }

    /// Get the next notification, waiting no longer than `timeout_ms`
    ///
    /// Returns the payload length and writes to `res_ptr` on success, SR_AGAIN
    /// when the wait expired with the channel still open, SR_CLOSED when the
    /// sending half is gone, and SR_ERROR if the payload could not be encoded.
    ///
    /// `timeout_ms` follows `poll(2)`: negative waits indefinitely and is exactly
    /// `sr_rpc_stream_next`, zero polls once and returns immediately, and a
    /// positive value waits up to that many milliseconds.
    ///
    /// SR_AGAIN means call again; SR_CLOSED means stop. An expired wait takes
    /// nothing off the channel, so a later notification is still delivered.
    ///
    /// Unlike `sr_stream_next_timeout` this does not touch a tokio runtime, which
    /// is deliberate: an RpcStream outlives the connection it came from, so it
    /// must not hold a handle to a runtime that may already be shut down. The
    /// wait is therefore a poll at millisecond granularity rather than a timer.
    #[export_name = "sr_rpc_stream_next_timeout"]
    pub extern "C" fn next_timeout(&mut self, res_ptr: *mut *mut u8, timeout_ms: c_int) -> c_int {
        if timeout_ms < 0 {
            return self.next(res_ptr);
        }
        let deadline = Instant::now() + Duration::from_millis(timeout_ms as u64);
        guard(|| {
        let notification = loop {
            match self.rx.try_recv() {
                Ok(n) => break n,
                // Checked every pass rather than only on expiry, so freeing the
                // connection still releases this reader promptly.
                Err(async_channel::TryRecvError::Closed) => return SR_CLOSED,
                Err(async_channel::TryRecvError::Empty) => {}
            }
            let now = Instant::now();
            if now >= deadline {
                return SR_AGAIN;
            }
            std::thread::sleep(std::cmp::min(deadline - now, Duration::from_millis(1)));
        };
        Self::encode(notification, res_ptr)
        })
    }

    /// CBOR-encode one notification into `res_ptr`. Shared by both variants.
    fn encode(notification: PublicNotification, res_ptr: *mut *mut u8) -> c_int {
        let mut obj = surrealdb::types::Object::new();
        obj.insert(
            "id".to_string(),
            sdbValue::Uuid(notification.id),
        );
        obj.insert(
            "action".to_string(),
            sdbValue::String(format!("{}", notification.action)),
        );
        obj.insert("result".to_string(), notification.result);

        let cbor_val = crate::rpc::value_to_cbor(&sdbValue::Object(obj));
        let mut res = Vec::new();
        if ciborium::into_writer(&cbor_val, &mut res).is_err() {
            return SR_ERROR;
        }
        let out = res.make_array();

        unsafe { res_ptr.write(out.ptr) }

        out.len
    }

    /// Free an RpcStream
    #[export_name = "sr_rpc_stream_free"]
    pub extern "C" fn free(stream: *mut RpcStream) {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            if !stream.is_null() {
                let _ = unsafe { Box::from_raw(stream) };
            }
        }));
    }
}
