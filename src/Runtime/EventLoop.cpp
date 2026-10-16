#include "EventLoop.h"
#include "HostLoop.h"
#include "Thread.h"
#include "../IO/Poller.h"
#include "../IO/Transfer.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ii::runtime {
namespace {
std::atomic<std::uint64_t> next_task_owner{1};
thread_local std::size_t external_hosts=0;
constexpr std::array wheel{
    Priority::high,   Priority::high,   Priority::high,      Priority::high,
    Priority::normal, Priority::normal, Priority::background};

Clock::time_point add_saturated(Clock::time_point now, Clock::duration delay) {
  if (delay <= Clock::duration::zero())
    return now;
  return core::Time::add(now, delay);
}
void validate_budget(PumpBudget budget) {
  if (budget.max_time < Clock::duration::zero())
    throw std::invalid_argument("Pump budget duration must not be negative");
}
Clock::duration transfer_timeout(std::chrono::milliseconds timeout) {
  if (timeout.count() < 0)
    return -Clock::duration(1);
  if (timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(
                     Clock::duration::max()))
    return Clock::duration::max();
  return std::chrono::duration_cast<Clock::duration>(timeout);
}
struct WakeInvocation final {
  const void* state;
  WakeInvocation* previous;
  static thread_local WakeInvocation* current;
  explicit WakeInvocation(const void* value) : state(value), previous(current) { current=this; }
  ~WakeInvocation() { current=previous; }
  static bool contains(const void* value) {
    for (auto* entry=current; entry; entry=entry->previous) if (entry->state==value) return true;
    return false;
  }
};
thread_local WakeInvocation* WakeInvocation::current=nullptr;
} // namespace

std::uint64_t TaskToken::next_owner() { return next_task_owner.fetch_add(1); }

struct HostLoop::State final {
  std::weak_ptr<EventLoop::Impl> loop;
  const std::thread::id owner=std::this_thread::get_id();
  Task wake;
  Thread waiter;
  std::atomic<bool> closing{false};
  mutable std::mutex mutex;
  std::condition_variable idle;
  HostLoopStats statistics{true};
  bool pending=false;
  std::size_t active=0;
  std::exception_ptr failure;
  bool registered=false; // Host-owner-only execution-context registration.
  void signal() noexcept {
    {
      std::lock_guard lock(mutex);
      ++statistics.wake_requests;
      if (closing.load() || pending) return;
      pending=true;
      ++active; ++statistics.wake_signals;
    }
    {
      WakeInvocation invocation(this);
      wake(); // Public constructor admits only noexcept-invocable wake targets.
    }
    { std::lock_guard lock(mutex); --active; idle.notify_all(); }
  }
  void acknowledge() {
    std::exception_ptr error;
    { std::lock_guard lock(mutex); pending=false; error=failure; }
    if (error) std::rethrow_exception(error);
  }
  void monitor(EventLoop::Impl&);
};

