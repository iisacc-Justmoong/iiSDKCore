#include <iiSDKCore/Runtime/Application.h>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include "ApplicationEntryArguments.h"
#endif

namespace {
int failure(const ii::core::Error& error) {
    std::cerr << error.message;
    if (error.cause) {
        try { std::rethrow_exception(error.cause); }
        catch (const std::exception& cause) { std::cerr << ": " << cause.what(); }
        catch (...) { std::cerr << ": native callback exception"; }
    }
    std::cerr << '\n';
    return 1;
}
}
int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        (void)argc; (void)argv;
        int count = 0;
        auto* raw = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        if (!raw) return failure({ii::core::ErrorCode::failed, "Cannot parse the Windows command line"});
        struct LocalArguments final {
            wchar_t** value;
            ~LocalArguments() { ::LocalFree(value); }
        } release{raw};
        if (count < 0) return failure({ii::core::ErrorCode::failed, "Invalid Windows argument count"});
        auto decoded = ii::runtime::detail::decode_windows_arguments(
            std::span<wchar_t* const>(raw, static_cast<std::size_t>(count)));
        if (!decoded) return failure(decoded.error());
        auto arguments = std::move(*decoded);
#else
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
#endif
        ii::runtime::Application application(std::move(arguments));
        auto configured = ii::runtime::configure_application(application);
        if (!configured) return failure(configured.error());
        auto result = application.exec();
        if (!result) return failure(result.error());
        return *result;
    } catch (const std::exception& error) {
        std::cerr << "Application startup failed: " << error.what() << '\n'; return 1;
    } catch (...) { std::cerr << "Application startup failed\n"; return 1; }
}
