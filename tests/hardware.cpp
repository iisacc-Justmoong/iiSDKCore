#include <atomic>
#include <iiSDKCore/IO/Hardware.h>
#include <iiSDKCore/Runtime/Thread.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <objbase.h>
#endif
using namespace ii::io;
using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void pump(EventLoop &loop, std::function<bool()> done, const char *stage) {
  const auto end = Clock::now() + 3s;
  while (!done() && Clock::now() < end) {
    loop.poll({64, 20ms});
    std::this_thread::sleep_for(2ms);
  }
  check(done(), stage);
}
} // namespace
int main() {
  std::cout << std::unitbuf;
  try {
#if defined(_WIN32)
    struct NativeApartment final {
      NativeApartment() {
        check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
              "initialize host MTA");
      }
      ~NativeApartment() { CoUninitialize(); }
    } apartment;
#endif
    EventLoop loop(16);
    std::size_t events = 0;
    Hardware devices(loop, [&](HardwareEvent) { ++events; });
    check(!devices.enumerate(static_cast<DeviceKind>(999)),
          "invalid device kind");
    check(!devices.usb_control({}, protocol::USBSetup{}, {}, 0ms),
          "invalid USB deadline");
    check(!devices.hid_get({}, protocol::HIDReportType::input, 0, 0),
          "invalid report length");
    check(!devices.scan_bluetooth({}), "BLE requires service filter");
    check(!devices.open_audio(DeviceKind::usb), "invalid audio direction");
    check(!devices.close(Endpoint{1, 123}), "foreign endpoint rejected");
    auto caps = hardware_capabilities();
    check(!caps.midi2 || caps.midi, "MIDI2 capability requires MIDI transport");
    for (auto kind : {DeviceKind::usb, DeviceKind::hid,
                      DeviceKind::audio_output, DeviceKind::midi_output}) {
      auto list = devices.enumerate(kind);
      const bool supported = kind == DeviceKind::usb            ? caps.usb
                             : kind == DeviceKind::hid          ? caps.hid
                             : kind == DeviceKind::audio_output ? caps.audio
                                                                : caps.midi;
      if (!supported)
        check(!list && list.error().code == ErrorCode::unsupported,
              "unsupported backend explicitly rejects enumeration");
      else if (!list) {
        // An implemented API still needs OS services, a driver and access.
        check(list.error().code == ErrorCode::not_found ||
                  list.error().code == ErrorCode::permission_denied ||
                  list.error().code == ErrorCode::unsupported,
              "native enumeration failure has explicit availability cause");
        std::cout << "device enumeration " << static_cast<int>(kind)
                  << ": unavailable " << static_cast<int>(list.error().code)
                  << '\n';
      }
      if (list)
        std::cout << "device enumeration " << static_cast<int>(kind) << ": "
                  << list->size() << '\n';
#if defined(_WIN32)
      if (kind == DeviceKind::usb)
        check(bool(list), "native Windows USB interface enumeration succeeds");
#endif
    }
#if defined(__APPLE__)
    auto outputs = devices.enumerate(DeviceKind::audio_output);
    if (outputs && !outputs->empty()) {
      auto audio = devices.open_audio(DeviceKind::audio_output);
      check(bool(audio), "open native default audio output");
      std::array<float, 128> silence{};
      auto written = devices.audio_write(*audio, silence);
      check(written && *written == silence.size(),
            "queue native float32 silence");
      check(bool(devices.close(*audio)), "audio session clean close");
    }
    const auto name = "iiSDKCore contract " +
                      std::to_string(Clock::now().time_since_epoch().count());
    std::atomic<bool> received = false;
    Endpoint input;
    Hardware receiver(loop, [&](HardwareEvent event) {
      if (event.endpoint == input && !event.midi.empty()) {
        auto note = protocol::note(event.midi.front());
        received = note && note->key == 60 && note->on;
      }
    });
    check(bool(devices.watch(DeviceKind::midi_output)),
          "watch CoreMIDI topology");
    bool removed = false, added = false;
    std::string watched_id;
    Hardware monitor(loop, [&](HardwareEvent event) {
      if (event.device.id == watched_id) {
        if (event.kind == HardwareEventKind::removed)
          removed = true;
        if (event.kind == HardwareEventKind::added)
          added = true;
      }
    });
    check(bool(monitor.watch(DeviceKind::midi_output)),
          "watch native addition and removal");
    std::this_thread::sleep_for(5ms);
    loop.poll({64, 20ms}); // Establish the pre-addition snapshot.
    auto made = receiver.create_midi_destination(name);
    check(bool(made), "create native MIDI destination");
    input = *made;
    auto destinations = devices.enumerate(DeviceKind::midi_output);
    check(bool(destinations), "enumerate virtual MIDI destination");
    auto found = std::find_if(
        destinations->begin(), destinations->end(),
        [&](const auto &d) { return d.name.find(name) != std::string::npos; });
    check(found != destinations->end(),
          "virtual endpoint visible to OS enumeration");
    watched_id = found->id;
    auto output = devices.open_midi(found->id);
    check(bool(output), "open native MIDI output");
    protocol::UMP packet;
    packet.size = 2;
    packet.words = {0x40903c00, 0xffff0000, 0, 0};
    check(bool(devices.midi_send(*output, std::span(&packet, 1))),
          "send native MIDI2 UMP");
    pump(loop, [&] { return received.load(); }, "native MIDI receive deadline");
    check(bool(devices.close(*output)) &&
              !devices.midi_send(*output, std::span(&packet, 1)),
          "closed MIDI endpoint rejects stale identity");
    pump(loop, [&] { return added; }, "native MIDI added hotplug deadline");
    check(bool(receiver.close(input)), "remove virtual MIDI destination");
    pump(loop, [&] { return removed; }, "native MIDI removed hotplug deadline");
    check(bool(receiver.close()), "receiver clean close");
    HardwareOptions one;
    one.event_capacity = one.dispatch_batch = 1;
    Hardware quota(loop, [](HardwareEvent) {}, one);
    for (int i = 0; i < 8; ++i) {
      auto endpoint = quota.create_midi_destination(name + " quota");
      check(bool(endpoint), "MIDI endpoint slot reusable");
      check(!quota.create_midi_destination(name + " overflow"),
            "native endpoint quota bounded");
      check(bool(quota.close(*endpoint)), "MIDI endpoint returns quota");
    }
    check(bool(quota.close()), "native endpoint quota clean close");
    check(bool(monitor.close()), "topology monitor clean close");
#endif
    const auto before = loop.stats().pending;
    check(bool(devices.close()) && loop.stats().pending + 1 == before,
          "hardware close returns dispatch slot");
    check(devices.enumerate(DeviceKind::usb).error().code ==
                  ErrorCode::closed &&
              devices.stats().pending == 0,
          "closed hardware clears events and rejects requests");
    check(bool(devices.close()), "hardware close idempotent");
#if defined(__APPLE__)
    // The last MIDI hub has retired its client and joined the native pump.
    // A subsequent hub must rebuild both without a stale process dispatcher.
    std::atomic<bool> reopened_input = false;
    bool reopened_added = false, reopened_removed = false;
    std::string reopened_id;
    Hardware reopened(loop, [&](HardwareEvent event) {
      if (!event.midi.empty())
        reopened_input = true;
      if (event.device.id == reopened_id) {
        if (event.kind == HardwareEventKind::added)
          reopened_added = true;
        if (event.kind == HardwareEventKind::removed)
          reopened_removed = true;
      }
    });
    check(bool(reopened.watch(DeviceKind::midi_output)),
          "MIDI topology after pump recreation");
    std::this_thread::sleep_for(5ms);
    loop.poll({64, 20ms});
    auto recreated = reopened.create_midi_destination(name + " reopened");
    check(bool(recreated), "MIDI hub reusable after complete close");
    auto after_close = reopened.enumerate(DeviceKind::midi_output);
    check(bool(after_close), "MIDI enumeration after complete close");
    auto recreated_device = std::find_if(
        after_close->begin(), after_close->end(), [&](const auto &device) {
          return device.name.find(name + " reopened") != std::string::npos;
        });
    check(recreated_device != after_close->end(),
          "recreated MIDI endpoint visible");
    reopened_id = recreated_device->id;
    auto reopened_output = reopened.open_midi(recreated_device->id);
    check(bool(reopened_output), "reopen native MIDI output");
    check(bool(reopened.midi_send(*reopened_output, std::span(&packet, 1))),
          "send MIDI after pump recreation");
    pump(
        loop, [&] { return reopened_input.load(); },
        "reopened MIDI receive deadline");
    pump(
        loop, [&] { return reopened_added; },
        "reopened MIDI addition deadline");
    check(bool(reopened.close(*reopened_output)) &&
              bool(reopened.close(*recreated)),
          "reopened MIDI endpoints clean close");
    pump(
        loop, [&] { return reopened_removed; },
        "reopened MIDI removal deadline");
    check(bool(reopened.close()), "reopened hardware clean close");
#endif
    std::cout << "native hardware contracts passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "hardware failure: " << e.what() << '\n';
    return 1;
  }
}