struct EventLoop::Impl {
  enum class Status { free, queued, executing, retiring };
  struct IOOperation {
    io::Channel channel;
    io::Interest interest = io::Interest::readable;
    IOReady ready_callback;
    IOCompletion completion;
    std::variant<std::monostate, io::Read, io::Write> request;
    io::CancellationToken stop;
    std::optional<io::Result<void>> ready;
    Clock::time_point retry_after{};
    bool probe = false;
    IOOperation() = default;
    IOOperation(IOOperation &&other) noexcept { swap(other); }
    void swap(IOOperation &other) noexcept {
      using std::swap;
      swap(channel, other.channel);
      swap(interest, other.interest);
      // Never destroy an unspecified moved-from function under the loop
      // mutex. Empty-function exchange transfers callback ownership.
      ready_callback.swap(other.ready_callback);
      completion.swap(other.completion);
      request.swap(other.request);
      swap(stop, other.stop);
      ready.swap(other.ready);
      swap(retry_after, other.retry_after);
      swap(probe, other.probe);
    }
    friend void swap(IOOperation &left, IOOperation &right) noexcept {
      left.swap(right);
    }
    bool empty_transfer() const {
      return std::visit(
          [](const auto &request) {
            if constexpr (std::same_as<std::decay_t<decltype(request)>,
                                       std::monostate>)
              return false;
            else
              return request.length == 0 ||
                     request.offset == request.buffer.bytes().size();
          },
          request);
    }
  };
  struct Slot {
    Task task;
    SliceTask slice;
    std::optional<RealtimeSchedule> realtime;
    Clock::time_point job_deadline{};
    Clock::duration spent{};
    std::optional<IOOperation> io;
    Clock::time_point deadline{};
    Clock::duration interval{};
    std::uint64_t generation = 0, sequence = 0;
    Priority priority = Priority::normal;
    Status status = Status::free;
    bool cancelled = false;
  };
  struct DispatchGuard {
    Impl &impl;
    explicit DispatchGuard(Impl &value) : impl(value) {
      std::lock_guard lock(impl.mutex);
      if (impl.host && (impl.host->owner!=std::this_thread::get_id() ||
          impl.host->closing.load() || WakeInvocation::contains(impl.host.get())))
        throw std::logic_error("Hosted EventLoop dispatch requires its host thread outside wake/close");
      if (impl.dispatching.test_and_set())
        throw std::logic_error("EventLoop dispatch is already active "
                               "(recursive or concurrent pump)");
    }
    ~DispatchGuard() { impl.dispatching.clear(); }
  };
  explicit Impl(std::size_t capacity)
      : slots(capacity), owner(TaskToken::next_owner()), reactor(capacity) {
    if (capacity == 0)
      throw std::invalid_argument("EventLoop capacity must be positive");
    free.reserve(capacity);
    io_waits.reserve(capacity);
    for (std::size_t i = capacity; i > 0; --i)
      free.push_back(i - 1);
    statistics.capacity = capacity;
  }
  void release(std::size_t index) {
    auto &slot = slots[index];
    slot.status = Status::free;
    slot.deadline = {};
    slot.interval = {};
    slot.sequence = 0;
    slot.priority = Priority::normal;
    slot.cancelled = false;
    slot.realtime.reset();
    slot.job_deadline = {};
    slot.spent = {};
    free.push_back(index);
    --statistics.pending;
    statistics.reserved_utilization = static_cast<double>(utilization());
  }
  long double utilization() const {
    long double total = 0;
    for (const auto &slot : slots)
      if (slot.status != Status::free && slot.realtime)
        total += static_cast<long double>(slot.realtime->runtime.count()) /
                 slot.realtime->period.count();
    return total;
  }
  std::optional<Clock::time_point> deadline() const {
    std::optional<Clock::time_point> next;
    if (state != LoopState::ready)
      return next;
    const auto now = Clock::now();
    for (const auto &slot : slots) {
      if (slot.status != Status::queued)
        continue;
      auto candidate = slot.deadline;
      if (slot.io) {
        if (slot.io->ready)
          candidate = now;
        else if (slot.io->retry_after > now)
          candidate = std::min(candidate, slot.io->retry_after);
        else if (candidate == Clock::time_point::max())
          continue;
      }
      if (!next || candidate < *next)
        next = candidate;
    }
    return next;
  }
  void refresh_io(Clock::time_point now) {
    for (auto &slot : slots) {
      if (slot.status != Status::queued || !slot.io)
        continue;
      auto &operation = *slot.io;
      if (operation.stop.cancelled()) {
        operation.ready =
            std::unexpected(io::Error{io::ErrorCode::cancelled, {}});
        continue;
      }
      if (operation.ready ||
          (operation.retry_after > now && slot.deadline > now))
        continue;
      auto ready = operation.empty_transfer()
                       ? io::Result<void>{}
                       : operation.channel.wait(operation.interest,
                                                std::chrono::milliseconds(0));
      if (ready || (ready.error().code != io::ErrorCode::would_block &&
                    ready.error().code != io::ErrorCode::busy)) {
        operation.ready = std::move(ready);
      } else if (slot.deadline <= now) {
        const auto error =
            operation.probe ? ready.error().code : io::ErrorCode::timed_out;
        operation.ready =
            std::unexpected(io::Error{error, ready.error().native});
      }
    }
  }
  void notify() {
    ++revision;
    changed.notify_all();
    reactor.wake();
  }
  std::optional<std::size_t> select(Clock::time_point now) {
    std::optional<std::size_t> realtime_choice;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      auto &slot = slots[i];
      if (slot.status != Status::queued || !slot.realtime)
        continue;
      if (now >= slot.job_deadline) {
        const auto position =
            core::Time::period(slot.deadline, slot.realtime->period, now);
        const bool expired =
            now >= core::Time::add(position.release, slot.realtime->deadline);
        const auto missed =
            position.skipped + static_cast<std::uint64_t>(expired);
        statistics.deadline_misses += missed;
        statistics.skipped_releases += missed;
        statistics.max_lateness =
            std::max(statistics.max_lateness,
                     core::Time::elapsed(slot.job_deadline, now));
        slot.deadline = expired ? position.next : position.release;
        slot.job_deadline =
            core::Time::add(slot.deadline, slot.realtime->deadline);
        slot.spent = {};
        slot.sequence = ++sequence;
      }
      if (slot.deadline <= now && now < slot.job_deadline &&
          (!realtime_choice ||
           slot.job_deadline < slots[*realtime_choice].job_deadline ||
           (slot.job_deadline == slots[*realtime_choice].job_deadline &&
            slot.sequence < slots[*realtime_choice].sequence)))
        realtime_choice = i;
    }
    if (realtime_choice)
      return realtime_choice;
    for (std::size_t attempt = 0; attempt < wheel.size(); ++attempt) {
      const auto priority = wheel[cursor];
      cursor = (cursor + 1) % wheel.size();
      std::optional<std::size_t> chosen;
      for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto &slot = slots[i];
        if (slot.status == Status::queued && !slot.realtime &&
            (slot.io ? bool(slot.io->ready) : slot.deadline <= now) &&
            slot.priority == priority &&
            (!chosen || slot.sequence < slots[*chosen].sequence))
          chosen = i;
      }
      if (chosen)
        return chosen;
    }
    return std::nullopt;
  }
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::atomic_flag dispatching = ATOMIC_FLAG_INIT;
  std::vector<Slot> slots;
  std::vector<std::size_t> free;
  const std::uint64_t owner;
  io::detail::ReadinessPoller reactor;
  std::vector<io::detail::WatchEntry> io_waits;
  std::uint64_t sequence = 0;
  std::size_t cursor = 0;
  LoopState state = LoopState::ready;
  LoopStats statistics;
  std::shared_ptr<HostLoop::State> host;
  std::uint64_t revision=0;
};

