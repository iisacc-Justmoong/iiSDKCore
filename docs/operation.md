# Tracked asynchronous work (0.14.0)

## 0.20의 두 단계 소유자 종료

OperationOwner.request_close()는 소유자 stop을 요청하고 대기 작업을 취소하되 활성 작업을
기다리지 않는다. close()가 최종 실행·캡처 장벽이다. ComputeRouter는 모든 실행 자원에
중단을 먼저 전달한 뒤 기다리기 위해 두 단계를 조합한다. 기존 Operation 상태·오류·
부수 효과 의미는 유지한다. 실제 자식 프로세스 연결은 [컴퓨팅 라우터](compute-router.md)를
따르며 tests/compute_router.cpp에서 소유자 종료와 자식 정리를 검사한다.

`Runtime/Operation.h` composes existing executors and TaskScope. It does not change
QueryPort admission, cancellation or receiver-domain completion semantics. Use
typed domain requests/responses and QueryPort for object communication. Use an
Operation when a caller must retain a result even if queued work is removed.
The first production consumer is iiCSMIDI's owned-buffer asynchronous decoder.

```cpp
ii::runtime::Runtime runtime;
ii::runtime::SerialQueue replies(runtime);
ii::runtime::TaskScope receiver;
ii::runtime::OperationOwner owner;
auto task = owner.submit<int>(runtime,
    [](ii::runtime::WorkContext& context) -> ii::core::Result<int> {
        if (auto ready = context.checkpoint(); !ready)
            return std::unexpected(ready.error());
        return 42;
    });
task.then(replies, receiver.sender(), [](auto result) {
    // Runs in replies' serial domain, and only while receiver remains open.
    if (result->value) { /* use the immutable owned result */ }
});
// owner closes before receiver/replies/runtime are destroyed (reverse order).
```

Work returns `core::Result<T>`; T is a concrete non-void type, with
`std::monostate` available for commands with no data. Admission is immediately
observable as SubmitError. Queued/running states have no result. Terminal results
are immutable shared snapshots; one outcome and at most one observer are
published. Snapshot storage is caller-owned and retained independently of the
owner/executor. Executors bound admission and work capacity; callers bound the
number of retained handles/results. No process-wide operation registry exists.

| Situation | Result |
|---|---|
| Executor refuses admission | `rejected`, original `admission()` error; work never starts |
| Work returns value | `succeeded`, owned value |
| Work returns error / throws | `failed`, original Error / exception_ptr |
| Explicit cancellation before execution | `cancelled`, captures retired, no work invoked |
| Deadline expired before execution | `timed_out`, no work invoked |
| Owner closes/removes pending work | `owner_removed`, captures retired before close returns |
| Executor removes pending work | `executor_stopped`, result retained |
| Running work returns interrupted checkpoint | `owner_removed`, `timed_out` or `cancelled` |

`cancel()` requests stop, tries native executor queue cancellation and returns
whether it made a new request. Running C++ callbacks are not preempted. Checkpoints
combine the owner's std::stop_token, the operation's std::stop_token and the
monotonic Deadline. During overlapping interruptions owner removal precedes
deadline, which precedes explicit cancellation. External APIs can bridge either
stop token through `std::stop_callback`; their resource cleanup remains theirs.
Deadlines do not start watchdog threads or timer services: queued work settles
when dispatched/retired; executing work observes expiry at checkpoints. A
deadline alone never asserts that an external side effect was undone.

`mark_effects(none/possible/committed)` monotonically records external state.
Before starting a possibly partial write/transfer, mark possible; mark committed
only after confirmation. A successful return after a stop request remains success
with committed effects. Failure/cancellation after partial work retains possible
or committed effects. This permits domain-specific reconciliation instead of
inventing rollback. The pure MIDI decoder always has no side effects.

`wait_for` uses steady time. A wait timeout only stops observing and leaves work
unchanged. Runtime workers and browser main threads return `would_block` for an
unfinished result. Event-loop/UI host callbacks must poll `result()` or register
completion, not block their dispatch thread. This facility provides no fixed OS
thread affinity: SerialQueue guarantees FIFO exclusion but may move between
workers. Use existing Thread + EventLoop when an API requires fixed thread
execution. `tests/operation.cpp` verifies both execution contexts.

`then` registers one completion observer, including after a result exists. The
caller supplies a live reply executor and a `TaskScope::Sender`. The copyable
sender holds only a weak admission endpoint; owner close/removal safely rejects
late posts and cancels already queued receiver callbacks. It never revives the
receiver. `completion_submission()` reports whether the reply was admitted;
admission does not guarantee delivery after receiver/executor shutdown. Reply
executors outlive all completion submission attempts, and receiver scopes close
before their captured state is released. Callback exceptions follow the reply
executor's existing failure contract. Do not retain WorkContext references.

`OperationOwner::close` requests cooperative stop before applying TaskScope's
stop-admission, cancel-queued, wait-active, destroy-captures barrier. The owner
must not be destroyed from its own callback or a worker where joining would
block; the existing self_join/would_block contracts remain in force. Results can
outlive this boundary, but receiver callbacks cannot outlive their own scope.

The Emscripten libc++ shipped with the tested toolchain gates standard stop_token
behind `-fexperimental-library`. Runtime exports this compile option on Emscripten
so consumers use the same std::stop_token API; Contracts-only code needs no such
option. No custom stop-token substitute or interpreter is introduced. The tracked
work test is configured as a pthread program with a prewarmed pool and main proxy,
like the other worker-owning runtime tests. The 339 checks also execute as WASM32
under Node. This is not browser-host or full-runtime-suite execution evidence.

## External host completion (0.16)

Use then_on(endpoint, receiver_scope.sender(), completion) for a copyable weak EventLoop::Sender or iiGUI UiExecutor. The operation owns that endpoint value, so a local endpoint can disappear before the worker finishes. Existing then(Executor&) keeps its borrowed-executor lifetime requirement. Receiver removal still suppresses delivery, endpoint expiry is reported by completion_submission, and the two methods share exactly one observer slot. Pending wait_for calls return would_block on a HostLoop owner, as they do on Runtime workers; observe completion asynchronously. TaskScope::at and Sender::at now accept structural TimedTaskExecutor targets. See [host-loop.md](host-loop.md).
