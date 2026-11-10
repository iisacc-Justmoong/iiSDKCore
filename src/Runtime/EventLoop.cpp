#include "EventLoop.h"
#include "EventLoopIndex.h"
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
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ii::runtime {
namespace {
std::atomic<std::uint64_t> next_task_owner{1};
thread_local std::size_t external_hosts=0;
thread_local const void* current_dispatch=nullptr;
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
  bool monitor_io=false;
  bool io_waiter_active=false;
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
    { std::lock_guard lock(mutex); if (--active==0 && closing.load()) idle.notify_all(); }
  }
  void acknowledge() {
    std::exception_ptr error;
    { std::lock_guard lock(mutex); pending=false; error=failure; }
    if (error) std::rethrow_exception(error);
  }
  // Caller holds the EventLoop mutex, serializing attachment and I/O admission.
  void start_monitor(EventLoop::Impl&);
  void monitor(EventLoop::Impl&);
};

struct EventLoop::Impl {
  enum class Status : unsigned char { free, queued, executing, retiring };
  struct RetirementWait final { std::condition_variable changed; std::size_t count=0; };
  enum class Scheduled : unsigned char { none, timed, realtime_timed, high, normal, background, realtime };
  struct IOOperation {
    io::Channel channel;
    io::Interest interest = io::Interest::readable;
    IOReady ready_callback;
    IOCompletion completion;
    std::variant<std::monostate, io::Read, io::Write> request;
    io::CancellationToken stop;
    std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
    std::optional<io::Result<void>> ready;
    std::optional<std::chrono::milliseconds> polling_interval;
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
      swap(cancellation, other.cancellation);
      ready.swap(other.ready);
      swap(polling_interval, other.polling_interval);
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
  struct RealtimeOperation final {
    RealtimeSchedule schedule;
    SliceTask slice;
    Clock::time_point job_deadline{};
    Clock::duration spent{};
  };
  struct Slot {
    Task task;
    std::unique_ptr<RealtimeOperation> realtime;
    std::unique_ptr<IOOperation> io;
    Clock::time_point deadline{};
    Clock::duration interval{};
    std::uint64_t generation = 0, sequence = 0;
    Clock::time_point wake_at{};
    Priority priority = Priority::normal;
    std::size_t io_position = 0;
    Status status = Status::free;
    Scheduled scheduled = Scheduled::none;
    bool tracked_io = false;
    bool cancelled = false, signaled_task = false, has_wake = false;
  };
  struct DispatchGuard {
    Impl &impl;
    const void* previous;
    explicit DispatchGuard(Impl &value) : impl(value), previous(current_dispatch) {
      std::lock_guard lock(impl.mutex);
      if (impl.host && (impl.host->owner!=std::this_thread::get_id() ||
          impl.host->closing.load() || WakeInvocation::contains(impl.host.get())))
        throw std::logic_error("Hosted EventLoop dispatch requires its host thread outside wake/close");
      if (impl.dispatching.test_and_set())
        throw std::logic_error("EventLoop dispatch is already active "
                               "(recursive or concurrent pump)");
      current_dispatch=&impl;
    }
    ~DispatchGuard() { current_dispatch=previous; impl.dispatching.clear(); }
  };
  explicit Impl(std::size_t capacity)
      : slots(std::min(capacity,std::size_t{64})), owner(TaskToken::next_owner()),
        reactor(std::min(capacity,std::size_t{64})) {
    if (capacity == 0)
      throw std::invalid_argument("EventLoop capacity must be positive");
    free.reserve(slots.size());
    io_active.reserve(slots.size());
    realtime_active.reserve(std::min(capacity,std::size_t{8}));
    io_waits.reserve(slots.size());
    reserve_indices(slots.size());
    realtime_timed.reserve_slots(slots.size());
    realtime_ready.reserve_slots(slots.size());
    realtime_timed.reserve_entries(1);
    realtime_ready.reserve_entries(1);
    realtime_free.reserve(1);
    realtime_free.push_back(std::make_unique<RealtimeOperation>());
    for (std::size_t i = slots.size(); i > 0; --i)
      free.push_back(i - 1);
    statistics.capacity = capacity;
  }
  bool ensure_free() {
    if (!free.empty()) return true;
    const auto old=slots.size();
    if (old>=statistics.capacity) return false;
    const auto additional=std::min(statistics.capacity-old,std::max(old,std::size_t{1}));
    const auto target=old+additional;
    // Reserve all storage needed to admit and later retire the new slots
    // before exposing any of them. Deque growth keeps existing slots stable.
    free.reserve(target);
    reserve_indices(target);
    slots.resize(target);
    for (auto i=target;i>old;--i) free.push_back(i-1);
    return true;
  }
  void reserve_indices(std::size_t count) {
    timed.reserve_slots(count);
    timed.reserve_entries(count);
  }
  template<class T> void reserve_pool(std::vector<T>& pool,std::size_t count) {
    if (count<=pool.capacity()) return;
    const auto current=pool.capacity();
    const auto growth=std::min(statistics.capacity-current,std::max(std::size_t{1},current));
    pool.reserve(std::max(count,current+growth));
  }
  void activate_io(std::size_t index) {
    auto &slot = slots[index];
    slot.io_position = io_active.size();
    slot.tracked_io = true;
    io_active.push_back(index);
    ++io_revision;
    ++io_probe_revision;
  }
  void activate_realtime(std::size_t index) {
    realtime_active.push_back(index);
  }
  void unschedule(std::size_t index) noexcept {
    auto& slot=slots[index];
    switch(slot.scheduled) {
      case Scheduled::timed: (void)timed.erase(index); break;
      case Scheduled::realtime_timed: (void)realtime_timed.erase(index); break;
      case Scheduled::high: (void)ready[0].erase(index); break;
      case Scheduled::normal: (void)ready[1].erase(index); break;
      case Scheduled::background: (void)ready[2].erase(index); break;
      case Scheduled::realtime: (void)realtime_ready.erase(index); break;
      case Scheduled::none: break;
    }
    slot.scheduled=Scheduled::none;
  }
  void schedule_ready(std::size_t index) {
    auto& slot=slots[index];
    unschedule(index);
    const auto priority=static_cast<std::size_t>(slot.priority);
    ready[priority].set(index,{Clock::time_point{},slot.sequence});
    slot.scheduled=static_cast<Scheduled>(static_cast<unsigned>(Scheduled::high)+priority);
  }
  void schedule(std::size_t index) {
    auto& slot=slots[index];
    unschedule(index);
    if (slot.status!=Status::queued) return;
    if (slot.realtime && slot.realtime->schedule.on_demand && !slot.has_wake) return;
    if (slot.signaled_task && !slot.has_wake) return;
    auto when=slot.deadline;
    if (slot.realtime && slot.realtime->schedule.on_demand) when=std::max(when,slot.wake_at);
    if (slot.realtime) {
      realtime_timed.set(index,{when,slot.sequence});
      slot.scheduled=Scheduled::realtime_timed;
      return;
    }
    if (slot.io) {
      if (slot.io->ready) { schedule_ready(index); return; }
      if (const auto retry=io_retries.find(index);
          retry!=io_retries.end() && retry->second!=Clock::time_point::min())
        when=std::min(when,retry->second);
      if (when==Clock::time_point::max()) return;
    }
    timed.set(index,{when,slot.sequence});
    slot.scheduled=Scheduled::timed;
  }
  void release(std::size_t index) {
    auto &slot = slots[index];
    unschedule(index);
    const bool was_realtime = bool(slot.realtime);
    if (was_realtime) {
      const auto position=std::find(realtime_active.begin(),realtime_active.end(),index);
      if (position==realtime_active.end()) std::terminate();
      *position=realtime_active.back();
      realtime_active.pop_back();
    }
    if (slot.tracked_io) {
      ++io_revision;
      if (!io_retries.empty()) io_retries.erase(index);
      const auto moved_io = io_active.back();
      io_active[slot.io_position] = moved_io;
      slots[moved_io].io_position = slot.io_position;
      io_active.pop_back();
      slot.tracked_io = false;
    }
    if (was_realtime) realtime_free.push_back(std::move(slot.realtime));
    else --normal_pending[static_cast<std::size_t>(slot.priority)];
    slot.status = Status::free;
    slot.deadline = {};
    slot.interval = {};
    slot.sequence = 0;
    slot.priority = Priority::normal;
    slot.cancelled = false;
    slot.signaled_task = false;
    slot.has_wake=false;
    free.push_back(index);
    --statistics.pending;
    if (!retirement_waiters.empty()) {
      const auto waiter=retirement_waiters.find(index);
      if (waiter!=retirement_waiters.end()) waiter->second->changed.notify_all();
    }
    if (was_realtime)
      statistics.reserved_utilization = static_cast<double>(utilization());
  }
  long double utilization() const {
    long double total = 0;
    for (const auto index : realtime_active) {
      const auto &slot = slots[index];
      if (slot.status != Status::free && slot.realtime)
        total += static_cast<long double>(slot.realtime->schedule.runtime.count()) /
                 slot.realtime->schedule.period.count();
    }
    return total;
  }
  std::optional<Clock::time_point> deadline() const {
    if (state!=LoopState::ready) return std::nullopt;
    if (realtime_ready.size() || ready[0].size() || ready[1].size() || ready[2].size())
      return Clock::now();
    const auto first=timed.front();
    const auto realtime_first=realtime_timed.front();
    if (first && realtime_first) return std::min(first->key.time,realtime_first->key.time);
    if (first) return first->key.time;
    if (realtime_first) return realtime_first->key.time;
    return std::nullopt;
  }
  std::size_t refresh_io(Clock::time_point now) {
    std::size_t became_ready = 0;
    io_next_check.reset();
    for (const auto index : io_active) {
      ++statistics.io_candidates;
      auto &slot = slots[index];
      if (slot.status != Status::queued || !slot.io)
        continue;
      auto &operation = *slot.io;
      const auto retry=io_retries.find(index);
      const auto previous_retry=retry==io_retries.end() ? Clock::time_point::min() : retry->second;
      const auto remember_next = [&] {
        if (operation.ready) return;
        auto candidate=slot.deadline;
        if (retry!=io_retries.end() && retry->second!=Clock::time_point::min())
          candidate=std::min(candidate,retry->second);
        if (candidate!=Clock::time_point::max() && (!io_next_check || candidate<*io_next_check))
          io_next_check=candidate;
      };
      if (operation.stop.cancelled()) {
        const bool newly_ready=!operation.ready;
        if (newly_ready) ++became_ready;
        operation.ready =
            std::unexpected(io::Error{io::ErrorCode::cancelled, {}});
        if (newly_ready) schedule(index);
        continue;
      }
      if (operation.ready)
        continue;
      if (retry!=io_retries.end() && now < retry->second) {
        if (slot.deadline <= now) {
          operation.ready = std::unexpected(io::Error{io::ErrorCode::timed_out, {}});
          ++became_ready;
          schedule(index);
        }
        remember_next();
        continue;
      }
      if (retry!=io_retries.end()) retry->second=Clock::time_point::min();
      auto ready = operation.empty_transfer()
                       ? io::Result<void>{}
                       : operation.channel.wait(operation.interest,
                                                std::chrono::milliseconds(0));
      if (ready || (ready.error().code != io::ErrorCode::would_block &&
                    ready.error().code != io::ErrorCode::busy)) {
        operation.ready = std::move(ready);
        ++became_ready;
      } else if (!operation.probe && slot.deadline>now && !operation.polling_interval &&
                 io::detail::needs_explicit_polling(operation.channel.capabilities())) {
        const auto error=ready.error().code==io::ErrorCode::busy ? io::ErrorCode::busy : io::ErrorCode::unsupported;
        operation.ready=std::unexpected(io::Error{error,ready.error().native});
        ++became_ready;
      } else if (slot.deadline <= now) {
        const auto error =
            operation.probe ? ready.error().code : io::ErrorCode::timed_out;
        operation.ready =
            std::unexpected(io::Error{error, ready.error().native});
        ++became_ready;
      } else if (retry!=io_retries.end())
        retry->second=add_saturated(now,*operation.polling_interval);
      if (operation.ready || (retry!=io_retries.end() && retry->second!=previous_retry))
        schedule(index);
      remember_next();
    }
    io_probed_revision=io_probe_revision;
    return became_ready;
  }
  void notify(bool io_change = false) {
    if (!host) changed.notify_all();
    if (io_change) {
      ++io_revision;
      if (host && host->monitor_io) io_changed.notify_all();
    }
    // A hosted loop's reactor belongs only to the I/O monitor; ordinary
    // posts already signal the external host and must not interrupt it.
    if (!host || (io_change && host->monitor_io)) reactor.wake();
  }
  void advance_realtime(std::size_t index,Clock::time_point now) {
    auto& slot=slots[index];
    if (now<slot.realtime->job_deadline) return;
    const auto position=core::Time::period(slot.deadline,slot.realtime->schedule.period,now);
    const bool expired=now>=core::Time::add(position.release,slot.realtime->schedule.deadline);
    const auto missed=position.skipped+static_cast<std::uint64_t>(expired);
    if (!slot.realtime->schedule.on_demand) {
      statistics.deadline_misses+=missed;
      statistics.skipped_releases+=missed;
      statistics.max_lateness=std::max(statistics.max_lateness,
                                       core::Time::elapsed(slot.realtime->job_deadline,now));
    }
    slot.deadline=expired ? position.next : position.release;
    slot.realtime->job_deadline=core::Time::add(slot.deadline,slot.realtime->schedule.deadline);
    slot.realtime->spent={};
    slot.sequence=++sequence;
  }
  void promote_realtime(Clock::time_point now) {
    while (const auto next=realtime_timed.front()) {
      if (next->key.time>now) break;
      const auto index=realtime_timed.take()->slot;
      auto& slot=slots[index];
      slot.scheduled=Scheduled::none;
      advance_realtime(index,now);
      if (slot.deadline<=now && now<slot.realtime->job_deadline &&
          (!slot.realtime->schedule.on_demand || (slot.has_wake && slot.wake_at<=now))) {
        realtime_ready.set(index,{slot.realtime->job_deadline,slot.sequence});
        slot.scheduled=Scheduled::realtime;
      } else schedule(index);
    }
  }
  void promote(Clock::time_point now) {
    while (const auto next=timed.front()) {
      if (next->key.time>now) break;
      const auto index=next->slot;
      auto& slot=slots[index];
      if (slot.io) {
        // A due I/O deadline/retry is resolved by its readiness contract.
        refresh_io(now);
        const auto remaining=timed.front();
        if (remaining && remaining->slot==index && remaining->key.time<=now)
          throw std::logic_error("due I/O readiness did not advance");
        continue;
      }
      (void)timed.take();
      slot.scheduled=Scheduled::none;
      schedule_ready(index);
    }
  }
  std::optional<std::size_t> select(Clock::time_point now) {
    for (;;) {
      promote_realtime(now);
      const auto candidate=realtime_ready.front();
      if (candidate && candidate->key.time<=now) {
        const auto index=realtime_ready.take()->slot;
        slots[index].scheduled=Scheduled::none;
        advance_realtime(index,now);
        schedule(index);
        continue;
      }
      if (candidate) {
        const auto index=realtime_ready.take()->slot;
        slots[index].scheduled=Scheduled::none;
        return index;
      }
      promote(now);
      const auto next_realtime=realtime_timed.front();
      if (next_realtime) {
        const auto current=Clock::now();
        if (next_realtime->key.time<=current) { now=current; continue; }
      }
      break;
    }
    for (std::size_t attempt=0;attempt<wheel.size();++attempt) {
      const auto priority=static_cast<std::size_t>(wheel[cursor]);
      cursor=(cursor+1)%wheel.size();
      if (const auto candidate=ready[priority].take()) {
        slots[candidate->slot].scheduled=Scheduled::none;
        return candidate->slot;
      }
    }
    return std::nullopt;
  }
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable io_changed;
  std::atomic_flag dispatching = ATOMIC_FLAG_INIT;
  std::deque<Slot> slots;
  std::vector<std::size_t> free;
  std::vector<std::size_t> io_active;
  std::vector<std::size_t> realtime_active;
  std::vector<std::unique_ptr<IOOperation>> io_free;
  std::vector<std::unique_ptr<RealtimeOperation>> realtime_free;
  std::array<std::size_t,3> normal_pending{};
  detail::EventLoopIndex timed,realtime_timed,realtime_ready;
  std::array<detail::EventLoopIndex,3> ready;
  std::unordered_map<std::size_t,std::unique_ptr<RetirementWait>> retirement_waiters;
  std::unordered_map<std::size_t,Clock::time_point> io_retries;
  const std::uint64_t owner;
  io::detail::ReadinessPoller reactor;
  std::vector<io::detail::WatchEntry> io_waits;
  std::uint64_t sequence = 0;
  std::size_t cursor = 0;
  LoopState state = LoopState::ready;
  LoopStats statistics;
  std::shared_ptr<HostLoop::State> host;
  std::uint64_t io_revision=0, io_probe_revision=0;
  std::uint64_t io_probed_revision=0;
  std::optional<Clock::time_point> io_next_check;
};

void HostLoop::State::monitor(EventLoop::Impl& events) {
  try {
    for (;;) {
      std::unique_lock lock(events.mutex);
      if (closing.load() || events.state==LoopState::stopped) return;
      const auto revision=events.io_revision;
      bool became_ready=false;
      events.io_waits.clear();
      std::optional<Clock::time_point> next;
      if (events.state==LoopState::ready) {
        const auto now=Clock::now();
        became_ready=events.refresh_io(now)>0;
        for (const auto index:events.io_active) {
          const auto& slot=events.slots[index];
          if (slot.status!=EventLoop::Impl::Status::queued || !slot.io) continue;
          if (slot.io->ready) continue;
          events.io_waits.push_back({slot.io->channel,slot.io->interest,slot.io->polling_interval});
          auto deadline=slot.deadline;
          if (const auto retry=events.io_retries.find(index);
              retry!=events.io_retries.end() && retry->second!=Clock::time_point::min())
            deadline=std::min(deadline,retry->second);
          if (deadline!=Clock::time_point::max() && (!next || deadline<*next)) next=deadline;
        }
      }
      if (became_ready) {
        lock.unlock(); signal(); lock.lock();
        if (closing.load() || events.state==LoopState::stopped) return;
        if (events.io_revision!=revision) continue;
      }
      if (events.io_waits.empty()) {
        events.io_changed.wait(lock,[&] { return closing.load() || events.state==LoopState::stopped || events.io_revision!=revision; });
      } else {
        auto timeout=std::chrono::milliseconds::max();
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

void HostLoop::State::start_monitor(EventLoop::Impl& events) {
  if (waiter.joinable()) return;
  const auto started=waiter.start([state=this,loop=&events](ThreadContext&) { state->monitor(*loop); });
  if (!started)
    throw std::runtime_error("HostLoop cannot start its native readiness waiter; disable monitor_io for a manually driven host");
  std::lock_guard lock(mutex);
  io_waiter_active=true;
}

std::shared_ptr<HostLoop::State> HostLoop::connect(EventLoop& loop,Task wake,HostLoopOptions options) {
  if (!wake) throw std::invalid_argument("HostLoop requires a wake callback");
  auto state=std::make_shared<State>();
  state->loop=loop.impl_; state->wake=std::move(wake); state->monitor_io=options.monitor_io;
  {
    std::lock_guard lock(loop.impl_->mutex);
    if (loop.impl_->host || loop.impl_->dispatching.test() || loop.impl_->state==LoopState::stopped)
      throw std::logic_error("HostLoop requires an idle, live, unbound EventLoop");
    loop.impl_->host=state;
    try {
      if (options.monitor_io && !loop.impl_->io_active.empty()) state->start_monitor(*loop.impl_);
    } catch (...) {
      loop.impl_->host.reset();
      throw;
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
    events->notify(true);
  } else state->closing.store(true);
  Task retired;
  {
    std::unique_lock lock(state->mutex);
    state->idle.wait(lock,[&]{return state->active==0;});
    retired.swap(state->wake);
    state->statistics.connected=false;
  }
  const auto joined=state->waiter.join();
  { std::lock_guard lock(state->mutex); state->io_waiter_active=false; }
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
bool HostLoop::io_waiter_active() const { std::lock_guard lock(state_->mutex); return state_->io_waiter_active; }
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
                             Priority priority, bool signaled_task) {
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
  if (!impl_->ensure_free())
    return reject(SubmitError::full);
  const auto priority_index=static_cast<std::size_t>(priority);
  impl_->ready[priority_index].reserve_slots(impl_->slots.size());
  impl_->ready[priority_index].reserve_entries(impl_->normal_pending[priority_index]+1);
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
  slot.signaled_task = signaled_task;
  slot.status = Impl::Status::queued;
  ++impl_->normal_pending[priority_index];
  impl_->schedule(index);
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
  token.wait_ = &EventLoop::wait_impl;
  if (!signaled_task) impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host && !signaled_task) host->signal();
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
Submission EventLoop::on_signal(Task task,Priority priority) {
  return submit(impl_,Clock::time_point::max(),{},std::move(task),priority,true);
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
  if (!impl_->ensure_free())
    return reject(SubmitError::full);
  const auto utilization = impl_->utilization() +
                           static_cast<long double>(schedule.runtime.count()) /
                               schedule.period.count();
  if (utilization > 1.0L)
    return reject(SubmitError::overloaded);
  impl_->realtime_timed.reserve_slots(impl_->slots.size());
  impl_->realtime_ready.reserve_slots(impl_->slots.size());
  impl_->realtime_timed.reserve_entries(impl_->realtime_active.size()+1);
  impl_->realtime_ready.reserve_entries(impl_->realtime_active.size()+1);
  if (impl_->realtime_active.size()==impl_->realtime_active.capacity()) {
    const auto count=impl_->realtime_active.size();
    const auto extra=std::min(impl_->slots.size()-count,std::max(std::size_t{1},count));
    impl_->realtime_active.reserve(count+extra);
  }
  std::unique_ptr<Impl::RealtimeOperation> operation;
  if (impl_->realtime_free.empty()) {
    impl_->reserve_pool(impl_->realtime_free,impl_->realtime_active.size()+1);
    operation=std::make_unique<Impl::RealtimeOperation>();
  } else {
    operation=std::move(impl_->realtime_free.back());
    impl_->realtime_free.pop_back();
  }
  const auto index = impl_->free.back();
  impl_->free.pop_back();
  auto &slot = impl_->slots[index];
  operation->schedule=schedule;
  operation->slice.swap(task);
  slot.realtime=std::move(operation);
  impl_->activate_realtime(index);
  slot.deadline = schedule.start.value_or(Clock::now());
  slot.realtime->job_deadline = core::Time::add(slot.deadline, schedule.deadline);
  slot.realtime->spent = {};
  slot.sequence = ++impl_->sequence;
  ++slot.generation;
  slot.cancelled = false;
  slot.status = Impl::Status::queued;
  impl_->schedule(index);
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
  token.wait_ = &EventLoop::wait_impl;
  if (!schedule.on_demand) impl_->notify();
  auto host=impl_->host; lock.unlock(); if (host && !schedule.on_demand) host->signal();
  return {token, SubmitError::none};
}

Submission
EventLoop::submit_io(const std::shared_ptr<Impl>& impl_,io::Channel channel, io::Interest interest, IOReady ready,
                     IOCompletion completion,
                     std::variant<std::monostate, io::Read, io::Write> request,
                     IOWatchOptions options) {
  // Register before taking the loop lock: an already-cancelled token invokes
  // its callback synchronously. The callback can then safely notify the host.
  std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
  if (options.stop.native_token().stop_possible()) {
    const std::weak_ptr<Impl> weak=impl_;
    cancellation=std::make_unique<std::stop_callback<std::function<void()>>>(
        options.stop.native_token(),[weak] {
          if (const auto state=weak.lock()) {
            std::shared_ptr<HostLoop::State> host;
            {
              std::lock_guard guard(state->mutex);
              host=state->host;
              state->notify(true);
              ++state->io_probe_revision;
              state->reactor.wake_persistent();
              // Prevent a new readiness epoch while the old native leases
              // retire; manually driven hosts have no outstanding epoch.
              if (host) state->reactor.synchronize();
            }
            if (host) host->signal();
          }
        });
  }
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
      !io::detail::valid_polling_interval(options.polling_interval) ||
      !valid_request || (request.index() == 0 ? !ready : !completion))
    return reject(SubmitError::invalid);
  if (impl_->state == LoopState::stopped)
    return reject(SubmitError::stopped);
  if (!impl_->ensure_free())
    return reject(SubmitError::full);
  if (impl_->host && impl_->host->monitor_io) impl_->host->start_monitor(*impl_);
  const auto priority_index=static_cast<std::size_t>(options.priority);
  impl_->ready[priority_index].reserve_slots(impl_->slots.size());
  impl_->ready[priority_index].reserve_entries(impl_->normal_pending[priority_index]+1);
  impl_->reserve_pool(impl_->io_active,impl_->io_active.size()+1);
  std::unique_ptr<Impl::IOOperation> payload;
  if (impl_->io_free.empty()) {
    impl_->reserve_pool(impl_->io_free,impl_->io_active.size()+1);
    payload=std::make_unique<Impl::IOOperation>();
  } else {
    payload=std::move(impl_->io_free.back());
    impl_->io_free.pop_back();
  }
  const auto index = impl_->free.back();
  if (options.polling_interval && io::detail::needs_explicit_polling(channel.capabilities()))
    impl_->io_retries.emplace(index,Clock::time_point::min());
  impl_->free.pop_back();
  auto &slot = impl_->slots[index];
  slot.io=std::move(payload);
  auto &operation=*slot.io;
  impl_->activate_io(index);
  operation.channel = std::move(channel);
  operation.interest = interest;
  operation.ready_callback.swap(ready);
  operation.completion.swap(completion);
  operation.request = std::move(request);
  operation.stop = std::move(options.stop);
  operation.polling_interval = options.polling_interval;
  operation.cancellation=std::move(cancellation);
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
  ++impl_->normal_pending[priority_index];
  impl_->schedule(index);
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
  token.wait_ = &EventLoop::wait_impl;
  impl_->notify(true);
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
                         priority, request.polling_interval};
  return submit_io(impl_,std::move(channel), io::Interest::readable, {},
                   std::move(completion), std::move(request),
                   std::move(options));
}
Submission EventLoop::write(io::Channel channel, io::Write request,
                            IOCompletion completion, Priority priority) {
  IOWatchOptions options{transfer_timeout(request.timeout), request.stop,
                         priority, request.polling_interval};
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
bool EventLoop::Sender::wake_realtime(TaskToken token,Clock::time_point when) const {
  const auto state=state_.lock();
  return state && EventLoop::wake_realtime_impl(state,token,when);
}
Submission EventLoop::Sender::on_signal(Task task,Priority priority) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit(state,Clock::time_point::max(),{},std::move(task),priority,true) : Submission{{},SubmitError::stopped};
}
bool EventLoop::Sender::signal(TaskToken token,Clock::time_point when) const {
  const auto state=state_.lock();
  return state && EventLoop::signal_impl(state,token,when);
}
Submission EventLoop::Sender::watch(io::Channel channel,io::Interest interest,IOReady ready,IOWatchOptions options) const {
  const auto state=state_.lock();
  return state ? EventLoop::submit_io(state,std::move(channel),interest,std::move(ready),{},std::monostate{},std::move(options)) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::read(io::Channel channel,io::Read request,IOCompletion completion,Priority priority) const {
  const auto state=state_.lock();
  IOWatchOptions options{transfer_timeout(request.timeout),request.stop,priority,request.polling_interval};
  return state ? EventLoop::submit_io(state,std::move(channel),io::Interest::readable,{},std::move(completion),std::move(request),std::move(options)) : Submission{{},SubmitError::stopped};
}
Submission EventLoop::Sender::write(io::Channel channel,io::Write request,IOCompletion completion,Priority priority) const {
  const auto state=state_.lock();
  IOWatchOptions options{transfer_timeout(request.timeout),request.stop,priority,request.polling_interval};
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
bool EventLoop::wait_impl(void* owner,const TaskToken& token) {
  auto& impl=*static_cast<Impl*>(owner);
  std::unique_lock lock(impl.mutex);
  const auto retired=[&] {
    return token.owner_!=impl.owner || token.slot_>=impl.slots.size() ||
      impl.slots[token.slot_].generation!=token.generation_ ||
      impl.slots[token.slot_].status==Impl::Status::free;
  };
  if(retired()) return true;
  if(current_dispatch==&impl || HostLoop::is_current_thread()) return false;
#if defined(__EMSCRIPTEN__)
  if(emscripten_is_main_browser_thread()) return false;
#endif
  auto found=impl.retirement_waiters.find(token.slot_);
  if (found==impl.retirement_waiters.end()) {
    auto state=std::make_unique<Impl::RetirementWait>();
    found=impl.retirement_waiters.emplace(token.slot_,std::move(state)).first;
  }
  auto& state=*found->second;
  ++state.count;
  struct Waiter final {
    Impl& impl; std::size_t index; Impl::RetirementWait& state;
    ~Waiter() { if (--state.count==0) impl.retirement_waiters.erase(index); }
  } waiter{impl,token.slot_,state};
  state.changed.wait(lock,retired);
  return true;
}
bool EventLoop::wake_realtime_impl(const std::shared_ptr<Impl>& impl,TaskToken token,Clock::time_point when) {
  std::unique_lock lock(impl->mutex);
  if(token.owner_!=impl->owner || token.slot_>=impl->slots.size()) return false;
  auto& slot=impl->slots[token.slot_];
  if(impl->state==LoopState::stopped || slot.generation!=token.generation_ || slot.cancelled ||
     (slot.status!=Impl::Status::queued && slot.status!=Impl::Status::executing) ||
     !slot.realtime || !slot.realtime->schedule.on_demand) return false;
  if(slot.has_wake && slot.wake_at<=when) return true;
  slot.wake_at=when;
  slot.has_wake=true;
  if(slot.status==Impl::Status::queued) impl->schedule(token.slot_);
  impl->notify();
  auto host=impl->host; lock.unlock(); if(host) host->signal();
  return true;
}
bool EventLoop::signal_impl(const std::shared_ptr<Impl>& impl,TaskToken token,Clock::time_point when) {
  std::unique_lock lock(impl->mutex);
  if(token.owner_!=impl->owner || token.slot_>=impl->slots.size()) return false;
  auto& slot=impl->slots[token.slot_];
  if(impl->state==LoopState::stopped || slot.generation!=token.generation_ || slot.cancelled || !slot.signaled_task ||
     (slot.status!=Impl::Status::queued && slot.status!=Impl::Status::executing)) return false;
  if(slot.has_wake && slot.wake_at<=when) return true;
  slot.wake_at=when;
  slot.has_wake=true;
  if(slot.status==Impl::Status::queued) { slot.deadline=slot.wake_at; impl->schedule(token.slot_); }
  impl->notify();
  auto host=impl->host; lock.unlock(); if(host) host->signal();
  return true;
}
bool EventLoop::cancel_impl(void *owner, const TaskToken &token) {
  auto &impl = *static_cast<Impl *>(owner);
  Task retired;
  SliceTask retired_slice;
  std::unique_ptr<Impl::IOOperation> retired_io;
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
    const bool was_io=slot.tracked_io;
    slot.cancelled = true;
    impl.unschedule(token.slot_);
    if (slot.tracked_io) ++impl.io_probe_revision;
    ++impl.statistics.cancelled;
    if (slot.status == Impl::Status::queued) {
      retired.swap(slot.task);
      if (slot.realtime) retired_slice.swap(slot.realtime->slice);
      retired_io.swap(slot.io);
      slot.status = Impl::Status::retiring;
      queued = true;
    }
    impl.notify(was_io);
    host=impl.host;
  }
  if (retired_io)
    impl.reactor.synchronize();
  retired =
      nullptr; // finish resource destruction before exposing the free slot
  retired_slice = nullptr;
  if (retired_io) { Impl::IOOperation released; retired_io->swap(released); }
  if (queued) {
    std::lock_guard lock(impl.mutex);
    if (retired_io) impl.io_free.push_back(std::move(retired_io));
    impl.release(token.slot_);
  }
  if (host) host->signal();
  return true;
}

std::size_t EventLoop::dispatch(PumpBudget budget) {
  const auto end = add_saturated(Clock::now(), budget.max_time);
  std::size_t executed = 0, attempts = 0;
  bool manually_checked_io=false;
  // A positive budget permits one callback even if bookkeeping consumed its
  // entire duration; otherwise a due task can spin without ever starting.
  while (attempts < budget.max_callbacks &&
         ((attempts == 0 && budget.max_time > Clock::duration::zero()) || Clock::now() < end)) {
    Task task;
    SliceTask slice;
    std::optional<RealtimeSchedule> realtime;
    core::TimeSlice window;
    std::unique_ptr<Impl::IOOperation> operation;
    Clock::time_point deadline;
    std::size_t index;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->state != LoopState::ready)
        break;
      const auto now=Clock::now();
      const bool monitored=impl_->host && impl_->host->monitor_io;
      if ((!monitored && !manually_checked_io) ||
          impl_->io_probed_revision!=impl_->io_probe_revision ||
          (impl_->io_next_check && now>=*impl_->io_next_check)) {
        impl_->refresh_io(now);
      }
      manually_checked_io=true;
      const auto chosen=impl_->select(Clock::now());
      if (!chosen)
        break;
      index = *chosen;
      auto &slot = impl_->slots[index];
      slot.status = Impl::Status::executing;
      if ((slot.realtime && slot.realtime->schedule.on_demand) || slot.signaled_task) slot.has_wake=false;
      // std::function's moved-from state is unspecified: swap with an
      // empty function so reused slots never retain callback captures.
      task.swap(slot.task);
      if (slot.realtime) {
        slice.swap(slot.realtime->slice);
        realtime=slot.realtime->schedule;
      }
      if (realtime) {
        const auto start = Clock::now();
        const auto remaining = realtime->runtime - slot.realtime->spent;
        const auto pump_end = attempts == 0 && end <= start
            ? core::Time::add(start,std::min(remaining,realtime->quantum)) : end;
        window = {
            slot.deadline, slot.realtime->job_deadline, start,
            std::min({pump_end, slot.realtime->job_deadline,
                      core::Time::add(start,
                                      std::min(remaining, realtime->quantum))}),
            remaining};
      }
      operation.swap(slot.io);
      if (operation) ++impl_->io_revision;
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
          if (now < deadline && !operation->polling_interval &&
              io::detail::needs_explicit_polling(operation->channel.capabilities())) {
            if(reply.error().code==io::ErrorCode::would_block)
              reply=std::unexpected(io::Error{io::ErrorCode::unsupported,reply.error().native});
          } else if (now < deadline) {
            std::lock_guard lock(impl_->mutex);
            auto &slot = impl_->slots[index];
            if (!slot.cancelled && impl_->state != LoopState::stopped) {
              operation->ready.reset();
              if (const auto retry=impl_->io_retries.find(index); retry!=impl_->io_retries.end())
                retry->second=add_saturated(now,*operation->polling_interval);
              slot.io.swap(operation);
              slot.sequence = ++impl_->sequence;
              slot.status = Impl::Status::queued;
              impl_->schedule(index);
              ++impl_->io_revision;
              ++impl_->io_probe_revision;
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
        slot.realtime->spent = core::Time::add(Clock::time_point(slot.realtime->spent), elapsed)
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
          if ((!realtime->on_demand && result != SliceResult::yield) || slot.realtime->spent >= realtime->runtime ||
              finished >= window.deadline) {
            if (result == SliceResult::yield && slot.realtime->spent >= realtime->runtime)
              ++impl_->statistics.budget_exhaustions;
            const auto position =
                core::Time::period(slot.deadline, realtime->period, finished);
            impl_->statistics.skipped_releases += position.skipped;
            slot.deadline = position.next;
            slot.realtime->job_deadline =
                core::Time::add(slot.deadline, realtime->deadline);
            slot.realtime->spent = {};
          }
          if (realtime->on_demand && result==SliceResult::yield &&
              (!slot.has_wake || finished<slot.wake_at)) { slot.wake_at=finished; slot.has_wake=true; }
          slot.realtime->slice.swap(slice);
          slot.sequence = ++impl_->sequence;
          slot.status = Impl::Status::queued;
        } else
          slot.status = Impl::Status::retiring;
      } else if (!failure && !slot.cancelled && slot.signaled_task &&
                 impl_->state != LoopState::stopped) {
        slot.deadline=slot.has_wake ? slot.wake_at : Clock::time_point::max();
        slot.task.swap(task);
        slot.sequence=++impl_->sequence;
        slot.status=Impl::Status::queued;
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
      if (slot.status==Impl::Status::queued) impl_->schedule(index);
    }
    // Non-periodic captures must retire before their slot can be reused.
    if (task || slice || operation) {
      task = nullptr;
      slice = nullptr;
      if (operation) { Impl::IOOperation released; operation->swap(released); }
      std::lock_guard lock(impl_->mutex);
      if (operation) impl_->io_free.push_back(std::move(operation));
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
  std::uint64_t io_revision;
  { std::lock_guard lock(impl_->mutex); host=impl_->host; io_revision=impl_->io_revision; }
  if (host) host->acknowledge();
  struct Changed final {
    Impl& state;
    std::uint64_t io_revision;
    ~Changed() { std::lock_guard lock(state.mutex); if (state.io_revision!=io_revision) state.notify(true); }
  } changed{*impl_,io_revision};
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
      for (const auto index : impl_->io_active) {
        const auto &slot = impl_->slots[index];
        if (slot.status == Impl::Status::queued && slot.io && !slot.io->ready)
          impl_->io_waits.push_back({slot.io->channel, slot.io->interest, slot.io->polling_interval});
      }
    }
    if (!impl_->io_waits.empty()) {
      auto timeout = std::chrono::milliseconds::max();
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
  if (impl_->state != LoopState::ready) return;
  impl_->state = LoopState::suspended;
  impl_->notify(true);
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
}
void EventLoop::resume() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state != LoopState::suspended) return;
  impl_->state = LoopState::ready;
  impl_->notify(true);
  auto host=impl_->host; lock.unlock(); if (host) host->signal();
}
void EventLoop::request_stop() {
  std::shared_ptr<HostLoop::State> host;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == LoopState::stopped) return;
    impl_->state = LoopState::stopped;
    impl_->notify(true);
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