void HostLoop::State::monitor(EventLoop::Impl& events) {
  try {
    for (;;) {
      std::unique_lock lock(events.mutex);
      if (closing.load() || events.state==LoopState::stopped) return;
      const auto revision=events.revision;
      bool became_ready=false;
      events.io_waits.clear();
      std::optional<Clock::time_point> next;
      if (events.state==LoopState::ready) {
        std::size_t before=0,after=0;
        for (const auto& slot:events.slots) if (slot.io && slot.io->ready) ++before;
        const auto now=Clock::now();
        events.refresh_io(now);
        for (const auto& slot:events.slots) {
          if (slot.status!=EventLoop::Impl::Status::queued || !slot.io) continue;
          if (slot.io->ready) { ++after; continue; }
          events.io_waits.push_back({slot.io->channel,slot.io->interest});
          auto deadline=slot.deadline;
          if (slot.io->retry_after>now) deadline=std::min(deadline,slot.io->retry_after);
          if (!next || deadline<*next) next=deadline;
        }
        became_ready=after>before;
      }
      if (became_ready) {
        lock.unlock(); signal(); lock.lock();
        if (closing.load() || events.state==LoopState::stopped) return;
        if (events.revision!=revision) continue;
      }
      if (events.io_waits.empty()) {
        events.changed.wait(lock,[&] { return closing.load() || events.state==LoopState::stopped || events.revision!=revision; });
      } else {
        auto timeout=std::chrono::milliseconds(20);
        if (next) timeout=std::clamp(std::chrono::ceil<std::chrono::milliseconds>(*next-Clock::now()),
            std::chrono::milliseconds(0),timeout);
        events.reactor.prepare(events.io_waits,timeout);
        events.io_waits.clear();
        ++events.statistics.io_waits;
        { std::lock_guard measured(mutex); ++statistics.io_waits; }
        lock.unlock(); events.reactor.wait();
      }
    }
  } catch (...) {
    { std::lock_guard lock(mutex); failure=std::current_exception(); }
    signal();
  }
}

std::shared_ptr<HostLoop::State> HostLoop::connect(EventLoop& loop,Task wake,HostLoopOptions options) {
  if (!wake) throw std::invalid_argument("HostLoop requires a wake callback");
  auto state=std::make_shared<State>();
  state->loop=loop.impl_; state->wake=std::move(wake);
  {
    std::lock_guard lock(loop.impl_->mutex);
    if (loop.impl_->host || loop.impl_->dispatching.test() || loop.impl_->state==LoopState::stopped)
      throw std::logic_error("HostLoop requires an idle, live, unbound EventLoop");
    loop.impl_->host=state;
  }
  if (options.monitor_io) {
    auto started=state->waiter.start([state=state.get(),events=loop.impl_.get()](ThreadContext&) { state->monitor(*events); });
    if (!started) {
      (void)disconnect(state);
      throw std::runtime_error("HostLoop cannot start its native readiness waiter; disable monitor_io for a manually driven host");
    }
  }
  state->registered=true; ++external_hosts;
  state->signal(); // Include submissions admitted before the host was attached.
  return state;
}
core::Result<void> HostLoop::disconnect(std::shared_ptr<State> state) {
  if (!state) return {};
  { std::lock_guard lock(state->mutex); if (!state->statistics.connected) return {}; }
  if (state->owner!=std::this_thread::get_id())
    return core::fail(core::ErrorCode::wrong_executor,"HostLoop close requires its host thread");
  if (WakeInvocation::contains(state.get()))
    return core::fail(core::ErrorCode::busy,"HostLoop cannot detach inside its wake callback");
  const auto events=state->loop.lock();
  if (events) {
    std::lock_guard lock(events->mutex);
    if (events->dispatching.test()) return core::fail(core::ErrorCode::busy,"HostLoop cannot detach during dispatch");
    state->closing.store(true);
    events->notify();
  } else state->closing.store(true);
  Task retired;
  {
    std::unique_lock lock(state->mutex);
    state->idle.wait(lock,[&]{return state->active==0;});
    retired.swap(state->wake);
    state->statistics.connected=false;
  }
  const auto joined=state->waiter.join();
  if (state->registered) { state->registered=false; --external_hosts; }
  if (events) {
    std::lock_guard lock(events->mutex);
    if (events->host==state) events->host.reset();
  }
  // Native wake captures retire after the barrier and outside Core locks.
  retired=nullptr;
  if (!joined) return core::fail(core::ErrorCode::failed,"HostLoop readiness waiter failed");
  std::exception_ptr failure;
  { std::lock_guard lock(state->mutex); failure=state->failure; }
  if (failure) return std::unexpected(core::Error{core::ErrorCode::failed,"HostLoop readiness wait failed",failure});
  return {};
}
HostLoop::~HostLoop() {
  const auto closed=close();
  if (!closed && (closed.error().code==core::ErrorCode::busy || closed.error().code==core::ErrorCode::wrong_executor))
    std::terminate();
}
core::Result<void> HostLoop::close() { return disconnect(state_); }
HostLoopStats HostLoop::stats() const { std::lock_guard lock(state_->mutex); return state_->statistics; }
bool HostLoop::is_current_thread() noexcept { return external_hosts!=0; }

