#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define IISDKCORE_CHUNK_FINAL final
#else
#define IISDKCORE_CHUNK_FINAL
#endif
/* Context and callbacks remain valid until the task reaches a terminal phase. */
typedef struct iisdkcore_chunk_queue iisdkcore_chunk_queue;
typedef struct iisdkcore_chunk_task iisdkcore_chunk_task;
typedef struct iisdkcore_chunk_lane IISDKCORE_CHUNK_FINAL { uint32_t group_size, max_groups; } iisdkcore_chunk_lane;
typedef int (*iisdkcore_chunk_cpu)(void*,uint64_t,uint32_t);
typedef int (*iisdkcore_chunk_submit)(void*,size_t,uint64_t,uint32_t,uint32_t,uint32_t,uintptr_t*);
typedef int (*iisdkcore_chunk_wait)(void*,uintptr_t);
typedef int (*iisdkcore_chunk_collect)(void*,uintptr_t);
typedef void (*iisdkcore_chunk_release)(void*,uintptr_t);
typedef int (*iisdkcore_chunk_interrupt)(void*,uint64_t,const void*,size_t);
typedef struct iisdkcore_chunk_callbacks IISDKCORE_CHUNK_FINAL {
    void* context;
    iisdkcore_chunk_cpu cpu;
    iisdkcore_chunk_submit submit;
    iisdkcore_chunk_wait wait;
    iisdkcore_chunk_collect collect;
    iisdkcore_chunk_release release;
    iisdkcore_chunk_interrupt interrupt;
} iisdkcore_chunk_callbacks;
typedef struct iisdkcore_chunk_snapshot IISDKCORE_CHUNK_FINAL {
    int phase;
    uint64_t revision,total_items,claimed_items,completed_items,completed_chunks,device_submitted;
    size_t cpu_active,device_active,device_in_flight,interrupts_active;
} iisdkcore_chunk_snapshot;
/* Return codes share IISDKCORE_RESOURCE_* in ResourceControlC.h. */
int iisdkcore_chunk_queue_create(size_t,size_t,const iisdkcore_chunk_lane*,size_t,iisdkcore_chunk_queue**);
int iisdkcore_chunk_queue_destroy(iisdkcore_chunk_queue*);
int iisdkcore_chunk_submit_work(iisdkcore_chunk_queue*,uint64_t,uint32_t,const iisdkcore_chunk_callbacks*,iisdkcore_chunk_task**);
void iisdkcore_chunk_task_destroy(iisdkcore_chunk_task*);
int iisdkcore_chunk_task_snapshot(iisdkcore_chunk_task*,iisdkcore_chunk_snapshot*);
int iisdkcore_chunk_task_wait(iisdkcore_chunk_task*,uint64_t,uint64_t,iisdkcore_chunk_snapshot*);
int iisdkcore_chunk_task_pause(iisdkcore_chunk_task*);
int iisdkcore_chunk_task_resume(iisdkcore_chunk_task*);
int iisdkcore_chunk_task_cancel(iisdkcore_chunk_task*);
int iisdkcore_chunk_task_interrupt(iisdkcore_chunk_task*,uint64_t,const void*,size_t);
#undef IISDKCORE_CHUNK_FINAL
#ifdef __cplusplus
}
#endif
