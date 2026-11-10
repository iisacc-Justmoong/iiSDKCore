# Selective live data streams

Core 0.18 adds `<iiSDKCore/Runtime/QueryStream.h>` to the Runtime component.
`QueryStream<Query, Source, Output>` is an instance-owned typed feed for live
application/game data. It composes QueryPort, TaskScope and existing executors;
it creates no threads. Independent subscribers can request different data on
different receivers, such as a UI panel and a background processor.

LiveStream remains the timestamp-ordered jitter/playout buffer. QueryStream
selects data at publication and dispatches it when its executor runs. Production
timestamps do not schedule presentation. EventLoop timers, native I/O, FrameLoop
ticks and Runtime jobs can supply sources. HostLoop/iiGUI can dispatch selected
results through the same EventLoop.

## Selection and ownership

The projector is `std::optional<Output>(const Query&, const Source&)`.
An absent optional filters that source. A present Output contains the requested
fields. Domain code owns filters, nested selections, access control and field
construction. QueryStream does not inspect object memory, infer field names or
expose private members. Check authorization before reading protected fields.
An empty selection must not implicitly select a whole object. Optional response
fields distinguish omission from zero/false/empty values; use an explicit
additional state for nullable domain values when needed.

```cpp
struct Select final { int device; bool level = false, name = false; };
struct Sample final { int device, level; std::string name; };
struct Fields final {
    std::optional<int> level;
    std::optional<std::string> name;
};
ii::runtime::EventLoop ui;
ii::runtime::QueryStream<Select, Sample, Fields> feed(
    [](const Select& query, const Sample& source) -> std::optional<Fields> {
        if (query.device != source.device) return {};
        Fields result;
        if (query.level) result.level = source.level;
        if (query.name) result.name = source.name;
        return result;
    });
auto subscriber = feed.subscribe_on(ui.sender(), {7, true, false},
    [](ii::runtime::QueryStreamEvent<Fields> event) {
        consume(*event.value.level); // zero is present; name was never copied
    });
if (!subscriber) handle_subscription_failure(subscriber.error());
auto publication = feed.publish({7, 0, "not selected"});
ui.poll();
```

publish borrows Source until it returns, serializes producer selection for this
feed and calls each open subscriber's projector on the publishing thread. It
queues only projected Output. Do not retain source/query input references.
Outputs own their data; a borrowed view requires an explicit owner such as a
shared immutable snapshot or ii::resource::View. Output can be move-only and must
support nonthrowing move construction/assignment. Source need not be copyable;
Query must be copy constructible.

Every publication has an increasing sequence, including publications with no
matches. Events carry sequence, subscription revision and a steady-clock
production timestamp. Successful deliveries preserve publication order per
subscriber, with visible gaps from filtering/discard/overflow. Subscribe before
publishing: there is no retained initial snapshot or replay.

`publish_changed(source, affected)`는 소스의 변경 사건과 각 구독 쿼리를 비교하여 영향을
받는 구독에만 projector를 실행한다. `affected(query, source)`는 동기식이며 false는
기존 필터 결과와 구분되는 `unaffected`로 집계한다. 호출자는 변경 필드나 자원 식별자를
판정 함수에 제공한다. 값 자체도 변경 필드만 출력하려면 변경 정보를 Source에 담아
projector에서 명시적으로 사용한다. 빈 변경을 임의로 전체 변경으로 해석하지 않는다.
기존 `publish`는 명시적인 전체 소스 게시 계약을 유지한다.

예를 들어 위의 `feed`에서 원본 값 중 `level`만 변경된 사건은 다음과 같이 전달한다.
판정 함수는 게시 호출 안에서 동기적으로 실행되므로 변경 식별자를 값 또는 안전한 참조로
전달할 수 있다.

```cpp
const bool level_changed = true;
const bool name_changed = false;
auto update = feed.publish_changed(Sample{7, 42, "unchanged"},
    [&](const Select& query, const Sample& source) {
        return query.device == source.device &&
               ((level_changed && query.level) || (name_changed && query.name));
    });
```

변경 사실과 쿼리의 관계는 도메인 소유자가 정확히 제공해야 한다. 영향을 받은 쿼리를
false로 판정하면 해당 수신자는 갱신을 받지 못한다. 변경이 없는 경우에는 생산자 자체가
게시하지 않는 것이 가장 적다. 판정 함수 예외는 해당 구독의 `projection_failed`와
`first_failure`에 기록되고 다른 구독은 계속 처리한다. 위 projector는 영향을 받은 쿼리의
모든 선택 필드를 구성하므로, 필드별 부분 값이 필요하면 Source에 변경 마스크를 포함하여
그 마스크와 쿼리가 모두 선택한 필드만 구성해야 한다. `publish_changed`는 등록된 쿼리의
영향 여부를 각각 판정하며, 임의의 사용자 쿼리에 대한 자동 색인은 제공하지 않는다.

구독 등록·제거가 없으면 게시마다 구독 목록 전체를 복사하지 않는다. 구독자 구성이
변했을 때만 게시 경계에서 유효한 구성 스냅샷을 다시 만들며, 동시에 제거된 구독은
기존 수명 장벽과 revision 검사를 따른다. `stats()`의 membership_rebuilds는 이 구성
갱신 횟수를 나타낸다.

## Query replacement

