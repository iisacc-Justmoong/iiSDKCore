# Time-driven runtime (0.7.0)

Core/Time/Time.h defines monotonic, saturating epoch arithmetic and phase-preserving period positions independently of Runtime. Deadline uses this lower layer. EventLoop's Clock alias preserves the steady-clock epoch.

## Scheduling

EventLoop::realtime reserves a recurring slot with period, relative deadline, runtime per period, callback quantum and optional first release. Durations satisfy 0 < quantum <= runtime <= deadline <= period. Missing start means immediate release. Ready jobs use earliest absolute deadline first; equal deadlines rotate by submission/continuation order. Ordinary work retains 4:2:1 priority selection when no reservation is runnable.

Admission rejects a sum of runtime/period greater than one with SubmitError::overloaded. This bandwidth bound does not prove deadline feasibility. Tight overlapping deadlines, ordinary callbacks, native calls, locks and OS descheduling can still cause misses.

The borrowed core::TimeSlice gives release, absolute deadline, actual start, slice end and remaining period runtime. End is bounded by deadline, remaining reservation, quantum and pump budget. Check should_yield() in long work. Return yield to continue the same job, complete to wait until the next period, or stop to retire. Exceptions retire and propagate. Continuation state belongs to consumer instances.

Callback wall time, including blocking and descheduling, is charged to runtime. Exhaustion throttles until the next release. A late host skips expired windows and executes at most the latest open window; completion/exhaustion advances to the next future phase boundary. Suspension preserves phase. Cancellation destroys captures before returning both slot and reservation.

run() waits for the next release or native I/O/wake; poll() remains a nonblocking host pump. Separate loop/Thread instances isolate time-sensitive work from blocking application work. BrowserLoop/requestAnimationFrame retains browser timing limits. LoopStats observes slices, yields, overruns, deadline misses, skipped releases, budget exhaustions, maximum lateness and reserved utilization.

This is cooperative soft realtime in native user space. C++ callbacks cannot be forcibly interrupted at quantum expiry. OS scheduler policy, locked memory, interrupt control and process-wide timer resolution are not changed. The design uses [Zephyr's cooperative/preemptive distinction](https://docs.zephyrproject.org/latest/kernel/services/scheduling/index.html) and the [Linux runtime/deadline/period model](https://docs.kernel.org/scheduler/sched-deadline.html); it is not a kernel RTOS port or Linux CBS implementation.

## Live playout

LiveStream<T> provides preallocated timestamp-ordered storage and a timed EventLoop consumer. Push accepts nondecreasing presentation times, including equal timestamps, and drops the oldest queued frame when full. Producers map capture/network timestamps to steady_clock and reorder packets before admission when needed. Network clock synchronization, codecs and resampling are separate responsibilities.

start(loop, schedule, handler) delivers due frames on the dispatch thread, retains future frames and discards frames older than max_lateness. Empty/not-yet-due storage completes this period. A quantum spreads backlog across invocations. take(now) supports deterministic explicit playout. Queue operations serialize producers/consumers. Nonthrowing payload moves are required; moved-from destructors must not re-enter the queue.

Close blocks producers, cancels playout, waits for active delivery/slot retirement, destroys buffered payloads and handler, then returns allocation. Repeated/concurrent close succeeds. Own-handler close returns busy; a Runtime worker also receives busy when waiting would cross active delivery. Destruction requires a close that can finish. The borrowed loop outlives playback, and external shared owners retain ownership. Close discards rather than draining frames.

Prepared trivial payload operations and scheduler slot reuse do not themselves allocate after construction. Payload creation/captures and native/event paths may allocate or block. This mutex queue does not replace a dedicated SPSC ring on an OS audio realtime callback. Hardware collectors can produce timestamped blocks for EventLoop playout.

The executable usage example is examples/realtime.cpp. Exact tests and evidence are in [validation.md](validation.md).

Live query filtering and per-subscriber field selection use [QueryStream](query-stream.md). Its production timestamps are metadata; use LiveStream when presentation-time scheduling and jitter control are required. Both use existing bounded runtime execution without a dedicated stream thread.
