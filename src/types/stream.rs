//! Live query notification streams.
//!
//! # Anchor
//!
//! This crate builds against the `homebrew` branch, which carries live-query
//! fixes that are not in a published release. Without them a killed live query
//! never ended its stream: the terminal `Killed` notification carried no
//! session id and the SDK's local router dropped anything that named no
//! session, so `SR_CLOSED` was unreachable and a reader parked in
//! `sr_stream_next` could wait forever. `REMOVE TABLE` stranded a stream the
//! same way, without anyone asking for it.
//!
//! - **surrealdb/surrealdb#7520** carries the owning session id on `Killed`
//!   notifications, taken from the subscription rather than from whoever ran the
//!   `KILL`, so root killing another user's live query still notifies the right
//!   stream. This is what makes `SR_CLOSED` reachable, and it is covered by
//!   `TEST(Stream, KillEndsTheStream)` and its `REMOVE TABLE` twin.
//! - **surrealdb/surrealdb#7521** fixes the WebSocket client rejecting `KILLED`
//!   when decoding it -- the same symptom for remote endpoints, an unrelated
//!   cause, and the reason `Action::Error` was silently undeliverable there too.
//!   This crate's `sr_action` already carries every variant, so nothing here
//!   changed for it. No WebSocket fixture exists in this suite, so it is not
//!   covered by a test on our side.
//!
//! Both are open upstream, which is why the dependency is pinned to a fork. See
//! the anchor note in `Cargo.toml`; when they are released, the dependency goes
//! back to crates.io and nothing here needs to change.
//!
//! # TODO(upstream, unfiled): the kill hook still never fires
//!
//! `KILL` resolves to `NONE` rather than to its own query id, so core's
//! `QueryType::Kill` dispatch never matches and `RpcProtocol::handle_kill` is
//! never called -- for the `kill` RPC method as well as for a `KILL` written as
//! query text. Neither fix above touches it. See the note on
//! `SurrealRpcInner::handle_kill` in `rpc.rs` and the bandaid in `run_rpc`.
//!
//! - **surrealdb/surrealdb (teardown)** -- `Stream::drop` built `KILL {id}` from
//!   a bare UUID, which the parser rejects, and ran it under a blank session
//!   with no namespace. So freeing a stream left its subscription registered,
//!   on every platform, for every live query -- silently, since the kill is
//!   spawned detached and its result discarded. `REMOVE DATABASE` and
//!   `REMOVE NAMESPACE` destroyed subscriptions without announcing them at all.
//!   Fixed on the anchor; `TEST(Stream, EitherTeardownRouteRetiresTheQuery)`
//!   covers it, and both `sr_stream_kill` and `sr_kill` now stand alone.

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
/// Use `sr_stream_next` or `sr_stream_next_timeout` to receive notifications.
/// Freeing the reader takes `sr_stream_kill`; retiring the live query itself
/// takes `sr_kill`, and they are separate steps.
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
    /// # Blocking and shutdown
    ///
    /// This is the natural call for a dedicated reader thread: it parks until
    /// there is something to report, and a killed live query does end the
    /// stream, so it does return. That last part is only true on the anchored
    /// dependency -- see the module note.
    ///
    /// What it still cannot do is wake for anything other than the stream. A
    /// reader that also has to notice a shutdown flag wants
    /// `sr_stream_next_timeout`, because `sr_stream_kill` frees the very stream
    /// this reader is borrowing and so cannot be used to release it from another
    /// thread.
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
    /// # When to prefer it over `sr_stream_next`
    ///
    /// When the reader has to stay responsive to something other than the
    /// stream -- a shutdown flag, a frame budget, a cancellation token. The
    /// blocking call returns on a kill, but only on a kill.
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

    /// Kill and free a stream
    ///
    /// Retires the live query in the datastore and releases the reader. The
    /// stream must not be used after calling this function.
    ///
    /// This is the teardown to reach for whenever a stream exists, because it
    /// needs no live query id -- `sr_select_live` does not hand one back.
    /// `sr_kill` covers the other case, a subscription you have an id for but no
    /// stream.
    ///
    /// Until the anchor picked up "Retire live queries on every teardown path"
    /// this freed the reader and left the subscription registered, so both calls
    /// were required. See the module note.
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
