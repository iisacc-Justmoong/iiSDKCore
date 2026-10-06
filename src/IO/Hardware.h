#pragma once
#include "../Runtime/EventLoop.h"
#include "Protocols.h"
#include <functional>

namespace ii::io {
enum class DeviceKind {
  usb,
  hid,
  bluetooth,
  audio_input,
  audio_output,
  midi_input,
  midi_output
};
struct DeviceInfo final {
  DeviceKind kind = DeviceKind::usb;
  std::string id, name, manufacturer, serial;
  std::uint16_t vendor = 0, product = 0;
  std::uint32_t usage_page = 0, usage = 0;
  friend bool operator==(const DeviceInfo &, const DeviceInfo &) = default;
};
// An identity owned by one Hardware instance, never an OS handle.
struct Endpoint final {
  std::uint64_t owner = 0, value = 0;
  friend bool operator==(const Endpoint &, const Endpoint &) = default;
};
enum class HardwareEventKind {
  added,
  removed,
  connected,
  disconnected,
  input,
  characteristic,
  failure
};
struct HardwareEvent final {
  HardwareEventKind kind = HardwareEventKind::input;
  DeviceInfo device;
  Endpoint endpoint;
  std::string service, characteristic;
  std::vector<std::byte> bytes;
  std::vector<float> samples;
  std::vector<protocol::UMP> midi;
  std::optional<Error> error;
};
struct HardwareOptions final {
  std::size_t event_capacity = 256, max_payload_bytes = 65536,
              dispatch_batch = 32;
  runtime::Clock::duration dispatch_interval = std::chrono::milliseconds(2);
  // Driver-defined WinUSB interface GUIDs, in canonical 128-bit text form.
  std::vector<std::string> usb_interface_guids;
};
struct HardwareStats final {
  std::size_t pending = 0;
  std::uint64_t delivered = 0, dropped = 0;
};
struct HardwareCapabilities final {
  bool usb = false, hid = false, hotplug = false, bluetooth = false,
       audio = false, midi = false, midi2 = false;
};
[[nodiscard]] HardwareCapabilities hardware_capabilities() noexcept;
struct AudioConfig final {
  double sample_rate = 48000;
  std::uint32_t channels = 2, frames_per_buffer = 256, queued_frames = 8192;
};
using HardwareHandler = std::function<void(HardwareEvent)>;

// Native callbacks only enqueue bounded events. User callbacks run on
// EventLoop. Close outside this object's callback; the borrowed loop outlives
// Hardware.
class Hardware final {
public:
  Hardware(runtime::EventLoop &, HardwareHandler, HardwareOptions options = {});
  ~Hardware();
  Hardware(const Hardware &) = delete;
  Hardware &operator=(const Hardware &) = delete;
  [[nodiscard]] Result<std::vector<DeviceInfo>> enumerate(DeviceKind);
  [[nodiscard]] Result<void> watch(DeviceKind);
  [[nodiscard]] Result<Endpoint> open_usb(std::string_view id);
  [[nodiscard]] Result<Endpoint> open_hid(std::string_view id);
  [[nodiscard]] Result<std::vector<std::byte>>
  usb_control(Endpoint, protocol::USBSetup,
              std::span<const std::byte> output = {},
              std::chrono::milliseconds timeout = std::chrono::seconds(1));
  [[nodiscard]] Result<std::vector<std::byte>> hid_descriptor(Endpoint);
  [[nodiscard]] Result<std::vector<std::byte>> hid_get(Endpoint,
                                                       protocol::HIDReportType,
                                                       std::uint8_t id,
                                                       std::size_t length);
  [[nodiscard]] Result<void> hid_set(Endpoint, protocol::HIDReportType,
                                     std::uint8_t id,
                                     std::span<const std::byte>);
  [[nodiscard]] Result<void>
  scan_bluetooth(std::span<const protocol::BluetoothUUID> services);
  [[nodiscard]] Result<void> stop_bluetooth_scan();
  [[nodiscard]] Result<Endpoint> connect_bluetooth(std::string_view id);
  [[nodiscard]] Result<void> gatt_read(Endpoint,
                                       protocol::BluetoothUUID service,
                                       protocol::BluetoothUUID characteristic);
  [[nodiscard]] Result<void> gatt_write(Endpoint,
                                        protocol::BluetoothUUID service,
                                        protocol::BluetoothUUID characteristic,
                                        std::span<const std::byte>,
                                        bool response = true);
  [[nodiscard]] Result<void> gatt_notify(Endpoint,
                                         protocol::BluetoothUUID service,
                                         protocol::BluetoothUUID characteristic,
                                         bool enabled = true);
  // Interleaved float32 PCM. Empty id selects the OS default endpoint.
  [[nodiscard]] Result<Endpoint>
  open_audio(DeviceKind, std::string_view id = {}, AudioConfig config = {});
  [[nodiscard]] Result<std::size_t> audio_write(Endpoint,
                                                std::span<const float>);
  [[nodiscard]] Result<Endpoint> open_midi(std::string_view id);
  [[nodiscard]] Result<Endpoint> create_midi_source(std::string_view name);
  [[nodiscard]] Result<Endpoint> create_midi_destination(std::string_view name);
  [[nodiscard]] Result<void> midi_send(Endpoint,
                                       std::span<const protocol::UMP>);
  [[nodiscard]] Result<void> close(Endpoint);
  [[nodiscard]] Result<void> close();
  [[nodiscard]] HardwareStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace ii::io
