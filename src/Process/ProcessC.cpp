#include "ProcessC.h"
#include "Process.h"
#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <vector>
struct iisdkcore_process_owner final { ii::process::Process process; };
namespace {
int error(const ii::core::Error& value) {
    using ii::core::ErrorCode;
    switch(value.code) {
        case ErrorCode::invalid_argument:case ErrorCode::out_of_range:return IISDKCORE_PROCESS_INVALID;
        case ErrorCode::closed:return IISDKCORE_PROCESS_CLOSED;
        case ErrorCode::busy:return IISDKCORE_PROCESS_BUSY;
        case ErrorCode::unsupported:return IISDKCORE_PROCESS_UNSUPPORTED;
        default:return IISDKCORE_PROCESS_FAILED;
    }
}
template<class F> int protect(F&& function) noexcept {
    try {return function();}catch(const std::bad_alloc&){return IISDKCORE_PROCESS_OUT_OF_MEMORY;}
    catch(...){return IISDKCORE_PROCESS_FAILED;}
}
void save(const ii::process::Exit& value,iisdkcore_process_exit& output) {
    output={1,static_cast<int>(value.reason),value.code,value.signal};
}
}
extern "C" {
int iisdkcore_process_start(size_t count,const char* const* arguments,iisdkcore_process_owner** out) {
    return protect([&]{
        if(!out || !arguments || !count)return int(IISDKCORE_PROCESS_INVALID);
        *out=nullptr;ii::process::Launch launch;launch.arguments.reserve(count);
        for(size_t i=0;i<count;++i) {
            if(!arguments[i])return int(IISDKCORE_PROCESS_INVALID);
            launch.arguments.emplace_back(arguments[i]);
        }
        launch.input=ii::process::Input::closed;launch.output=ii::process::Output::discard;
        launch.error=ii::process::ErrorOutput::discard;
        auto owner=std::make_unique<iisdkcore_process_owner>();
        auto child=ii::process::Process::start(launch);if(!child)return error(child.error());
        owner->process=std::move(*child);*out=owner.release();return int(IISDKCORE_PROCESS_OK);
    });
}
void iisdkcore_process_destroy(iisdkcore_process_owner* owner) {delete owner;}
int iisdkcore_process_id(iisdkcore_process_owner* owner,uint64_t* out) {
    return protect([&]{if(!owner || !out)return int(IISDKCORE_PROCESS_INVALID);*out=owner->process.id();return int(IISDKCORE_PROCESS_OK);});
}
int iisdkcore_process_signal(iisdkcore_process_owner* owner,int native_signal,int group) {
    return protect([&]{
        if(!owner || (group!=0 && group!=1))return int(IISDKCORE_PROCESS_INVALID);
        auto result=owner->process.send_signal(native_signal,group?ii::process::SignalTarget::group:ii::process::SignalTarget::child);
        return result?int(IISDKCORE_PROCESS_OK):error(result.error());
    });
}
int iisdkcore_process_wait(iisdkcore_process_owner* owner,uint64_t ms,iisdkcore_process_exit* out) {
    return protect([&]{
        if(!owner || !out || ms>static_cast<uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)()))
            return int(IISDKCORE_PROCESS_INVALID);
        *out={};auto result=owner->process.wait_for(std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(ms)));
        if(!result)return error(result.error());if(*result)save(**result,*out);return int(IISDKCORE_PROCESS_OK);
    });
}
int iisdkcore_process_cancel(iisdkcore_process_owner* owner,iisdkcore_process_exit* out) {
    return protect([&]{
        if(!owner || !out)return int(IISDKCORE_PROCESS_INVALID);*out={};auto result=owner->process.cancel();
        if(!result)return error(result.error());save(*result,*out);return int(IISDKCORE_PROCESS_OK);
    });
}
}
