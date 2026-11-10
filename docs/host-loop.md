# External event-host integration (0.16)

HostLoop connects an existing EventLoop to an event host owned by another
module. Actual consumers are iiGUI's AppKit/CFRunLoop, Win32 message loop and
Linux Wayland hosts. Core owns task admission, timing, cancellation, readiness
and execution-context contracts; the consumer owns windowing, lifecycle,
native wake handles, GPU work, input and fonts. No upper SDK or GUI framework
is imported into Core. HostLoop also works for non-GUI external event hosts.

## Wake and dispatch

```cpp
ii::runtime::EventLoop events(1024);
ii::runtime::HostLoop connection(events, [&native_gate]() noexcept {
    native_gate.signal(); // Thread-safe nonblocking notification, not dispatch.
});
auto producer=events.sender();
// The native host invokes this on its owner after consuming a native wake:
events.poll({64,std::chrono::milliseconds(4)});
// Arm the host's native one-shot timer using events.next_deadline().
```

The callback must be copyable, noexcept-invocable, thread-safe and prompt. The
constructor's constraint enforces noexcept invocation; it does not prove that
the callback never blocks. Do not poll/run, detach, destroy the loop, or wait
for a host reply inside it. Core calls it without holding its queue mutex;
inspection/submission is safe. Recursive wake requests coalesce until a host
poll acknowledges the outstanding notification. Initial attachment signals
the host so pre-attachment submissions are included.

Ready or timed submissions, direct TaskToken cancellation, actual suspension,
resumption and stop notify the same host. An untriggered `on_signal` binding or
on-demand realtime reservation stays parked until its explicit signal, and a
repeated suspend/resume with no state change does not notify. QueryPort, TaskScope, FrameLoop and direct
EventLoop producers do not need a separate GUI-specific wake wrapper. A budget
that leaves ready work is followed by the host rearming an immediate deadline.
There is no automatic frame timer.

One live connection is allowed per loop. Attach only when no dispatch is
active. Poll is pinned to the attaching thread for the connection's lifetime;
foreign/recursive dispatch throws logic_error. Blocking EventLoop::run is
unavailable while connected, including from FrameLoop::run or Thread's loop
driver. Use FrameLoop::step inside a scheduled callback, or poll from an
external native frame callback outside existing dispatch.

HostLoop::is_current_thread reports connected external-host context, using a
thread-local count rather than a global host registry. Operation::wait_for
returns would_block on that thread while the result is pending. A result
already available returns ready. This does not intercept arbitrary blocking
C++/OS calls; use Runtime/AsyncIO and then_on for results.

## Native readiness

The first explicitly registered I/O watch starts one managed Thread to wait
for channel readiness and wake the host. A host without a watch allocates no
readiness thread. Existing watches also start it when a host attaches. The
waiter belongs to the connection and is joined when it closes; between watches
it remains parked for reuse. It never invokes an I/O completion, task, query or frame callback;
all dispatch still occurs in the owner's poll. It reuses Core's native
ReadinessPoller and bounded watch storage. Prepared native leases protect
handles from reuse. Cancellation synchronizes that wait before reclaiming
the slot and callback storage.

The waiter parks on a condition variable when no unready I/O remains or the
loop is suspended. On POSIX, pending I/O waits for descriptor readiness,
cancellation, an explicit wake, or the actual deadline without recurring idle
checks. If another operation temporarily owns a channel lock, the waiter is
notified when the lock is released. Normal task posts and empty host polls do
not restart an unchanged readiness wait. Windows uses the existing explicitly
requested wake-event/readiness-probe path, not a new IOCP backend.
There is no permanent UI polling timer. This adds at most one native waiter per
connection that actually watches I/O; it is not one thread per request.
`HostLoop::io_waiter_active()` reports whether that connection owns a waiter.

HostLoopOptions::monitor_io=false disables that waiter for a host that explicitly
polls I/O itself. An I/O cancellation signal still wakes that host so it can
dispatch the cancellation; ordinary readiness remains the host's responsibility.
Threadless WASM and the browser main thread cannot start a native waiter; a
watch that requires one, or attachment with an existing watch, throws if the
waiter cannot be started. Partial host attachment is cleaned up.
An explicit tasks/manual-I/O connection or the existing BrowserLoop remains
available. Building Core for a platform does not verify its native GUI behavior.

Slow synchronous device/regular-file transfers may still block when performed;
readiness alone does not make disk I/O asynchronous. Use AsyncIO/Runtime for
such work. Waiter failures are rethrown from owner poll, or returned from close
with a preserved exception cause. No failure is converted into a success.

## Lifetime and cancellation

Create, close and destroy the connection on its attaching thread. close is
idempotent and is a barrier for in-flight wake calls, the waiter and its native
leases. It rejects inline/active-dispatch close with busy, and foreign close
with wrong_executor. Destruction on those invalid paths terminates instead of
abandoning native callbacks. Close after dispatch returns, before releasing
the native gate. Wake-capture destruction occurs outside Core locks.

Detach leaves caller-owned queued tasks and loop state intact. The caller can
attach another host or use a native loop driver afterward. EventLoop destruction
stops submissions/cancels work and detaches a surviving connection safely on
its host owner. Self/concurrent owner destruction during active dispatch remains
invalid. The connection never owns a window or native callback target.

EventLoop::Sender is a copyable weak admission endpoint with post/at/after/every,
realtime, on_signal/signal, watch/read/write and cancel. It does not retain the loop owner, expose
native handles or allow dispatch. Removed/stopped executors reject; bounded
admission and generation-safe tokens are unchanged. TaskScope::at and its
Sender::at accept TimedTaskExecutor implementations, including Core Sender and
iiGUI UiExecutor. Scope removal cancels timers through the same wake path.

Operation::then_on owns a copyable endpoint value. Use a weak Sender/UiExecutor
when the local endpoint may disappear before completion. Receiver TaskScope
still controls delivery; endpoint expiry reports stopped in
completion_submission. Existing then(Executor&) retains its borrowed-executor
lifetime requirement. Exactly one completion observer is allowed across both.

HostLoopStats counts wake requests, coalesced wake signals and actual pending-I/O
waits. These are Core notifications, not total OS events, presentation counts,
CPU utilization or hard real-time guarantees.

## Tests and package boundary

tests/host_loop.cpp checks queue bounds, reentrant inspection, wake coalescing,
direct-token cancellation, owner affinity, detach races/barriers, surviving
connections, structural timed scopes, endpoint-owned Operation delivery,
nonblocking host observation, FrameLoop phases and delayed native pipe input.
The pipe fixture acknowledges process startup, then waits for an explicit
release after I/O admission. The one-second readiness limit is measured after
that handshake, against a three-second request timeout, without including cold
loader/sanitizer startup. It requires an otherwise idle host to wake for input.
The relocated Runtime consumer runs that same contract. Independent Contracts
and Values consumers still import no GUI, thread runtime or graphics package.
iiGUI tests separately prove actual window/GPU integration on the verified host.
