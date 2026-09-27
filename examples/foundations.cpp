#include <iiSDKCore/Core/IO/BinaryCursor.h>
#include <iiSDKCore/Core/Property/PropertyLens.h>
#include <iiSDKCore/Debug/Diagnostics.h>
#include <iiSDKCore/Scene/Scene.h>
#include <iiSDKCore/Scripting/Bindings.h>
#include <array>
#include <iostream>

int main() {
    using namespace ii;
    if (!runtime::Thread::supported()) {
        std::cout << "foundation pipeline requires worker threads\n";
        return 0;
    }
    runtime::Runtime runtime({.workers=2,.capacity=32});
    runtime::SerialQueue domain(runtime);
    scene::Scene scene(domain,8);
    debug::Diagnostics diagnostics(8);
    core::PropertyLens reading({"sensor","reading"});
    graph::NodeId sensor;
    scripting::Bindings bindings(domain, {{"receive",[&](const runtime::DynamicValue& input) -> runtime::DynamicValue {
        auto value=runtime::value_cast<int>(input).value();
        auto& properties=scene.properties(sensor).value().get();
        (void)properties.apply(reading.write(value)).value();
        return value;
    }}});
    if (!domain.post([&] {
        sensor=scene.create({{.name="sensor",.children={{.name="reading",.value=0,.policy={.writable=true}}}}}).value();
    })) return 1;

    // A device protocol payload carries just two bytes; no whole object transfer.
    std::array<std::byte,2> packet{std::byte{0},std::byte{42}};
    core::ByteReader reader(packet);
    auto value=reader.read<std::uint16_t>(std::endian::big).value();
    int received=0;
    if (!bindings.queries().post({"receive",value},[&](runtime::QueryReply<scripting::CallResult> reply) {
        received=runtime::value_cast<int>(reply.value().value().value).value();
        (void)diagnostics.write(debug::Level::info,"sensor reading updated");
    })) return 2;
    if (!runtime.wait_idle() || runtime.stats().failed || received!=42 || !runtime.join()) return 3;
    debug::Record record;
    if (!diagnostics.try_pop(record)) return 4;
    std::cout << "foundation pipeline complete: sensor.reading=" << received << '\n';
    return 0;
}