EventLoop::EventLoop(std::size_t capacity)
    : impl_(std::make_shared<Impl>(capacity)) {}
EventLoop::~EventLoop() {
  request_stop();
  if (impl_->host) {
    const auto detached=HostLoop::disconnect(impl_->host);
    if (!detached && (detached.error().code==core::ErrorCode::busy || detached.error().code==core::ErrorCode::wrong_executor)) std::terminate();
  }
}

Submission EventLoop::submit(const std::shared_ptr<Impl>& impl_,Clock::time_point deadline,
                             Clock::duration interval, Task task,
                             Priority priority) {
  std::unique_lock lock(impl_->mutex);
  auto reject = [&](SubmitError error) {
    ++impl_->statistics.rejected;
    return Submission{{}, error};
  };
  if (!task ||
      (priority != Priority::high && priority != Priority::normal &&
       priority != Priority::background) ||
      interval < Clock::duration::zero())
    return reject(SubmitError::invalid);
  if (impl_->state == LoopState::stopped)
    return reject(SubmitError::stopped);
  if (impl_->free.empty())
    return reject(SubmitError::full);
  const auto index = impl_->free.back();
  impl_->free.pop_back();
  auto &slot = impl_->slots[index];
  slot.task.swap(task);
  slot.deadline = deadline;
  slot.interval = interval;
  slot.priority = priority;
  slot.sequence = ++impl_->sequence;
  ++slot.generation;
  slot.cancelled = false;
  slot.status = Impl::Status::queued;
  ++impl_->statistics.submitted;
  ++impl_->statistics.pending;
  impl_->statistics.high_watermark =
      std::max(impl_->statistics.high_watermark, impl_->statistics.pending);
  TaskToken token;
  token.owner_ = impl_->owner;
  token.generation_ = slot.generation;
  token.slot_ = index;
  token.executor_ = impl_;
  token.cancel_ = &EventLoop::cancel_impl;
  token.pending_ = &EventLoop::pending_impl;
  impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
  return {token, SubmitError::none};
}

Submission EventLoop::post(Task task, Priority priority) {
  return submit(impl_,Clock::now(), {}, std::move(task), priority);
}
Submission EventLoop::at(Clock::time_point deadline, Task task,
                         Priority priority) {
  // Clamp past deadlines to avoid overflow in timer arithmetic.
  return submit(impl_,std::max(deadline, Clock::now()), {}, std::move(task),
                priority);
}
Submission EventLoop::after(Clock::duration delay, Task task,
                            Priority priority) {
  return submit(impl_,add_saturated(Clock::now(), delay), {}, std::move(task),
                priority);
}
Submission EventLoop::every(Clock::duration interval, Task task,
                            Priority priority) {
  return submit_every(impl_,interval,std::move(task),priority);
}
Submission EventLoop::submit_every(const std::shared_ptr<Impl>& impl_,Clock::duration interval,Task task,Priority priority) {
  if (interval <= Clock::duration::zero()) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->statistics.rejected;
    return {{}, SubmitError::invalid};
  }
  return submit(impl_,add_saturated(Clock::now(), interval), interval,
                std::move(task), priority);
}

