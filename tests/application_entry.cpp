#include <iiSDKCore/Runtime/Application.h>
#include <iostream>

ii::core::Result<void> ii::runtime::configure_application(Application& app) {
    const auto args = app.arguments();
    const bool simple = args.size()==2 && args[1]=="--entry-test";
    const bool unicode = args.size()==4 && args[1]=="--entry-unicode" &&
                         args[2]=="\xed\x95\x9c \xea\xb8\x80 \xf0\x9f\x8e\xa8" && args[3].empty();
    if (!simple && !unicode)
        return core::fail(core::ErrorCode::invalid_argument,"Core entry arguments missing");
    if (app.workers().stats().workers || app.workers().allocated_slots())
        return core::fail(core::ErrorCode::failed,"Core entry created unrequested workers");
    if (auto allocated=app.workers().resize_workers(1); !allocated)
        return core::fail(core::ErrorCode::failed,"Cannot allocate requested entry worker",allocated.error().cause);
    int completed=0;
    if (!app.post([&]{++completed;}) || !app.workers().wait_idle() || completed!=1)
        return core::fail(core::ErrorCode::failed,"Explicit entry worker did not execute its task");
    auto attached = app.attach({"native-code", [](Application&) -> core::Result<void> {
        std::cout<<"CORE_ENTRY_ATTACHED\n"; return {};
    }, []() -> core::Result<void> { std::cout<<"CORE_ENTRY_DETACHED\n"; return {}; }});
    if (!attached) return attached;
    return app.set_driver({[](const ApplicationStart& start) { return start(); },[]() -> core::Result<void> { return {}; }});
}
