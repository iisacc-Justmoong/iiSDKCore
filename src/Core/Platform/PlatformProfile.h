#pragma once
#include "../../Runtime/Thread.h"
#include "../../IO/Channel.h"
#include <bit>

namespace ii::core {
enum class Architecture { arm64, arm32, x64, x86, wasm32, wasm64, unknown };
struct PlatformProfile final {
    runtime::Platform os;
    Architecture architecture;
    unsigned pointer_bits;
    std::endian byte_order;
    bool workers;
    io::PlatformCapabilities io;
    [[nodiscard]] static PlatformProfile current() noexcept {
        Architecture architecture=Architecture::unknown;
#if defined(__wasm64__)
        architecture=Architecture::wasm64;
#elif defined(__EMSCRIPTEN__)
        architecture=Architecture::wasm32;
#elif defined(__aarch64__) || defined(_M_ARM64)
        architecture=Architecture::arm64;
#elif defined(__arm__) || defined(_M_ARM)
        architecture=Architecture::arm32;
#elif defined(__x86_64__) || defined(_M_X64)
        architecture=Architecture::x64;
#elif defined(__i386__) || defined(_M_IX86)
        architecture=Architecture::x86;
#endif
        return {runtime::current_platform(),architecture,sizeof(void*)*8,std::endian::native,
                runtime::Thread::supported(),ii::io::platform_capabilities()};
    }
};
} // namespace ii::core