Submission EventLoop::realtime(RealtimeSchedule schedule, SliceTask task) {
  return submit_realtime(impl_,std::move(schedule),std::move(task));
}
Submission EventLoop::submit_realtime(const std::shared_ptr<Impl>& impl_,RealtimeSchedule schedule,SliceTask task) {
  std::unique_lock lock(impl_->mutex);
  auto reject = [&](SubmitError error) {
    ++impl_->statistics.rejected;
    return Submission{{}, error};
  };
  if (!task || schedule.period <= Clock::duration::zero() ||
      schedule.deadline <= Clock::duration::zero() ||
      schedule.deadline > schedule.period ||
      schedule.runtime <= Clock::duration::zero() ||
      schedule.runtime > schedule.deadline ||
      schedule.quantum <= Clock::duration::zero() ||
      schedule.quantum > schedule.runtime)
    return reject(SubmitError::invalid);
  if (impl_->state == LoopState::stopped)
    return reject(SubmitError::stopped);
  if (impl_->free.empty())
    return reject(SubmitError::full);
  const auto utilization = impl_->utilization() +
                           static_cast<long double>(schedule.runtime.count()) /
                               schedule.period.count();
  if (utilization > 1.0L)
    return reject(SubmitError::overloaded);
  const auto index = impl_->free.back();
  impl_->free.pop_back();
  auto &slot = impl_->slots[index];
  slot.slice.swap(task);
  slot.realtime = schedule;
  slot.deadline = schedule.start.value_or(Clock::now());
  slot.job_deadline = core::Time::add(slot.deadline, schedule.deadline);
  slot.spent = {};
  slot.sequence = ++impl_->sequence;
  ++slot.generation;
  slot.cancelled = false;
  slot.status = Impl::Status::queued;
  ++impl_->statistics.submitted;
  ++impl_->statistics.pending;
  impl_->statistics.high_watermark =
      std::max(impl_->statistics.high_watermark, impl_->statistics.pending);
  impl_->statistics.reserved_utilization = static_cast<double>(utilization);
  TaskToken token;
  token.owner_ = impl_->owner;
  token.generation_ = slot.generation;
  token.slot_ = index;
  token.executor_ = impl_;
  token.cancel_ = &EventLoop::cancel_impl;
  token.pending_ = &EventLoop::pending_impl;
  impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
  return {token, SubmitError::none};
}

Submission
EventLoop::submit_io(const std::shared_ptr<Impl>& impl_,io::Channel channel, io::Interest interest, IOReady ready,
                     IOCompletion completion,
                     std::variant<std::monostate, io::Read, io::Write> request,
                     IOWatchOptions options) {
  std::unique_lock lock(impl_->mutex);
  auto reject = [&](SubmitError error) {
    ++impl_->statistics.rejected;
    return Submission{{}, error};
  };
  const bool valid_request = std::visit(
      [](const auto &value) {
        if constexpr (std::same_as<std::decay_t<decltype(value)>,
                                   std::monostate>)
          return true;
        else
          return io::detail::valid_transfer(value);
      },
      request);
  if ((interest != io::Interest::readable &&
       interest != io::Interest::writable) ||
      (options.priority != Priority::high &&
       options.priority != Priority::normal &&
       options.priority != Priority::background) ||
      (options.timeout && *options.timeout < Clock::duration::zero()) ||
      !valid_request || (request.index() == 0 ? !ready : !completion))
    return reject(SubmitError::invalid);
  if (impl_->state == LoopState::stopped)
    return reject(SubmitError::stopped);
  if (impl_->free.empty())
    return reject(SubmitError::full);
  const auto index = impl_->free.back();
  impl_->free.pop_back();
  auto &slot = impl_->slots[index];
  auto &operation = slot.io.emplace();
  operation.channel = std::move(channel);
  operation.interest = interest;
  operation.ready_callback.swap(ready);
  operation.completion.swap(completion);
  operation.request = std::move(request);
  operation.stop = std::move(options.stop);
  operation.probe =
      options.timeout && *options.timeout == Clock::duration::zero();
  slot.deadline = options.timeout
                      ? add_saturated(Clock::now(), *options.timeout)
                      : Clock::time_point::max();
  slot.priority = options.priority;
  slot.sequence = ++impl_->sequence;
  ++slot.generation;
  slot.cancelled = false;
  slot.status = Impl::Status::queued;
  ++impl_->statistics.submitted;
  ++impl_->statistics.pending;
  impl_->statistics.high_watermark =
      std::max(impl_->statistics.high_watermark, impl_->statistics.pending);
  TaskToken token;
  token.owner_ = impl_->owner;
  token.generation_ = slot.generation;
  token.slot_ = index;
  token.executor_ = impl_;
  token.cancel_ = &EventLoop::cancel_impl;
  token.pending_ = &EventLoop::pending_impl;
  impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
  return {token, SubmitError::none};
}
Submission EventLoop::watch(io::Channel channel, io::Interest interest,
                            IOReady ready, IOWatchOptions options) {
  return submit_io(impl_,std::move(channel), interest, std::move(ready), {},
                   std::monostate{}, std::move(options));
}
Submission EventLoop::read(io::Channel channel, io::Read request,
                           IOCompletion completion, Priority priority) {
  IOWatchOptions options{transfer_timeout(request.timeout), request.stop,
                         priority};
  return submit_io(impl_,std::move(channel), io::Interest::readable, {},
                   std::move(completion), std::move(request),
                   std::move(options));
}
Submission EventLoop::write(io::Channel channel, io::Write request,
                            IOCompletion completion, Priority priority) {
  IOWatchOptions options{transfer_timeout(request.timeout), request.stop,
                         priority};
  return submit_io(impl_,std::move(channel), io::Interest::writable, {},
                   std::move(completion), std::move(request),
                   std::move(options));
}

