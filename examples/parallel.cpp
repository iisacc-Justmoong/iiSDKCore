#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <iostream>

using namespace ii::runtime;

int main() {
    if (!Thread::supported()) {
        std::cout << "parallel runtime requires workers\n";
        return 0;
    }
    Runtime runtime;
    SerialQueue device_queue(runtime), calculation_queue(runtime);
    ObjectProperties device(device_queue, {{.name = "level", .value = 0,
        .policy = {.queryable = true, .writable = true}}});
    QueryPort<int, int> calculate(calculation_queue, [](const int& value) { return value * 2; });
    int result = 0;
    const auto caller = Thread::current_id();
    bool background = false;
    if (!calculate.post(21, [&](QueryReply<int> reply) {
        if (!reply) return;
        // Completion continues by posting to the receiving object's queue.
        if (!device.queries().post(PropertyWrite{{"level"}, *reply}, [&](QueryReply<PropertyResult> edited) {
            if (!edited || !*edited) return;
            auto selected = device.queries().send(PropertyRead{{{.name = "level"}}});
            if (!selected || !*selected) return;
            result = std::any_cast<int>((**selected).fields[0].value);
            background = Thread::current_id() != caller;
        })) throw std::runtime_error("device queue rejected response");
    })) return 1;
    // Test/console synchronization only; applications keep processing other work.
    if (!runtime.wait_idle() || result != 42 || !background) return 2;
    std::cout << "parallel runtime complete: workers=" << runtime.stats().workers
              << " result=" << result << " asynchronous=1\n";
    return runtime.join() ? 0 : 3;
}
