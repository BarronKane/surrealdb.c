//! Live query notification streams.
//!
//! TODO(upstream): a killed live query never ends its stream on the embedded
//! path, so `SR_CLOSED` is effectively unreachable there.
//!
//! The cause is in surrealdb-core, not here, and not in the Rust SDK either.
//! `KILL` builds its terminal notification with the session id hardcoded to
//! `None` (surrealdb-core 3.2.4, `src/expr/statements/kill.rs`, the
//! `PublicNotification::new(lid.into(), None, PublicAction::Killed, ..)` call).
//! The SDK's local router then drops exactly that shape on the floor before it
//! can be routed:
//!
//! ```text
//! // surrealdb 3.2.4, src/engine/local/native.rs
//! let Some(session_id) = notification.session.map(|x| x.into_inner()) else {
//!     continue                       // <- the Killed notification dies here
//! };
//! ```
//!
//! Every layer below that is already correct and needs no change:
//!
//! - the router forwards all actions to the per-live-query sender, Killed
//!   included -- it does not filter,
//! - `Stream<Value>::poll_next` maps `Action::Killed` to `Poll::Ready(None)`,
//!   i.e. end of stream (surrealdb 3.2.4, `src/method/live.rs`),
//! - and this module maps that `None` to `SR_CLOSED`.
//!
//! So the whole chain works the moment that `None` becomes `Some(session_id)`.
//! Nothing here can substitute for it: the fix is one field, three layers up.
//!
//! `RpcStream` is unaffected because it reads the datastore's broker channel
//! directly and never passes through the session gate, which is why a KILLED
//! notification is observable there and nowhere else.
//!
//! Until it is fixed: every wait on `Stream` is bounded, so a reader always
//! gets control back even though it cannot be told the query is dead. See
//! `sr_stream_next_timeout`.

use std::ffi::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::time::{Duration, Instant};

use async_channel::Receiver;
use futures::StreamExt;
use surrealdb::method::Stream as sdbStream;
use surrealdb::types::{Value as sdbValue, Notification as PublicNotification};
use tokio::runtime::Handle;

use crate::SR_ERROR;
use crate::{notification::Notification, SR_CLOSED, SR_NONE};

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
/// to close. There is deliberately no unbounded read on this type; see the
/// note on `sr_stream_next_timeout`.
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
    // TODO(upstream): restore `sr_stream_next` once a killed live query ends
    // its stream on the embedded path -- see the module TODO above for the
    // one-field fix in core that this waits on.
    //
    // Kept verbatim rather than deleted, because there is nothing wrong with
    // this code: it is correct the moment `SR_CLOSED` becomes reachable. It is
    // disabled only because an unbounded wait here currently has no exit, and a
    // blocking read whose only outcome can be "park until the process dies" is
    // worse than no blocking read at all.
    //
    // To bring it back: uncomment, drop the `timeout_ms < 0` rejection in
    // `next_timeout` below so a negative bound delegates here again, restore the
    // `sr_stream_next` entry in c_test/src/tests/api_bridge_tests.c, and delete
    // `TEST(Stream, NegativeTimeoutIsRejected)`.
    //
    // /// Get the next notification, blocking until one arrives
    // ///
    // /// Returns 1 and writes to `notification_ptr` when a notification is received,
    // /// SR_CLOSED when the stream has ended, and SR_ERROR on a stream error.
    // /// It never returns SR_NONE: a blocking call has nothing to report until it
    // /// has something.
    // ///
    // /// # Blocking and shutdown
    // ///
    // /// This call blocks until a notification is available. It is intended to be
    // /// driven from a dedicated thread rather than a latency-sensitive one; see
    // /// `sr_stream_next_timeout` for a bounded or non-blocking wait.
    // ///
    // /// A stream borrows the runtime owned by the connection it was opened on, so
    // /// teardown is ordered: call `sr_stream_kill` first, then
    // /// `sr_surreal_disconnect`. Disconnecting first drops that runtime out from
    // /// under the stream, and the kill then runs against a runtime that is already
    // /// shut down.
    // ///
    // /// There is no way to release a reader already parked in this call from another
    // /// thread; `sr_stream_kill` frees the very stream that reader is borrowing. Call
    // /// this only when an event is expected, and prefer `sr_stream_next_timeout`
    // /// for a reader that has to be able to give up.
    // #[export_name = "sr_stream_next"]
    // pub extern "C" fn next(&mut self, notification_ptr: *mut Notification) -> c_int {
    //     guard(|| match self.rt.block_on(self.inner.next()) {
    //         Some(Ok(n)) => Self::deliver(n, notification_ptr),
    //         Some(Err(_)) => SR_ERROR,
    //         None => SR_CLOSED,
    //     })
    // }

    /// Get the next notification, waiting no longer than `timeout_ms`
    ///
    /// Returns 1 and writes to `notification_ptr` when a notification is
    /// received, SR_NONE when the wait expired with the stream still open,
    /// SR_CLOSED when the stream has ended, and SR_ERROR on a stream error or
    /// a negative `timeout_ms`.
    ///
    /// `timeout_ms` is a bound in milliseconds and zero polls once and returns
    /// immediately. Unlike `poll(2)` a negative value is **not** "wait
    /// forever" -- it is rejected with SR_ERROR. See below.
    ///
    /// # There is no unbounded wait on this type, on purpose
    ///
    /// A live query that is killed cannot be observed to end here (see the TODO
    /// on this module). A reader parked with no deadline on such a stream has no
    /// exit: nothing further arrives, SR_CLOSED never comes, and
    /// `sr_stream_kill` frees the very stream that reader is borrowing, so
    /// another thread cannot release it either. The process has to die.
    ///
    /// Rather than document that as a caveat and let callers walk into it, the
    /// unbounded read is not offered: `sr_stream_next` is disabled in 0.3.0 --
    /// commented out in place, not deleted, since it becomes correct again the
    /// moment the upstream fix lands -- and a negative bound is an error rather
    /// than a synonym for it. Every wait on this type therefore terminates.
    ///
    /// A long bound is cheap: the wait is a real timer, not a poll loop, so
    /// `sr_stream_next_timeout(s, &n, 3600000)` costs what blocking for an hour
    /// would have cost, and still returns.
    ///
    /// `sr_rpc_stream_next` keeps its unbounded form because the RPC path does
    /// not have this defect: freeing the context ends the stream and releases a
    /// parked reader with SR_CLOSED, which is covered by a test.
    ///
    /// # SR_NONE is not SR_CLOSED
    ///
    /// SR_NONE means nothing has arrived yet and the stream is still live, so
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
            // Not "wait forever": that state is unrecoverable on this path, so
            // asking for it is a caller error rather than a supported mode.
            return SR_ERROR;
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
            Err(_elapsed) => SR_NONE,
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

    /// Kill and free a stream
    ///
    /// Closes the stream and releases all associated resources.
    /// The stream must not be used after calling this function.
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
    /// Returns the payload length and writes to `res_ptr` on success, SR_NONE
    /// when the wait expired with the channel still open, SR_CLOSED when the
    /// sending half is gone, and SR_ERROR if the payload could not be encoded.
    ///
    /// `timeout_ms` follows `poll(2)`: negative waits indefinitely and is exactly
    /// `sr_rpc_stream_next`, zero polls once and returns immediately, and a
    /// positive value waits up to that many milliseconds.
    ///
    /// SR_NONE means call again; SR_CLOSED means stop. An expired wait takes
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
                return SR_NONE;
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