bool EventLoop::cancel(TaskToken token) {
  return cancel_impl(impl_.get(), token);
}
Submission EventLoop::Sender::post(Task task,Priority priority) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit(state,Clock::now(),{},std::move(task),priority) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::at(Clock::time_point time,Task task,Priority priority) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit(state,std::max(time,Clock::now()),{},std::move(task),priority) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::after(Clock::duration delay,Task task,Priority priority) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit(state,add_saturated(Clock::now(),delay),{},std::move(task),priority) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::every(Clock::duration period,Task task,Priority priority) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit_every(state,period,std::move(task),priority) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::realtime(RealtimeSchedule schedule,SliceTask task) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit_realtime(state,std::move(schedule),std::move(task)) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::watch(io::Channel channel,io::Interest interest,IOReady ready,IOWatchOptions options) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit_io(state,std::move(channel),interest,std::move(ready),{},std::monostate{},std::move(options)) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::read(io::Channel channel,io::Read request,IOCompletion completion,Priority priority) const {
  const auto state=state_.lock();
  IOWatchOptions options{transfer_timeout(request.timeout),request.stop,priority};
  return state ? EventLoop::submit_io(state,std::move(channel),io::Interest::readable,{},std::move(completion),std::move(request),std::move(options)) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::write(io::Channel channel,io::Write request,IOCompletion completion,Priority priority) const {
  const auto state=state_.lock();
  IOWatchOptions options{transfer_timeout(request.timeout),request.stop,priority};
  return state ? EventLoop::submit_io(state,std::move(channel),io::Interest::writable,{},std::move(completion),std::move(request),std::move(options)) : Submission{{},SubmitError::stopped};
}
bool EventLoop::Sender::cancel(TaskToken token) const {
  const auto state=state_.lock(); return state && EventLoop::cancel_impl(state.get(),token);
}
bool EventLoop::pending_impl(void *owner, const TaskToken &token) {
  auto &impl = *static_cast<Impl *>(owner);
  std::lock_guard lock(impl.mutex);
  return token.owner_ == impl.owner && token.slot_ < impl.slots.size() &&
         impl.slots[token.slot_].generation == token.generation_ &&
         impl.slots[token.slot_].status != Impl::Status::free;
}
bool EventLoop::cancel_impl(void *owner, const TaskToken &token) {
  auto &impl = *static_cast<Impl *>(owner);
  Task retired;
  SliceTask retired_slice;
  std::optional<Impl::IOOperation> retired_io;
  bool queued = false;
  std::shared_ptr<HostLoop::State> host;
  {
    std::lock_guard lock(impl.mutex);
    if (token.owner_ != impl.owner || token.slot_ >= impl.slots.size())
      return false;
    auto &slot = impl.slots[token.slot_];
    if (slot.generation != token.generation_ ||
        slot.status == Impl::Status::free ||
        slot.status == Impl::Status::retiring || slot.cancelled)
      return false;
    slot.cancelled = true;
    ++impl.statistics.cancelled;
    if (slot.status == Impl::Status::queued) {
      retired.swap(slot.task);
      retired_slice.swap(slot.slice);
      retired_io.swap(slot.io);
      slot.status = Impl::Status::retiring;
      queued = true;
    }
    impl.notify();
    host=impl.host;
  }
  if (retired_io)
    impl.reactor.synchronize();
  retired =
      nullptr; // finish resource destruction before exposing the free slot
  retired_slice = nullptr;
  retired_io.reset();
  if (queued) {
    std::lock_guard lock(impl.mutex);
    impl.release(token.slot_);
    impl.changed.notify_all();
  }
  if (host) host->signal();
  return true;
}

std::size_t EventLoop::dispatch(PumpBudget budget) {
  const auto end = add_saturated(Clock::now(), budget.max_time);
  std::size_t executed = 0, attempts = 0;
  while (attempts < budget.max_callbacks && Clock::now() < end) {
    Task task;
    SliceTask slice;
    std::optional<RealtimeSchedule> realtime;
    core::TimeSlice window;
    std::optional<Impl::IOOperation> operation;
    Clock::time_point deadline;
    std::size_t index;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->state != LoopState::ready)
        break;
      impl_->refresh_io(Clock::now());
      const auto chosen = impl_->select(Clock::now());
      if (!chosen)
        break;
      index = *chosen;
      auto &slot = impl_->slots[index];
      slot.status = Impl::Status::executing;
      // std::function's moved-from state is unspecified: swap with an
      // empty function so reused slots never retain callback captures.
      task.swap(slot.task);
      slice.swap(slot.slice);
      realtime = slot.realtime;
      if (realtime) {
        const auto start = Clock::now();
        const auto remaining = realtime->runtime - slot.spent;
        window = {
            slot.deadline, slot.job_deadline, start,
            std::min({end, slot.job_deadline,
                      core::Time::add(start,
                                      std::min(remaining, realtime->quantum))}),
            remaining};
      }
      operation.swap(slot.io);
      deadline = slot.deadline;
    }
    ++attempts;
    std::exception_ptr failure;
    bool retried = false;
    SliceResult result = SliceResult::yield;
    try {
      if (realtime) {
        if (!window.should_yield())
          result = slice(window);
        if (result != SliceResult::complete && result != SliceResult::yield &&
            result != SliceResult::stop)
          throw std::logic_error("invalid realtime slice result");
      } else if (!operation)
        task();
      else if (operation->request.index() == 0)
        operation->ready_callback(std::move(*operation->ready));
      else {
        io::IOResult reply =
            std::unexpected(io::Error{io::ErrorCode::invalid_argument, {}});
        if (!*operation->ready)
          reply = std::unexpected(operation->ready->error());
        else
          reply = std::visit(
              [&](const auto &request) -> io::IOResult {
                if constexpr (std::same_as<std::decay_t<decltype(request)>,
                                           std::monostate>)
                  return std::unexpected(
                      io::Error{io::ErrorCode::invalid_argument, {}});
                else
                  return io::detail::transfer_now(operation->channel, request);
              },
              operation->request);
        if (!reply && !operation->probe &&
            (reply.error().code == io::ErrorCode::would_block ||
             reply.error().code == io::ErrorCode::busy)) {
          const auto now = Clock::now();
          if (now < deadline) {
            std::lock_guard lock(impl_->mutex);
            auto &slot = impl_->slots[index];
            if (!slot.cancelled && impl_->state != LoopState::stopped) {
              operation->ready.reset();
              operation->retry_after =
                  add_saturated(now, std::chrono::milliseconds(1));
              slot.io.swap(operation);
              slot.sequence = ++impl_->sequence;
              slot.status = Impl::Status::queued;
              retried = true;
            }
          } else
            reply = std::unexpected(
                io::Error{io::ErrorCode::timed_out, reply.error().native});
        }
        if (!retried)
          operation->completion(std::move(reply));
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (retried)
      continue;
    {
      std::lock_guard lock(impl_->mutex);
      auto &slot = impl_->slots[index];
      ++impl_->statistics.executed;
      if (realtime) {
        const auto finished = Clock::now();
        const auto elapsed = core::Time::elapsed(window.started, finished);
        slot.spent = core::Time::add(Clock::time_point(slot.spent), elapsed)
                         .time_since_epoch();
        ++impl_->statistics.slices;
        if (finished > window.ends)
          ++impl_->statistics.slice_overruns;
        if (result == SliceResult::yield)
          ++impl_->statistics.slice_yields;
        if (finished >= window.deadline) {
          ++impl_->statistics.deadline_misses;
          impl_->statistics.max_lateness =
              std::max(impl_->statistics.max_lateness,
                       core::Time::elapsed(window.deadline, finished));
        }
        if (!failure && !slot.cancelled && result != SliceResult::stop &&
            impl_->state != LoopState::stopped) {
          if (result != SliceResult::yield || slot.spent >= realtime->runtime ||
              finished >= window.deadline) {
            if (result == SliceResult::yield && slot.spent >= realtime->runtime)
              ++impl_->statistics.budget_exhaustions;
            const auto position =
                core::Time::period(slot.deadline, realtime->period, finished);
            impl_->statistics.skipped_releases += position.skipped;
            slot.deadline = position.next;
            slot.job_deadline =
                core::Time::add(slot.deadline, realtime->deadline);
            slot.spent = {};
          }
          slot.slice.swap(slice);
          slot.sequence = ++impl_->sequence;
          slot.status = Impl::Status::queued;
        } else
          slot.status = Impl::Status::retiring;
      } else if (!failure && !slot.cancelled &&
                 slot.interval > Clock::duration::zero() &&
                 impl_->state != LoopState::stopped) {
        const auto position =
            core::Time::period(slot.deadline, slot.interval, Clock::now());
        impl_->statistics.missed_ticks += position.skipped;
        // Preserve phase, but never run an unbounded catch-up burst.
        slot.deadline = position.next;
        slot.task.swap(task);
        slot.sequence = ++impl_->sequence;
        slot.status = Impl::Status::queued;
      } else {
        slot.status = Impl::Status::retiring;
      }
    }
    // Non-periodic captures must retire before their slot can be reused.
    if (task || slice || operation) {
      task = nullptr;
      slice = nullptr;
      operation.reset();
      std::lock_guard lock(impl_->mutex);
      impl_->release(index);
    }
    ++executed;
    if (failure)
      std::rethrow_exception(failure);
  }
  return executed;
}

std::size_t EventLoop::poll(PumpBudget budget) {
  validate_budget(budget);
  Impl::DispatchGuard guard(*impl_);
  std::shared_ptr<HostLoop::State> host;
  { std::lock_guard lock(impl_->mutex); host=impl_->host; }
  if (host) host->acknowledge();
  struct Changed final {
    Impl& state;
    ~Changed() { std::lock_guard lock(state.mutex); state.notify(); }
  } changed{*impl_};
  return dispatch(budget);
}

void EventLoop::run(PumpBudget budget) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->host) throw std::logic_error("Hosted EventLoop is driven by its external host; blocking run is unavailable");
  }
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  (void)budget;
  throw std::logic_error("WASM requires host-driven poll() or BrowserLoop; "
                         "run() would block the host");
