#include <iiSDKCore/Runtime/Application.h>
#include <iostream>

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
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
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
