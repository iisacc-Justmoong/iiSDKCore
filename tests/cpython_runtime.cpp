#include <iiSDKCore/Scripting/CPythonHost.h>

#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ii;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory required");
        runtime::Runtime runtime({.workers=2, .capacity=32});
        runtime::SerialQueue queue(runtime);
        auto host = scripting::CPythonHost::attach(queue, {.module_paths={argv[1]}});
        check(bool(host), "attach CPython host");
        auto invoke = [&](scripting::PythonCall call) -> core::Result<core::Value> {
            std::promise<core::Result<core::Value>> promise;
            auto future = promise.get_future();
            auto submitted = (*host)->queries().post(std::move(call), [&](auto reply) {
                if (!reply) promise.set_value(core::fail(core::ErrorCode::failed, "query delivery failed"));
                else promise.set_value(std::move(*reply));
            });
            check(bool(submitted), "submit Python query");
            return future.get();
        };
        const core::Value input(core::Value::Object{
            {"number", core::Value(std::int64_t{42})},
            {"items", core::Value(core::Value::Array{core::Value(true), core::Value("text"), core::Value{}, core::Value(1.25)})},
            {"bytes", core::Value(core::Bytes{std::byte{0}, std::byte{255}})},
            {"unsigned", core::Value(std::numeric_limits<std::uint64_t>::max())}
        });
        auto echoed = invoke({"iisdk_cpython_fixture", "echo", {input}});
        check(echoed && *echoed == input, "structured values round-trip");
        auto failed = invoke({"iisdk_cpython_fixture", "fail", {}});
        check(!failed && failed.error().message.find("fixture failure") != std::string::npos,
              "Python exception is reported");
        check(!invoke({"iisdk_cpython_fixture", "missing", {}}), "missing callable is an error");
        check(!invoke({"iisdk_cpython_fixture", "cycle", {}}), "cyclic Python values are rejected");
        check(!invoke({"iisdk_cpython_fixture", "huge", {}}), "oversized Python integers are rejected");
        check(bool(invoke({"iisdk_cpython_fixture", "echo", {core::Value("after error")}})),
              "the Python error state is cleared after a failed call");
        check(bool(invoke({"iisdk_cpython_fixture", "rewrite_origin", {}})), "fixture rewrites its source-like origin");
        check(bool(invoke({"iisdk_cpython_fixture", "echo", {core::Value("again")}})),
              "an already verified AOT module remains callable");
        auto second = scripting::CPythonHost::attach(queue, {.module_paths={argv[1]}});
        check(bool(second), "another host attaches to the process interpreter");
        std::promise<runtime::QueryReply<scripting::PythonResult>> second_reply;
        auto second_future = second_reply.get_future();
        check(bool((*second)->queries().post({"iisdk_cpython_fixture", "echo", {core::Value("shared")}},
            [&](auto reply) { second_reply.set_value(std::move(reply)); })), "second host submits a call");
        auto shared = second_future.get();
        check(shared && *shared && (**shared) == core::Value("shared"),
              "a second host validates the original native extension loader");
        second->reset();
        check(!invoke({"json", "dumps", {input}}), "source and unapproved modules cannot be AOT entry modules");
        host->reset();
        check(bool(runtime.wait_idle()), "runtime becomes idle");
        check(bool(queue.close()), "queue closes after host");
        check(bool(runtime.join()), "runtime joins");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