#else
#if defined(__EMSCRIPTEN__)
  if (emscripten_is_main_browser_thread())
    throw std::logic_error(
        "EventLoop::run cannot block the browser main thread");
#endif
  validate_budget(budget);
  if (budget.max_callbacks == 0 || budget.max_time == Clock::duration::zero())
    throw std::invalid_argument("Blocking run requires a positive pump budget");
  Impl::DispatchGuard guard(*impl_);
  for (;;) {
    dispatch(budget);
    std::unique_lock lock(impl_->mutex);
    if (impl_->state == LoopState::stopped)
      return;
    const auto next = impl_->deadline();
    if (next && *next <= Clock::now())
      continue;
    impl_->io_waits.clear();
    if (impl_->state == LoopState::ready) {
      for (const auto &slot : impl_->slots)
        if (slot.status == Impl::Status::queued && slot.io && !slot.io->ready)
          impl_->io_waits.push_back({slot.io->channel, slot.io->interest});
    }
    if (!impl_->io_waits.empty()) {
      auto timeout = std::chrono::milliseconds(20);
      if (next)
        timeout = std::min(
            timeout,
            std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()));
      impl_->reactor.prepare(impl_->io_waits, timeout);
      ++impl_->statistics.io_waits;
      impl_->io_waits.clear(); // only prepared native leases retain channels
      lock.unlock();
      impl_->reactor.wait();
    } else if (next)
      impl_->changed.wait_until(lock, *next);
    else
      impl_->changed.wait(lock);
    // Recompute after every wake: posts, cancellations and lifecycle changes
    // share this mutex, preventing the check/sleep lost-wakeup race.
  }
