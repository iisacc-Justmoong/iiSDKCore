#include <stdint.h>
// MSVC's default __cplusplus remains 199711L even with a modern /std mode.
#pragma push_macro("__cplusplus")
#pragma push_macro("_MSVC_LANG")
#undef __cplusplus
#undef _MSVC_LANG
#define __cplusplus 199711L
#define _MSVC_LANG 202302L
#include <iiSDKCore/Embedded/Embedded.h>
#pragma pop_macro("_MSVC_LANG")
#pragma pop_macro("__cplusplus")
int main() {
    ii::embedded::EventLoop<1> loop;
    return loop.pending() == 0 && ii::embedded::contract_version() == 1 ? 0 : 1;
}
