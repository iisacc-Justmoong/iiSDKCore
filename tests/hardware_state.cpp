#include "../src/IO/HardwareInternal.h"
#if defined(__APPLE__)
#include "../src/IO/HardwareAppleIds.h"
#endif
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
using namespace ii::io;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
#if defined(__APPLE__)
    check(detail::core_midi_identity("midi:-2147483648") == INT32_MIN,
          "CoreMIDI signed identity minimum");
    check(detail::core_midi_identity("midi:2147483647") == INT32_MAX,
          "CoreMIDI signed identity maximum");
    check(detail::core_midi_identity("midi:2147483648") == INT32_MIN &&
              detail::core_midi_identity("midi:4294967295") == -1,
          "CoreMIDI legacy unsigned identity aliases");
    check(!detail::core_midi_identity("midi:-2147483649") &&
              !detail::core_midi_identity("midi:4294967296") &&
              !detail::core_midi_identity("midi:") &&
              !detail::core_midi_identity("midi:1x") &&
              !detail::core_midi_identity("usb:1"),
          "CoreMIDI malformed identity rejected");
#endif
    HardwareOptions config;
    config.event_capacity = 2;
    config.dispatch_batch = 2;
    config.max_payload_bytes = 16;
    std::vector<std::uint64_t> delivered;
    detail::HardwareState state(
        [&](HardwareEvent event) { delivered.push_back(event.endpoint.value); },
        config);
    Endpoint first{state.owner, 1}, second{state.owner, 2};
    state.activate(first);
    state.activate(second);
    check(!state.has_capacity(), "endpoint capacity enforced");
    HardwareEvent event;
    event.endpoint = first;
    event.bytes.resize(8);
    event.samples.resize(3);
    check(!state.publish(event) && state.stats().dropped == 1,
          "combined event payload bounded");
    event.samples.clear();
    check(state.publish(event), "first event queued");
    event.endpoint = second;
    check(state.publish(event), "second event queued");
    check(!state.publish(event) && state.stats().dropped == 2,
          "event queue capacity bounded");
    state.deactivate(first);
    check(state.stats().pending == 1 && state.has_capacity(),
          "closed endpoint releases queue and quota");
    event.endpoint = first;
    check(!state.publish(event), "late native callback discarded");
    state.dispatch();
    check(delivered == std::vector<std::uint64_t>{2},
          "retained endpoint preserves FIFO delivery");
    check(bool(state.callbacks.close()), "quiesce callback scope");
    state.retire();
    check(state.stats().pending == 0 && !state.publish(event),
          "retired state releases events");
    detail::AudioRing ring(4);
    const std::array<float, 5> input{1, 2, 3, 4, 5};
    std::array<float, 4> output{};
    check(ring.write(input) == 4 && ring.read(std::span(output).first(2)) == 2,
          "audio ring bounded write and partial read");
    check(ring.write(std::span(input).last(2)) == 2 && ring.read(output) == 4 &&
              output == std::array<float, 4>{3, 4, 4, 5},
          "audio ring wrap preserves samples");
#if !defined(__EMSCRIPTEN__)
    std::latch entered(1), release(1);
    std::atomic<bool> returned = false;
    config.event_capacity = config.dispatch_batch = 1;
    detail::HardwareState active(
        [&](HardwareEvent) {
          entered.count_down();
          release.wait();
        },
        config);
    Endpoint identity{active.owner, 1};
    active.activate(identity);
    event.endpoint = identity;
    event.bytes.clear();
    check(active.publish(event), "active completion queued");
    std::jthread dispatcher([&] { active.dispatch(); });
    entered.wait();
    active.deactivate(identity);
    std::jthread closer([&] {
      active.await_endpoint(identity);
      returned = true;
    });
    check(!active.has_capacity() && !returned,
          "active callback keeps endpoint quota until completion");
    release.count_down();
    dispatcher.join();
    closer.join();
    check(returned && active.has_capacity(),
          "close waits and returns endpoint quota");
    detail::AudioRing concurrent(64);
    std::atomic<bool> corrupt = false;
    std::jthread producer([&] {
      for (int i = 0; i < 20000; ++i) {
        const float sample = static_cast<float>(i);
        while (!concurrent.write(std::span(&sample, 1)))
          std::this_thread::yield();
      }
    });
    for (int i = 0; i < 20000; ++i) {
      float sample;
      while (!concurrent.read(std::span(&sample, 1)))
        std::this_thread::yield();
      if (sample != i)
        corrupt = true;
    }
    producer.join();
    check(!corrupt, "audio callback SPSC concurrent sample order");
#endif
    std::cout << "bounded hardware state contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
