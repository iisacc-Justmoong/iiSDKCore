#include "ResourceControlC.h"
#include "ManagedMemory.h"
#include "Runtime.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

using namespace ii::runtime;
struct iisdkcore_resource_pool final { explicit iisdkcore_resource_pool(size_t limit):pool(limit){} MemoryPool pool; };
struct iisdkcore_resource_block final { explicit iisdkcore_resource_block(MemoryBlock value):block(std::move(value)){} MemoryBlock block; };
struct iisdkcore_resource_runtime final {
    iisdkcore_resource_runtime(size_t workers,size_t capacity):runtime({.workers=workers,.capacity=capacity}){}
    Runtime runtime;
};
namespace {
int memory_error(MemoryError error){
    switch(error){case MemoryError::invalid:return IISDKCORE_RESOURCE_INVALID;case MemoryError::limit_exceeded:return IISDKCORE_RESOURCE_LIMIT;
    case MemoryError::out_of_memory:return IISDKCORE_RESOURCE_OUT_OF_MEMORY;case MemoryError::closed:return IISDKCORE_RESOURCE_CLOSED;}
    return IISDKCORE_RESOURCE_FAILED;
}
int thread_error(const ThreadError& error){
    switch(error.code){case ThreadErrorCode::invalid_work:return IISDKCORE_RESOURCE_INVALID;
    case ThreadErrorCode::busy:return IISDKCORE_RESOURCE_BUSY;
    case ThreadErrorCode::self_join:case ThreadErrorCode::would_block:return IISDKCORE_RESOURCE_WOULD_BLOCK;
    default:return IISDKCORE_RESOURCE_FAILED;}
}
template<class Function> int protect(Function&& function) noexcept {
    try{return function();}catch(const std::bad_alloc&){return IISDKCORE_RESOURCE_OUT_OF_MEMORY;}catch(...){return IISDKCORE_RESOURCE_FAILED;}
}
bool range(const MemoryBlock& block,size_t offset,size_t length){return offset<=block.size()&&length<=block.size()-offset;}
}
extern "C" {
int iisdkcore_resource_pool_create(size_t limit,iisdkcore_resource_pool** out){return protect([&]{if(!out)return int(IISDKCORE_RESOURCE_INVALID);*out=nullptr;*out=new iisdkcore_resource_pool(limit);return int(IISDKCORE_RESOURCE_OK);});}
void iisdkcore_resource_pool_destroy(iisdkcore_resource_pool* pool){delete pool;}
int iisdkcore_resource_pool_limit(iisdkcore_resource_pool* pool,size_t limit){return protect([&]{if(!pool)return int(IISDKCORE_RESOURCE_INVALID);auto result=pool->pool.set_limit(limit);return result?int(IISDKCORE_RESOURCE_OK):memory_error(result.error());});}
int iisdkcore_resource_pool_stats(iisdkcore_resource_pool* pool,size_t* limit,size_t* used,size_t* peak,size_t* allocations){return protect([&]{
    if(!pool||!limit||!used||!peak||!allocations)return int(IISDKCORE_RESOURCE_INVALID);
    const auto s=pool->pool.stats();*limit=s.limit;*used=s.used;*peak=s.peak;*allocations=s.allocations;return int(IISDKCORE_RESOURCE_OK);
});}
int iisdkcore_resource_block_create(iisdkcore_resource_pool* pool,size_t size,size_t alignment,iisdkcore_resource_block** out){return protect([&]{
    if(!pool||!out)return int(IISDKCORE_RESOURCE_INVALID);*out=nullptr;
    auto allocation=pool->pool.allocate(size,alignment?alignment:alignof(std::max_align_t));if(!allocation)return memory_error(allocation.error());
    std::fill(allocation->bytes().begin(),allocation->bytes().end(),std::byte{});
    *out=new iisdkcore_resource_block(std::move(*allocation));return int(IISDKCORE_RESOURCE_OK);
});}
void iisdkcore_resource_block_destroy(iisdkcore_resource_block* block){delete block;}
int iisdkcore_resource_block_size(iisdkcore_resource_block* block,size_t* out){return protect([&]{if(!block||!out)return int(IISDKCORE_RESOURCE_INVALID);*out=block->block.size();return int(IISDKCORE_RESOURCE_OK);});}
int iisdkcore_resource_block_read(iisdkcore_resource_block* block,size_t offset,void* data,size_t length){return protect([&]{
    if(!block||(!data&&length)||!range(block->block,offset,length))return int(IISDKCORE_RESOURCE_INVALID);
    if(length)std::memcpy(data,block->block.bytes().data()+offset,length);return int(IISDKCORE_RESOURCE_OK);
});}
int iisdkcore_resource_block_write(iisdkcore_resource_block* block,size_t offset,const void* data,size_t length){return protect([&]{
    if(!block||(!data&&length)||!range(block->block,offset,length))return int(IISDKCORE_RESOURCE_INVALID);
    if(length)std::memcpy(block->block.bytes().data()+offset,data,length);return int(IISDKCORE_RESOURCE_OK);
});}
int iisdkcore_resource_block_resize(iisdkcore_resource_block* block,size_t size){return protect([&]{
    if(!block)return int(IISDKCORE_RESOURCE_INVALID);const auto old=block->block.size();auto result=block->block.resize(size);
    if(!result)return memory_error(result.error());if(size>old)std::fill(block->block.bytes().begin()+static_cast<std::ptrdiff_t>(old),block->block.bytes().end(),std::byte{});
    return int(IISDKCORE_RESOURCE_OK);
});}
int iisdkcore_resource_block_transfer(iisdkcore_resource_block* block,iisdkcore_resource_pool* destination){return protect([&]{
    if(!block||!destination)return int(IISDKCORE_RESOURCE_INVALID);auto result=block->block.transfer(destination->pool);
    return result?int(IISDKCORE_RESOURCE_OK):memory_error(result.error());
});}
int iisdkcore_resource_runtime_create(size_t workers,size_t capacity,iisdkcore_resource_runtime** out){return protect([&]{
    if(!out||!capacity||workers>capacity)return int(IISDKCORE_RESOURCE_INVALID);*out=nullptr;
    auto value=std::make_unique<iisdkcore_resource_runtime>(workers,capacity);
    *out=value.release();return int(IISDKCORE_RESOURCE_OK);
});}
void iisdkcore_resource_runtime_destroy(iisdkcore_resource_runtime* runtime){delete runtime;}
int iisdkcore_resource_runtime_resize(iisdkcore_resource_runtime* runtime,size_t workers){return protect([&]{
    if(!runtime)return int(IISDKCORE_RESOURCE_INVALID);auto result=runtime->runtime.resize_workers(workers);
    return result?int(IISDKCORE_RESOURCE_OK):thread_error(result.error());
});}
int iisdkcore_resource_runtime_workers(iisdkcore_resource_runtime* runtime,size_t* out){return protect([&]{
    if(!runtime||!out)return int(IISDKCORE_RESOURCE_INVALID);*out=runtime->runtime.stats().workers;return int(IISDKCORE_RESOURCE_OK);
});}
int iisdkcore_resource_runtime_allocated_slots(iisdkcore_resource_runtime* runtime,size_t* out){return protect([&]{
    if(!runtime||!out)return int(IISDKCORE_RESOURCE_INVALID);*out=runtime->runtime.allocated_slots();return int(IISDKCORE_RESOURCE_OK);
});}
}
