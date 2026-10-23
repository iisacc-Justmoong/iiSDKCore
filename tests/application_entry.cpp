#include <iiSDKCore/Runtime/Application.h>
#include <iostream>

ii::core::Result<void> ii::runtime::configure_application(Application& app) {
    if (app.arguments().size()!=2 || app.arguments()[1]!="--entry-test")
        return core::fail(core::ErrorCode::invalid_argument,"Core entry arguments missing");
    auto attached = app.attach({"native-code", [](Application&) -> core::Result<void> {
        std::cout<<"CORE_ENTRY_ATTACHED\n"; return {};
    }, []() -> core::Result<void> { std::cout<<"CORE_ENTRY_DETACHED\n"; return {}; }});
    if (!attached) return attached;
    return app.set_driver({[](const ApplicationStart& start) { return start(); },[]() -> core::Result<void> { return {}; }});
}
