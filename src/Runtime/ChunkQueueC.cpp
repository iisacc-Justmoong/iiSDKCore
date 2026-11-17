#include "ChunkQueueC.h"
#include "ChunkQueue.h"
#include "ResourceControlC.h"
#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>
using namespace ii::runtime;
struct iisdkcore_chunk_queue final { explicit iisdkcore_chunk_queue(ChunkQueueOptions o):queue(std::move(o)){} ChunkQueue queue; };
struct iisdkcore_chunk_task final { ChunkTicket ticket; };
namespace {
int error(const ii::core::Error& e) {
    using ii::core::ErrorCode;
    switch(e.code) {
        case ErrorCode::invalid_argument:case ErrorCode::out_of_range:return IISDKCORE_RESOURCE_INVALID;
        case ErrorCode::full:return IISDKCORE_RESOURCE_LIMIT;
        case ErrorCode::closed:return IISDKCORE_RESOURCE_CLOSED;
        case ErrorCode::busy:return IISDKCORE_RESOURCE_BUSY;
        case ErrorCode::unsupported:return IISDKCORE_RESOURCE_UNSUPPORTED;
        default:return IISDKCORE_RESOURCE_FAILED;
    }
}
template<class F> int protect(F&& f) noexcept {
    try{return f();}catch(const std::bad_alloc&){return IISDKCORE_RESOURCE_OUT_OF_MEMORY;}
    catch(const std::invalid_argument&){return IISDKCORE_RESOURCE_INVALID;}catch(...){return IISDKCORE_RESOURCE_FAILED;}
}
int status(ii::core::Result<void> value) { return value?IISDKCORE_RESOURCE_OK:error(value.error()); }
ii::core::Result<void> callback_result(int value) {
    return value==0?ii::core::Result<void>{}:ii::core::fail(ii::core::ErrorCode::failed,"Foreign chunk callback failed");
}
void snapshot(const ChunkSnapshot& s,iisdkcore_chunk_snapshot& o) {
    o={static_cast<int>(s.phase),s.revision,s.total_items,s.claimed_items,s.completed_items,
       s.completed_chunks,s.device_submitted,s.cpu_active,s.device_active,s.device_in_flight,s.interrupts_active};
}
struct DeviceOperation final {
    iisdkcore_chunk_callbacks callbacks;
    uintptr_t token=0;
    bool submitted=false,waited=false;
    ~DeviceOperation() {
        if(submitted) {
            // Allocation of a completion callback can fail after submission.
            // Retain the foreign operation and wait before releasing it.
            if(!waited)(void)callbacks.wait(callbacks.context,token);
            callbacks.release(callbacks.context,token);
        }
    }
};
}
extern "C" {
int iisdkcore_chunk_queue_create(size_t workers,size_t capacity,const iisdkcore_chunk_lane* lanes,size_t count,iisdkcore_chunk_queue** out) {
    return protect([&]{
        if(!out || (!lanes && count))return int(IISDKCORE_RESOURCE_INVALID);
        *out=nullptr;ChunkQueueOptions options;options.cpu_workers=workers;options.capacity=capacity;
        for(size_t i=0;i<count;++i)options.devices.push_back({lanes[i].group_size,lanes[i].max_groups});
        *out=new iisdkcore_chunk_queue(std::move(options));return int(IISDKCORE_RESOURCE_OK);
    });
}
int iisdkcore_chunk_queue_destroy(iisdkcore_chunk_queue* q) {
    return protect([&]{if(!q)return int(IISDKCORE_RESOURCE_INVALID);auto result=q->queue.close();
        if(!result)return error(result.error());delete q;return int(IISDKCORE_RESOURCE_OK);});
}
int iisdkcore_chunk_submit_work(iisdkcore_chunk_queue* q,uint64_t items,uint32_t chunk,const iisdkcore_chunk_callbacks* cb,iisdkcore_chunk_task** out) {
    return protect([&]{
        if(!q || !cb || !out || (cb->submit && (!cb->wait || !cb->release)))return int(IISDKCORE_RESOURCE_INVALID);
        *out=nullptr;const auto callbacks=*cb;
        auto task=std::make_unique<iisdkcore_chunk_task>();
        ChunkWork work;work.items=items;work.chunk_items=chunk;
        if(callbacks.cpu)work.cpu=[callbacks](ChunkRange r,std::stop_token){return callback_result(callbacks.cpu(callbacks.context,r.offset,r.count));};
        if(callbacks.interrupt)work.interrupt=[callbacks](uint64_t type,std::span<const std::byte> bytes){
            return callback_result(callbacks.interrupt(callbacks.context,type,bytes.data(),bytes.size()));
        };
        if(callbacks.submit)work.device=[callbacks](size_t lane,ChunkDispatch dispatch,std::stop_token)->ii::core::Result<ChunkSubmission>{
            auto op=std::make_shared<DeviceOperation>();op->callbacks=callbacks;
            if(callbacks.submit(callbacks.context,lane,dispatch.range.offset,dispatch.range.count,
                                dispatch.group_size,dispatch.groups,&op->token)!=0)
                return ii::core::fail(ii::core::ErrorCode::failed,"Foreign device submission failed");
            op->submitted=true;
            ChunkSubmission pending;
            pending.wait=[op]{
                const auto result=op->callbacks.wait(op->callbacks.context,op->token);
                op->waited=true;
                return callback_result(result);
            };
            if(callbacks.collect)pending.collect=[op]{return callback_result(op->callbacks.collect(op->callbacks.context,op->token));};
            return pending;
        };
        auto accepted=q->queue.submit(std::move(work));if(!accepted)return error(accepted.error());
        task->ticket=std::move(*accepted);*out=task.release();return int(IISDKCORE_RESOURCE_OK);
    });
}
void iisdkcore_chunk_task_destroy(iisdkcore_chunk_task* task) { delete task; }
int iisdkcore_chunk_task_snapshot(iisdkcore_chunk_task* task,iisdkcore_chunk_snapshot* out) {
    return protect([&]{if(!task || !out)return int(IISDKCORE_RESOURCE_INVALID);snapshot(task->ticket.snapshot(),*out);return int(IISDKCORE_RESOURCE_OK);});
}
int iisdkcore_chunk_task_wait(iisdkcore_chunk_task* task,uint64_t revision,uint64_t milliseconds,iisdkcore_chunk_snapshot* out) {
    return protect([&]{
        if(!task || !out)return int(IISDKCORE_RESOURCE_INVALID);
        using Rep=std::chrono::milliseconds::rep;
        if(milliseconds>static_cast<uint64_t>((std::numeric_limits<Rep>::max)()))return int(IISDKCORE_RESOURCE_INVALID);
        snapshot(task->ticket.wait_for_change(revision,std::chrono::milliseconds(static_cast<Rep>(milliseconds))),*out);
        return int(IISDKCORE_RESOURCE_OK);
    });
}
int iisdkcore_chunk_task_pause(iisdkcore_chunk_task* t){return protect([&]{return t?status(t->ticket.pause()):int(IISDKCORE_RESOURCE_INVALID);});}
int iisdkcore_chunk_task_resume(iisdkcore_chunk_task* t){return protect([&]{return t?status(t->ticket.resume()):int(IISDKCORE_RESOURCE_INVALID);});}
int iisdkcore_chunk_task_cancel(iisdkcore_chunk_task* t){return protect([&]{return t?status(t->ticket.cancel()):int(IISDKCORE_RESOURCE_INVALID);});}
int iisdkcore_chunk_task_interrupt(iisdkcore_chunk_task* t,uint64_t type,const void* bytes,size_t size){
    return protect([&]{if(!t || (!bytes && size))return int(IISDKCORE_RESOURCE_INVALID);
        return status(t->ticket.interrupt(type,{static_cast<const std::byte*>(bytes),size}));});
}
}
