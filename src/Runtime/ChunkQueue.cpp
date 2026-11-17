#include "ChunkQueue.h"
#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ii::runtime {
namespace detail {
struct ChunkTaskState final {
    ChunkWork work;
    ChunkSnapshot view;
    std::stop_source stop;
    bool paused=false,cancelled=false,failed=false,terminal=false;
};
struct ChunkQueueState final {
    explicit ChunkQueueState(ChunkQueueOptions value):options(std::move(value)),slots(options.capacity) {}
    ChunkQueueOptions options;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<ChunkTaskState>> slots;
    bool closing=false;
};
}
namespace {
using detail::ChunkQueueState;
using detail::ChunkTaskState;
using core::ErrorCode;
thread_local ChunkQueueState* callback_queue=nullptr;
auto failure(ErrorCode code,const char* text) { return core::fail(code,text); }
struct CallbackScope final {
    ChunkQueueState* previous;
    explicit CallbackScope(ChunkQueueState* current):previous(callback_queue) { callback_queue=current; }
    ~CallbackScope() { callback_queue=previous; }
};
void update(ChunkQueueState& q,const std::shared_ptr<ChunkTaskState>& task) {
    auto& v=task->view;
    const auto active=v.cpu_active+v.device_active+v.interrupts_active;
    if(task->failed || task->cancelled) {
        v.phase=active?ChunkPhase::cancelling:(task->failed?ChunkPhase::failed:ChunkPhase::cancelled);
    } else if(v.claimed_items==v.total_items && !active) v.phase=ChunkPhase::completed;
    else if(task->paused) v.phase=active?ChunkPhase::pausing:ChunkPhase::paused;
    else v.phase=v.claimed_items?ChunkPhase::running:ChunkPhase::queued;
    const bool done=v.phase==ChunkPhase::completed || v.phase==ChunkPhase::cancelled || v.phase==ChunkPhase::failed;
    if(done && !task->terminal) {
        task->terminal=true;
        for(auto& slot:q.slots) if(slot==task) { slot.reset();break; }
    }
    ++v.revision;
    q.changed.notify_all();
}
core::Error exception_error() {
    try { throw; }
    catch(const std::exception& e) { return {ErrorCode::failed,e.what(),std::current_exception()}; }
    catch(...) { return {ErrorCode::failed,"Chunk callback threw",std::current_exception()}; }
}
void run_lane(std::shared_ptr<ChunkQueueState> q,std::optional<std::size_t> device) {
    CallbackScope scope(q.get());
    std::size_t cursor=0;
    for(;;) {
        std::shared_ptr<ChunkTaskState> task;
        ChunkRange range;
        ChunkDispatch dispatch;
        {
            std::unique_lock lock(q->mutex);
            auto available=[&] {
                if(q->closing)return true;
                for(const auto& item:q->slots)
                    if(item && !item->paused && !item->cancelled && !item->failed &&
                       item->view.claimed_items<item->view.total_items &&
                       (device?bool(item->work.device):bool(item->work.cpu)))return true;
                return false;
            };
            q->changed.wait(lock,available);
            if(q->closing)return;
            for(std::size_t n=0;n<q->slots.size();++n) {
                const auto i=(cursor+n)%q->slots.size();
                const auto& candidate=q->slots[i];
                if(candidate && !candidate->paused && !candidate->cancelled && !candidate->failed &&
                   candidate->view.claimed_items<candidate->view.total_items &&
                   (device?bool(candidate->work.device):bool(candidate->work.cpu))) {
                    task=candidate;cursor=(i+1)%q->slots.size();break;
                }
            }
            if(!task)continue;
            auto count=std::min<std::uint64_t>(task->work.chunk_items,task->view.total_items-task->view.claimed_items);
            if(device) {
                const auto lane=q->options.devices[*device];
                count=std::min<std::uint64_t>(count,std::uint64_t(lane.group_size)*lane.max_groups);
                dispatch.group_size=lane.group_size;
                dispatch.groups=static_cast<std::uint32_t>((count+lane.group_size-1)/lane.group_size);
                ++task->view.device_active;
            } else ++task->view.cpu_active;
            range={task->view.claimed_items,static_cast<std::uint32_t>(count)};
            dispatch.range=range;
            task->view.claimed_items+=count;
            update(*q,task);
        }
        core::Result<void> result;
        bool submitted=false;
        try {
            if(!device) result=task->work.cpu(range,task->stop.get_token());
            else {
                auto pending=task->work.device(*device,dispatch,task->stop.get_token());
                if(!pending) result=std::unexpected(pending.error());
                else if(!pending->wait) result=failure(ErrorCode::invalid_argument,"A submitted device operation requires a completion wait");
                else {
                    {
                        std::lock_guard lock(q->mutex);
                        ++task->view.device_submitted;++task->view.device_in_flight;submitted=true;
                        update(*q,task);
                    }
                    result=pending->wait();
                    bool collect=false;
                    {
                        std::lock_guard lock(q->mutex);
                        --task->view.device_in_flight;submitted=false;
                        collect=!task->cancelled && !task->failed;
                        update(*q,task);
                    }
                    if(result && collect && pending->collect) result=pending->collect();
                }
            }
        } catch(...) { result=std::unexpected(exception_error()); }
        bool stop=false;
        {
            std::lock_guard lock(q->mutex);
            if(device) {--task->view.device_active;if(submitted)--task->view.device_in_flight;}
            else --task->view.cpu_active;
            if(!result && !task->cancelled && !task->failed) {
                task->failed=true;task->view.error=result.error();stop=true;
            } else if(result && !task->cancelled && !task->failed) {
                task->view.completed_items+=range.count;++task->view.completed_chunks;
            }
            update(*q,task);
        }
        if(stop) { CallbackScope callback(q.get());task->stop.request_stop(); }
    }
}
}
struct ChunkQueue::Impl final {
    std::shared_ptr<ChunkQueueState> state;
    std::vector<std::thread> workers;
    std::mutex lifecycle;
    bool joined=false;
    explicit Impl(ChunkQueueOptions options) {
        if(!options.capacity || (!options.cpu_workers && options.devices.empty()) || !options.max_concurrent_interrupts)
            throw std::invalid_argument("Chunk queue needs bounded capacity, lanes, and interrupt capacity");
        for(const auto& lane:options.devices) if(!lane.group_size || !lane.max_groups)
            throw std::invalid_argument("Device group size and group limit must be positive");
        state=std::make_shared<ChunkQueueState>(std::move(options));
        try {
            for(std::size_t i=0;i<state->options.cpu_workers;++i)workers.emplace_back(run_lane,state,std::nullopt);
            for(std::size_t i=0;i<state->options.devices.size();++i)workers.emplace_back(run_lane,state,i);
        } catch(...) {
            {std::lock_guard lock(state->mutex);state->closing=true;}
            state->changed.notify_all();
            for(auto& worker:workers)if(worker.joinable())worker.join();
            throw;
        }
    }
};
ChunkTicket::ChunkTicket(std::shared_ptr<ChunkQueueState> q,std::shared_ptr<ChunkTaskState> t):queue_(std::move(q)),task_(std::move(t)) {}
ChunkSnapshot ChunkTicket::snapshot() const {
    if(!queue_ || !task_)return {};
    std::lock_guard lock(queue_->mutex);return task_->view;
}
ChunkSnapshot ChunkTicket::wait_for_change(std::uint64_t revision,std::chrono::milliseconds timeout) const {
    if(!queue_ || !task_)return {};
    std::unique_lock lock(queue_->mutex);
    if(timeout.count()>0)queue_->changed.wait_for(lock,timeout,[&]{return task_->view.revision!=revision;});
    return task_->view;
}
core::Result<void> ChunkTicket::pause() const {
    if(!queue_ || !task_)return failure(ErrorCode::closed,"No task");
    std::lock_guard lock(queue_->mutex);
    if(task_->terminal || task_->cancelled || task_->failed)return failure(ErrorCode::closed,"Task is terminating");
    task_->paused=true;update(*queue_,task_);return {};
}
core::Result<void> ChunkTicket::resume() const {
    if(!queue_ || !task_)return failure(ErrorCode::closed,"No task");
    std::lock_guard lock(queue_->mutex);
    if(task_->terminal || task_->cancelled || task_->failed)return failure(ErrorCode::closed,"Task is terminating");
    task_->paused=false;update(*queue_,task_);return {};
}
core::Result<void> ChunkTicket::cancel() const {
    if(!queue_ || !task_)return failure(ErrorCode::closed,"No task");
    {
        std::lock_guard lock(queue_->mutex);
        if(task_->terminal)return failure(ErrorCode::closed,"Task is terminal");
        task_->cancelled=true;update(*queue_,task_);
    }
    { CallbackScope callback(queue_.get());task_->stop.request_stop(); }
    return {};
}
core::Result<void> ChunkTicket::interrupt(std::uint64_t type,std::span<const std::byte> bytes) const {
    if(!queue_ || !task_)return failure(ErrorCode::closed,"No task");
    {
        std::lock_guard lock(queue_->mutex);
        if(task_->terminal || task_->cancelled || task_->failed)return failure(ErrorCode::closed,"Task is terminating");
        if(!task_->work.interrupt)return failure(ErrorCode::unsupported,"Task has no interrupt handler");
        if(task_->view.interrupts_active>=queue_->options.max_concurrent_interrupts)
            return failure(ErrorCode::full,"Task interrupt capacity exhausted");
        ++task_->view.interrupts_active;update(*queue_,task_);
    }
    core::Result<void> result;
    { CallbackScope scope(queue_.get());
      try {result=task_->work.interrupt(type,bytes);}catch(...) {result=std::unexpected(exception_error());} }
    {
        std::lock_guard lock(queue_->mutex);
        --task_->view.interrupts_active;update(*queue_,task_);
    }
    return result;
}
ChunkQueue::ChunkQueue(ChunkQueueOptions options):impl_(std::make_unique<Impl>(std::move(options))) {}
ChunkQueue::~ChunkQueue() { if(!close())std::terminate(); }
core::Result<ChunkTicket> ChunkQueue::submit(ChunkWork work) {
    if(!work.items || !work.chunk_items || (!work.cpu && !work.device))
        return failure(ErrorCode::invalid_argument,"Work requires items, a chunk size, and an implementation");
    auto q=impl_->state;
    if((work.cpu && !q->options.cpu_workers) || (work.device && q->options.devices.empty()))
        return failure(ErrorCode::unsupported,"Work implementation has no matching execution lane");
    auto task=std::make_shared<ChunkTaskState>();
    task->work=std::move(work);task->view.total_items=task->work.items;
    {
        std::lock_guard lock(q->mutex);
        if(q->closing)return failure(ErrorCode::closed,"Chunk queue is closing");
        auto slot=std::find(q->slots.begin(),q->slots.end(),nullptr);
        if(slot==q->slots.end())return failure(ErrorCode::full,"Chunk queue capacity exhausted");
        *slot=task;update(*q,task);
    }
    return ChunkTicket(q,std::move(task));
}
core::Result<void> ChunkQueue::close() {
    auto q=impl_->state;
    if(callback_queue==q.get())return failure(ErrorCode::busy,"A queue callback cannot close its own queue");
    std::unique_lock life(impl_->lifecycle,std::try_to_lock);
    if(!life.owns_lock())return failure(ErrorCode::busy,"Chunk queue is already closing");
    if(impl_->joined)return {};
    std::vector<std::shared_ptr<ChunkTaskState>> tasks;
    {
        std::lock_guard lock(q->mutex);
        q->closing=true;
        for(auto& task:q->slots)if(task)tasks.push_back(task);
        for(auto& task:tasks) {task->cancelled=true;update(*q,task);}
        q->changed.notify_all();
    }
    { CallbackScope callback(q.get());for(auto& task:tasks)task->stop.request_stop(); }
    for(auto& worker:impl_->workers)if(worker.joinable())worker.join();
    {
        std::unique_lock lock(q->mutex);
        q->changed.wait(lock,[&]{return std::all_of(q->slots.begin(),q->slots.end(),[](const auto& task){return !task;});});
    }
    impl_->joined=true;return {};
}
} // namespace ii::runtime
