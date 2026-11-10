# Time-driven runtime (0.7.0)

Core/Time/Time.h defines monotonic, saturating epoch arithmetic and phase-preserving period positions independently of Runtime. Deadline uses this lower layer. EventLoop's Clock alias preserves the steady-clock epoch.

## Scheduling

EventLoop::realtime reserves a recurring slot with period, relative deadline, runtime per period, callback quantum and optional first release. Durations satisfy 0 < quantum <= runtime <= deadline <= period. Missing start means immediate release. Ready jobs use earliest absolute deadline first; equal deadlines rotate by submission/continuation order. Ordinary work retains 4:2:1 priority selection when no reservation is runnable.

Admission rejects a sum of runtime/period greater than one with SubmitError::overloaded. This bandwidth bound does not prove deadline feasibility. Tight overlapping deadlines, ordinary callbacks, native calls, locks and OS descheduling can still cause misses.

Future releases and ready deadlines are indexed separately. Dispatch and next-deadline
queries do not scan all active slots when I/O, ordinary tasks and reservations coexist.
Admission and release still inspect the registered reservations when calculating the
utilization bound. An elapsed deadline is normalized when it becomes a scheduling
candidate, preserving skipped-release accounting and on-demand wake semantics.

The borrowed core::TimeSlice gives release, absolute deadline, actual start, slice end and remaining period runtime. End is bounded by deadline, remaining reservation, quantum and pump budget. Check should_yield() in long work. Return yield to continue the same job, complete to wait until the next period, or stop to retire. Exceptions retire and propagate. Continuation state belongs to consumer instances.

Callback wall time, including blocking and descheduling, is charged to runtime. Exhaustion throttles until the next release. A late host skips expired windows and executes at most the latest open window; completion/exhaustion advances to the next future phase boundary. Suspension preserves phase. Cancellation destroys captures before returning both slot and reservation.

run() waits for the next release or native I/O/wake; poll() remains a nonblocking host pump. Separate loop/Thread instances isolate time-sensitive work from blocking application work. BrowserLoop wakes event-only loops for submissions and deadlines, while FrameLoop uses requestAnimationFrame; both retain browser timing limits. LoopStats observes slices, yields, overruns, deadline misses, skipped releases, budget exhaustions, maximum lateness and reserved utilization.

`RealtimeSchedule::on_demand` keeps the reservation parked until `Sender::wake_realtime` supplies a presentation time. `complete` parks it again; `yield` continues under the same quota. The earliest pending wake is retained even when it arrives during a callback. Periods spent idle are not recorded as missed jobs. For ordinary non-realtime bindings, `EventLoop::on_signal` and `Sender::signal` retain one bounded callback slot, coalesce signals and optionally defer delivery until a deadline. A signal received during delivery causes a subsequent callback. Cancelling the returned token retires the callback and its captures.

A positive pump time budget permits at least one ready callback even when dispatch bookkeeping exhausts an extremely small budget. Callbacks remain cooperative and can overrun it; a zero time or callback budget still dispatches nothing. This progress rule prevents a ready task from causing an immediate wait/retry loop without ever executing.

This is cooperative soft realtime in native user space. C++ callbacks cannot be forcibly interrupted at quantum expiry. OS scheduler policy, locked memory, interrupt control and process-wide timer resolution are not changed. The design uses [Zephyr's cooperative/preemptive distinction](https://docs.zephyrproject.org/latest/kernel/services/scheduling/index.html) and the [Linux runtime/deadline/period model](https://docs.kernel.org/scheduler/sched-deadline.html); it is not a kernel RTOS port or Linux CBS implementation.

## Live playout

LiveStream<T> provides preallocated timestamp-ordered storage and a timed EventLoop consumer. Push accepts nondecreasing presentation times, including equal timestamps, and drops the oldest queued frame when full. Producers map capture/network timestamps to steady_clock and reorder packets before admission when needed. Network clock synchronization, codecs and resampling are separate responsibilities.

start(loop, schedule, handler) delivers due frames on the dispatch thread, retains future frames and discards frames older than max_lateness. An empty stream parks its reservation without a periodic callback. A push wakes it, and a future frame arms only its presentation time; the real-time runtime, deadline and quantum still bound delivery. A quantum spreads backlog across invocations. take(now) supports deterministic explicit playout. Queue operations serialize producers/consumers. Nonthrowing payload moves are required; moved-from destructors must not re-enter the queue.

Close blocks producers, cancels playout, waits for active delivery/slot retirement, destroys buffered payloads and handler, then returns allocation. Repeated/concurrent close succeeds. Own-handler close returns busy; a Runtime worker also receives busy when waiting would cross active delivery. Destruction requires a close that can finish. The borrowed loop outlives playback, and external shared owners retain ownership. Close discards rather than draining frames.

Prepared trivial payload operations and reuse of already allocated scheduler slots do not
themselves allocate. EventLoop can allocate when a new concurrent high-water mark grows
its slot storage within the configured capacity. Payload creation/captures and native/event
paths may allocate or block. This mutex queue does not replace a dedicated SPSC ring on an
OS audio realtime callback. Hardware collectors can produce timestamped blocks for EventLoop playout.

The executable usage example is examples/realtime.cpp. Exact tests and evidence are in [validation.md](validation.md).

Live query filtering and per-subscriber field selection use [QueryStream](query-stream.md). Its production timestamps are metadata; use LiveStream when presentation-time scheduling and jitter control are required. Both use existing bounded runtime execution without a dedicated stream thread.
