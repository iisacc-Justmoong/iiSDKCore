#include "file_fixture.h"
#include <iiSDKCore/API/Services.h>
#include <iiSDKCore/Core/Assert/Contract.h>
#include <iiSDKCore/Core/Event/Event.h>
#include <iiSDKCore/Core/File/File.h>
#include <iiSDKCore/Core/IO/BinaryCursor.h>
#include <iiSDKCore/Core/Path/PathRoot.h>
#include <iiSDKCore/Core/Platform/PlatformProfile.h>
#include <iiSDKCore/Core/Property/PropertyLens.h>
#include <iiSDKCore/Core/Result/Result.h>
#include <iiSDKCore/Core/Task/TaskGroup.h>
#include <iiSDKCore/Core/Time/Deadline.h>
#include <iiSDKCore/Core/Version/Version.h>
#include <iiSDKCore/Debug/Diagnostics.h>
#include <iiSDKCore/Graph/DependencyGraph.h>
#include <iiSDKCore/Scene/Scene.h>
#include <iiSDKCore/Scripting/Bindings.h>
#include <iiSDKCore/SDL/HostSession.h>
#include <iiSDKCore/Vulkan/Timeline.h>
#include <array>
#include <atomic>
#include <iostream>
#include <future>

using namespace ii;
using namespace std::chrono_literals;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void cancellation_contract(bool shutdown) {
    runtime::Runtime runtime({.workers=1,.capacity=2});
    std::promise<void> started,release;
    auto gate=release.get_future().share();
    auto running=runtime.post([&] { started.set_value(); gate.wait(); });
    started.get_future().wait();
    core::TaskGroup group(runtime);
    int executed=0;
    auto queued=group.post([&] { ++executed; });
    auto rejected=group.post([] {});
    group.close();
    bool cancelled=true;
    if (shutdown) runtime.request_stop(runtime::ShutdownMode::cancel_pending);
    else cancelled=runtime.cancel(queued.token);
    release.set_value();
    check(bool(runtime.join()), "cancel runtime joins");
    check(running && queued && !rejected && cancelled && executed==0, "queued group task cancellation");
    auto result=group.result();
    check(group.pending()==0 && result && !*result && result->error().code==core::ErrorCode::cancelled,
          "discarded callable settles task group");
}
int run() {
    auto version=core::Version::parse("1.20.3");
    check(version && version->string()=="1.20.3" && *version > core::Version{1,2,99}, "version numeric ordering");
    for (auto invalid : {"", "1.2", "1.2.3.4", "01.2.3", "1.-2.3", "4294967296.0.0"})
        check(!core::Version::parse(invalid), "strict bounded version parsing");
    int violations=0;
    core::Contract contract([&](const core::Violation& v) { check(v.location.line()>0, "contract source"); ++violations; });
    check(contract.check(true, "valid") && !contract.check(false, "invalid") && violations==1, "instance contract reporter");
    const auto now=runtime::Clock::time_point{};
    auto deadline=core::Deadline::after(10ms, now);
    check(!deadline.expired(now) && deadline.expired(now+10ms) && deadline.remaining(now+20ms)==0ms, "deadline boundaries");
    check(!core::Deadline{}.expired(now), "unbounded deadline");
    core::EventSequence events;
    auto first=events.make(42), second=events.make(43);
    check(first.sequence+1==second.sequence && first.payload==42 && first.time<=second.time, "event value envelope");
    core::PathRoot root(std::filesystem::current_path());
    check(root.resolve("a/../b").value()==root.path()/"b", "relative path normalization");
    check(!root.resolve("../escape") && !root.resolve(root.path()), "root traversal/absolute rejection");
    check(!root.resolve(std::string("a\0b",3)), "embedded NUL path rejected");
    std::array<std::byte, 8> bytes{};
    core::ByteWriter writer(bytes);
    check(bool(writer.write<std::uint32_t>(0x12345678)) && bool(writer.write<std::int16_t>(-2, std::endian::big)), "binary write");
    check(bytes[0]==std::byte{0x78} && bytes[4]==std::byte{0xff} && bytes[5]==std::byte{0xfe}, "explicit endian layout");
    auto cursor=writer.position();
    check(!writer.write<std::uint32_t>(0) && writer.position()==cursor, "failed write is atomic");
    core::ByteReader reader(bytes);
    check(reader.read<std::uint32_t>()==0x12345678 && reader.read<std::int16_t>(std::endian::big)==-2, "binary roundtrip");
    check(!reader.take(3) && reader.position()==6, "reader bounds");
    auto profile=core::PlatformProfile::current();
    check(profile.os==runtime::current_platform() && profile.pointer_bits==sizeof(void*)*8, "platform snapshot");
    api::Services services;
    int service=7;
    check(bool(services.expose(service)) && &services.find<int>()->get()==&service, "typed borrowed service");
    check(!services.expose(service) && !services.find<double>(), "no service replacement or type confusion");
    check(services.remove<int>() && !services.find<int>(), "remove service registration");
    debug::Diagnostics diagnostics(2);
    check(diagnostics.write(debug::Level::info,"one") && diagnostics.write(debug::Level::warning,"two") &&
          diagnostics.write(debug::Level::error,"three"), "bounded diagnostics write");
    debug::Record record;
    check(diagnostics.stats().dropped==1 && diagnostics.try_pop(record) && record.message=="two", "diagnostic eviction");

    graph::DependencyGraph graph(3), other(1);
    auto a=graph.add().value(), b=graph.add().value(), c=graph.add().value();
    check(!graph.add() && !graph.connect(a,other.add().value()), "capacity and owner identity");
    check(bool(graph.connect(a,b)) && bool(graph.connect(b,c)) && !graph.connect(c,a), "acyclic dependency edges");
    check(graph.order().value()==std::vector{a,b,c}, "topological order");
    check(bool(graph.remove(b)), "remove node");
    auto reused=graph.add().value();
    check(reused!=b && !graph.contains(b) && graph.children(a)->empty(), "generation reuse and edge cleanup");

    std::uint64_t hardware_completed=0;
    vulkan::Timeline timeline([&]() -> core::Result<std::uint64_t> { return hardware_completed; });
    auto ticket=timeline.issue().value();
    check(!timeline.ready(ticket).value() && !timeline.ready(ticket+1), "GPU completion not assumed");
    hardware_completed=ticket;
    check(timeline.poll()==ticket && timeline.ready(ticket).value(), "backend completion observation");
    hardware_completed=0;
    check(!timeline.poll() && timeline.ready(ticket).value(), "counter regression rejected");
    hardware_completed=ticket+1;
    check(!timeline.poll() && timeline.ready(ticket).value(), "counter overshoot preserves completion");
    vulkan::Timeline broken([]() -> core::Result<std::uint64_t> { throw std::runtime_error("driver failure"); });
    check(broken.poll().error().cause!=nullptr, "backend exception retained");
    std::function<core::Result<std::uint64_t>()> reenter;
    vulkan::Timeline reentrant([&] { return reenter(); });
    reenter=[&]() -> core::Result<std::uint64_t> {
        check(reentrant.poll().error().code==core::ErrorCode::busy, "recursive poll rejected");
        return 0;
    };
    check(reentrant.poll()==0, "polling guard restored");

    runtime::EventLoop loop(8);
    int host_state=0;
    sdl::HostSession host({
        [&]() -> core::Result<void> { check(host_state++==0, "host initialize once"); return {}; },
        [&]() -> core::Result<bool> { ++host_state; return true; },
        [&]() -> core::Result<void> { ++host_state; return {}; }
    });
    check(bool(host.open()) && !host.open(), "explicit host lifecycle");
    int pumped=0;
    check(bool(loop.post([&] { ++pumped; })) && host.step(loop).value() && pumped==1, "host and runtime pump composition");
    check(bool(host.close()) && bool(host.close()) && host_state==3, "idempotent host shutdown");
    check(!host.step(loop), "closed host cannot pump");
    sdl::HostSession failed_host({
        []() -> core::Result<void> { throw std::runtime_error("init failed"); },
        []() -> core::Result<bool> { return true; },
        []() -> core::Result<void> { return {}; }
    });
    check(failed_host.open().error().cause!=nullptr && !failed_host.step(loop), "failed initialization stays inactive");

    // Named file object owns the path and channel. Fixture lives inside build/VFS.
    const auto filename="iisdk-foundations-"+std::to_string(runtime::Clock::now().time_since_epoch().count());
    auto file=core::File::open(root, filename, ii::testing::open_file, {.access=io::Access::read_write, .creation=io::Creation::create_new});
    check(bool(file) && file->path()==root.path()/filename, "rooted file opening");
    check(file->channel().write(bytes)==bytes.size() && bool(file->channel().close()), "file channel composition");
    std::filesystem::remove(file->path());

    if (runtime::Thread::supported()) {
        cancellation_contract(false);
        cancellation_contract(true);
        runtime::Runtime runtime({.workers=2,.capacity=128});
        std::atomic<int> count=0;
        core::TaskGroup group(runtime);
        for (int i=0;i<50;++i) check(bool(group.post([&] { ++count; })), "group submit");
        group.close();
        check(!group.post([] {}), "sealed group rejects new work");
        check(bool(runtime.wait_idle()) && count==50 && group.result() && bool(*group.result()), "group asynchronous completion");
        core::TaskGroup failed(runtime);
        check(bool(failed.post([] { throw std::runtime_error("group error"); })), "failing task submit");
        failed.close();
        check(bool(runtime.wait_idle()) && failed.result() && !*failed.result() && failed.result()->error().cause, "group exception retained");
        runtime::SerialQueue queue(runtime);
        scene::Scene scene(queue,8);
        scripting::Bindings script(queue, {{"twice",[](const runtime::DynamicValue& input) -> runtime::DynamicValue {
            return runtime::value_cast<int>(input).value()*2;
        }}});
        check(!scene.create({}), "scene enforces execution domain");
        check(!script.define("foreign",[](const auto& input) { return input; }), "binding execution domain");
        check(bool(queue.post([&] {
            auto parent=scene.create({}).value();
            auto child=scene.create({{.name="audio", .children={{.name="gain",.value=1,.policy={.writable=true}}}}},parent).value();
            check(!scene.reparent(parent,child), "scene cycle rejected");
            check(scene.parent(child).value()==parent && !scene.parent(parent).value(), "failed reparent preserves topology");
            core::PropertyLens lens({"audio","gain"});
            auto& properties=scene.properties(child).value().get();
            check(bool(properties.apply(lens.write(42))), "lens writes selected path");
            auto selected=properties.apply(lens.read(typeid(std::string)));
            check(selected && runtime::value_cast<std::string>(selected->fields[0].children[0].value)=="42", "lens nested selection and cast");
            check(bool(scene.erase(parent)) && !scene.properties(child), "subtree lifetime and stale identity");
            check(bool(script.define("identity",[](const runtime::DynamicValue& input) { return input; })), "extend instance bindings");
            check(!script.define("identity",[](const auto& input) { return input; }), "duplicate binding rejected");
            check(bool(script.define("identity",[](const auto&) -> runtime::DynamicValue { return 5; },true)), "explicit binding replacement");
            check(runtime::value_cast<int>(script.queries().send({"identity",{}}).value().value().value)==5, "replacement callable");
            check(bool(script.remove("identity")) && !script.queries().send({"identity",{}}).value(), "removed binding not callable");
            check(bool(script.define("once",[&](const auto&) -> runtime::DynamicValue {
                script.remove("once").value(); return 9;
            })), "self-removing binding registration");
            check(runtime::value_cast<int>(script.queries().send({"once",{}}).value().value().value)==9,
                  "active callable survives its removal");
        })), "scene update submission");
        int scripted=0;
        check(bool(script.queries().post({"twice",21},[&](runtime::QueryReply<scripting::CallResult> reply) {
            check(reply && *reply, "script callable query");
            scripted=runtime::value_cast<int>((**reply).value).value();
        })), "script async query");
        check(bool(runtime.wait_idle()) && scripted==42 && runtime.stats().failed==0 && bool(runtime.join()), "composed foundation execution");
    }
    std::cout << "foundation contracts passed on " << runtime::platform_name() << '\n';
    return 0;
}
int main() { try { return run(); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