Each move-only Subscription owns a
`QueryPort<Query, QueryStreamReply<std::uint64_t>>`, exposed by queries().
Use `queries().send(next)` for synchronous replacement or
`queries().post(executor, next, completion)` for asynchronous control that owns
the request. This control port is intentionally unbound and synchronizes its own
state. The delivery executor does not implicitly order independent control
requests. Posting control and delivery to one SerialQueue gives FIFO ordering
when needed. Keep the handle alive until control finishes, or close it to cancel.

The initial revision is 1. Replacement increments it, removes queued old output
and counts that output as discarded. An in-flight projection checks its revision
again before enqueueing; an old result becomes stale. A delivery already claimed
by an executing pump, including one about to enter its callback, cannot be
preempted; its event retains the old revision. For a stronger boundary,
route control and delivery through one serial domain or check revisions in
receiver state.

Replies have two explicit layers: QueryReply reports port/handler failure and
its QueryStreamReply value reports control success/rejection. queries() on a
moved handle throws logic_error; close/resume/stats on it safely describe closed
admission. A closed handle keeps its identity across registry slot reuse.

## Bounds and dispatch

QueryStreamOptions limits subscription slots (default 16, valid 1..4096).
QuerySubscriptionOptions sets fixed ring capacity (default 64, valid 1..65536),
overflow policy, callback priority and events per dispatch batch (default 64,
valid 1..65536).

- reject preserves admitted output and counts rejected new matches.
- drop_oldest removes the oldest queued output before admitting the new match.
- Query replacement/close accounts discarded output separately from overflow.

One scheduling ticket per subscription coalesces bursts. Callbacks never overlap
for that subscription, even with a parallel executor; independent subscriptions
may run concurrently. A dispatch processes at most delivery_batch outputs, then
yields through another executor submission. User code is cooperative and must
return promptly.

Output admission and executor admission are separate. Full/stopped executors
leave admitted output in the bounded ring and record dispatch_failed and
last_dispatch. A later accepted publication or resume() retries it. Cancellation
releases the scheduling ticket too, so retry cannot become stuck. There is no
busy retry loop, hidden worker or unbounded queue. If a batch resubmission finds
the executor full (including a one-slot executor still occupied by that batch),
stats expose the failure; resume after capacity returns. Arrange a host retry or
poll stats when publication has stopped. Admission is not exactly-once delivery.

publish returns QueryStreamPublish with visited/filtered/enqueued/rejected/stale,
projection_failed/dispatch_failed and the first projector exception. Subscriber
stats add high-water mark, revision, successful deliveries, discard and callback
failure. Queue admission does not wait for consumers; selection is synchronous
and may block/allocate if domain code does. Ring slots are reused, while tasks,
projections, query replacement and subscription creation may allocate. Each
matched output owns an allocated event node. Only owning pointers move under
the ring lock, so payload moves/destructors and endpoint copies cannot reenter
a held state lock. Capacity
bounds item count; domain code must bound arbitrary payload byte size. This is
not a hard real-time or allocation-free primitive.

## Lifetime, executors and failures

subscribe_on(copyable_endpoint, ...) stores a copy. EventLoop::Sender is weak
and stays safe after loop removal; subscribe(EventLoop&, ...) also uses it.
subscribe(Runtime&, ...) and subscribe(SerialQueue&, ...) borrow those executors,
which must outlive subscription/stream close. Custom endpoints must satisfy
TaskExecutor and defer execution, following Core's asynchronous contract.
Receiver captures must own/protect their referenced objects. Declare and close
subscriptions before destroying receiver state.

A projector exception is recorded per subscriber and in the publication report;
other subscribers continue. A delivery exception is recorded and rethrown into
the executor's existing failure handling. That event is consumed; remaining
events may be resumed. failure() retains the latest cause until close. Admission
errors are stopped/invalid/exhausted/reentrant. Recursive publication into the
same feed, including cross-feed projection cycles, is rejected.

Subscription close stops data/control, closes the query port, cancels queued
pumps, waits for active projections/callbacks and capture destruction, then
clears selected query/output/receiver captures before its registry slot is
reused. Stream close first stops producer/subscriber admission and waits for
selection, then closes all subscriptions. Handles may outlive the stream and
remain safely closed. Independently scheduled downstream user work has its own
lifetime boundary.

`Subscription::try_close()` closes admission and cancels pending delivery
without waiting for an active projection, control query, or callback. It returns
`would_block` while those owners are still retiring; retain the subscription and
retry from a safe owner context until it succeeds. It does not relax the
`self_join` rule or the destructor's completed-removal requirement. The control
port's `QueryPort::try_close()` supplies the same nonblocking removal barrier.

Self-close from the same selector/callback/capture destructor returns self_join.
A Runtime worker or browser host that would need to wait receives would_block;
retry the incomplete removal barrier from a safe external context. Destruction
where removal cannot finish terminates, consistent with TaskScope. Cancellation
does not preempt arbitrary active C++ code.

The portable C++23 implementation imports no Qt/device/storage/transport SDK.
Network/media transport, encoders, file creation and replay storage belong to
feature modules. WASM uses compiled C++ and the existing pthread/host contract;
blocking the browser main thread remains prohibited.

Executable timer/worker/receiver example: examples/query_stream.cpp. A producer
SerialQueue preserves tick order across the persistent worker pool; the receiver
uses EventLoop timers and changes its typed query while the feed is live. The
producer publishes field changes and the projector constructs only fields that
are both selected and changed.
Contract tests: tests/query_stream.cpp and the relocated installed-package
consumer in tests/incremental-consumer.
Current verification evidence: [validation.md](validation.md).
