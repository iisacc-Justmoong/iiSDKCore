#include <iiSDKCore/Runtime/QueryStream.h>
#include <atomic>
#include <iostream>
#include <optional>
#include <string>

using namespace ii::runtime;
using namespace std::chrono_literals;
struct Observe final { int device; bool level = false, label = false; };
struct Measurement final { int device, level; std::string label; };
struct Selected final { std::optional<int> level; std::optional<std::string> label; };

int main() {
    EventLoop ui;
    Runtime workers({.workers=2, .capacity=32});
    SerialQueue producer(workers);
    std::atomic<int> label_reads = 0;
    QueryStream<Observe, Measurement, Selected> stream(
        [&](const Observe& query, const Measurement& source) -> std::optional<Selected> {
            if (query.device != source.device) return {};
            Selected fields;
            if (query.level) fields.level = source.level;
            if (query.label) { fields.label = source.label; ++label_reads; }
            return fields;
        });
    int last = 0;
    auto subscription = stream.subscribe_on(ui.sender(), {1, true, false}, [&](auto event) {
        if (!event.value.level || event.value.label) throw std::runtime_error("invalid selected fields");
        last = *event.value.level;
        std::cout << "revision=" << event.revision << " sequence=" << event.sequence
                  << " level=" << last << '\n';
        if (last == 6) ui.request_stop();
    });
    if (!subscription) return 1;
    int tick = 0;
    std::atomic<bool> failed = false;
    const auto timer = ui.every(5ms, [&] {
        if (tick >= 6) return;
        const int value = ++tick;
        if (value == 3) {
            const auto reply = subscription->queries().send({2, true, false});
            if (!reply || !*reply) { failed = true; ui.request_stop(); return; }
        }
        const auto submitted = producer.post([&, value] {
            // Example measurements; a product can publish native I/O or game state here.
            auto result = stream.publish({value >= 3 ? 2 : 1, value, "unselected label"});
            if (!result || result->projection_failed || result->rejected || result->dispatch_failed) {
                failed = true; ui.request_stop();
            }
        });
        if (!submitted) { failed = true; ui.request_stop(); }
    });
    const auto deadline = ui.after(2s, [&] { failed = true; ui.request_stop(); });
    if (!timer || !deadline) return 1;
    ui.run();
    if (!workers.join() || !stream.close() || failed || last != 6 || label_reads != 0) return 1;
    std::cout << "selective live stream complete; unselected label reads=0\n";
}
