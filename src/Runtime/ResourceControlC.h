#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct iisdkcore_resource_pool iisdkcore_resource_pool;
typedef struct iisdkcore_resource_block iisdkcore_resource_block;
typedef struct iisdkcore_resource_runtime iisdkcore_resource_runtime;
enum iisdkcore_resource_error {
    IISDKCORE_RESOURCE_OK=0, IISDKCORE_RESOURCE_INVALID=1,
    IISDKCORE_RESOURCE_LIMIT=2, IISDKCORE_RESOURCE_OUT_OF_MEMORY=3,
    IISDKCORE_RESOURCE_CLOSED=4, IISDKCORE_RESOURCE_BUSY=5,
    IISDKCORE_RESOURCE_WOULD_BLOCK=6, IISDKCORE_RESOURCE_FAILED=7
};
int iisdkcore_resource_pool_create(size_t limit,iisdkcore_resource_pool** out);
void iisdkcore_resource_pool_destroy(iisdkcore_resource_pool* pool);
int iisdkcore_resource_pool_limit(iisdkcore_resource_pool* pool,size_t limit);
int iisdkcore_resource_pool_stats(iisdkcore_resource_pool* pool,size_t* limit,size_t* used,size_t* peak,size_t* allocations);
int iisdkcore_resource_block_create(iisdkcore_resource_pool* pool,size_t size,size_t alignment,iisdkcore_resource_block** out);
void iisdkcore_resource_block_destroy(iisdkcore_resource_block* block);
int iisdkcore_resource_block_size(iisdkcore_resource_block* block,size_t* out);
int iisdkcore_resource_block_read(iisdkcore_resource_block* block,size_t offset,void* data,size_t length);
int iisdkcore_resource_block_write(iisdkcore_resource_block* block,size_t offset,const void* data,size_t length);
int iisdkcore_resource_block_resize(iisdkcore_resource_block* block,size_t size);
int iisdkcore_resource_block_transfer(iisdkcore_resource_block* block,iisdkcore_resource_pool* destination);
int iisdkcore_resource_runtime_create(size_t workers,size_t capacity,iisdkcore_resource_runtime** out);
void iisdkcore_resource_runtime_destroy(iisdkcore_resource_runtime* runtime);
int iisdkcore_resource_runtime_resize(iisdkcore_resource_runtime* runtime,size_t workers);
int iisdkcore_resource_runtime_workers(iisdkcore_resource_runtime* runtime,size_t* out);
int iisdkcore_resource_runtime_allocated_slots(iisdkcore_resource_runtime* runtime,size_t* out);
#ifdef __cplusplus
}
#endif