#endif
}

void EventLoop::suspend() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state == LoopState::ready)
    impl_->state = LoopState::suspended;
  impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
}
void EventLoop::resume() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state == LoopState::suspended)
    impl_->state = LoopState::ready;
  impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
}
void EventLoop::request_stop() {
  std::shared_ptr<HostLoop::State> host;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->state = LoopState::stopped;
    impl_->notify();
    host=impl_->host;
  }
  for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
    TaskToken token;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->slots[i].status != Impl::Status::queued)
        continue;
      token.owner_ = impl_->owner;
      token.slot_ = i;
      token.generation_ = impl_->slots[i].generation;
    }
    (void)cancel(token);
  }
  if (host) host->signal();
}
LoopState EventLoop::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}
LoopStats EventLoop::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->statistics;
}
std::optional<Clock::time_point> EventLoop::next_deadline() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->deadline();
}

Platform current_platform() noexcept {
#if defined(__EMSCRIPTEN__)
  return Platform::wasm;
#elif defined(__ANDROID__)
  return Platform::android;
#elif defined(_WIN32)
  return Platform::windows;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  return Platform::ios;
#elif defined(__APPLE__)
  return Platform::macos;
#elif defined(__linux__)
  return Platform::linux_os;
#else
  return Platform::unknown;
#endif
}
std::string_view platform_name() noexcept {
  switch (current_platform()) {
  case Platform::macos:
    return "macOS";
  case Platform::windows:
    return "Windows";
  case Platform::linux_os:
    return "Linux";
  case Platform::ios:
    return "iOS";
  case Platform::android:
    return "Android";
  case Platform::wasm:
    return "WASM";
  case Platform::unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace ii::runtime
