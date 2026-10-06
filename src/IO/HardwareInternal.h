#pragma once
#include "../Runtime/TaskScope.h"
#include "Hardware.h"
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace ii::io::detail {
class HardwareState final {
public:
  HardwareState(HardwareHandler, HardwareOptions);
  bool publish(HardwareEvent);
  void dispatch();
  void retire();
  void activate(Endpoint);
  void deactivate(Endpoint);
  void await_endpoint(Endpoint);
  bool has_capacity() const;
  HardwareStats stats() const;
  const HardwareOptions options;
  const std::uint64_t owner;
  runtime::TaskScope callbacks;

private:
  mutable std::mutex mutex_;
  std::vector<std::optional<HardwareEvent>> events_;
  std::size_t begin_ = 0, count_ = 0;
  std::uint64_t delivered_ = 0, dropped_ = 0;
  bool closed_ = false;
  std::set<std::uint64_t> active_;
  std::set<std::uint64_t> closing_;
  std::map<std::uint64_t, std::size_t> executing_;
  std::condition_variable changed_;
  HardwareHandler handler_;
};
// One producer and one consumer; no allocation or locks in the audio callback.
class AudioRing final {
public:
  explicit AudioRing(std::size_t capacity);
  std::size_t write(std::span<const float>) noexcept;
  std::size_t read(std::span<float>) noexcept;

private:
  std::vector<float> data_;
  std::atomic<std::uint64_t> read_{0}, write_{0};
};
class NativeHardware final {
public:
  explicit NativeHardware(std::shared_ptr<HardwareState>);
  ~NativeHardware();
  Result<std::vector<DeviceInfo>> enumerate(DeviceKind);
  Result<void> watch(DeviceKind);
  Result<Endpoint> open(DeviceKind, std::string_view, AudioConfig = {});
  Result<std::vector<std::byte>> usb_control(Endpoint, protocol::USBSetup,
                                             std::span<const std::byte>,
                                             std::chrono::milliseconds);
  Result<std::vector<std::byte>> hid_descriptor(Endpoint);
  Result<std::vector<std::byte>> hid_get(Endpoint, protocol::HIDReportType,
                                         std::uint8_t, std::size_t);
  Result<void> hid_set(Endpoint, protocol::HIDReportType, std::uint8_t,
                       std::span<const std::byte>);
  Result<void> scan(std::span<const protocol::BluetoothUUID>);
  Result<void> stop_scan();
  Result<void> gatt(Endpoint, protocol::BluetoothUUID, protocol::BluetoothUUID,
                    int, std::span<const std::byte>, bool);
  Result<std::size_t> audio_write(Endpoint, std::span<const float>);
  Result<Endpoint> virtual_midi(std::string_view, bool);
  Result<void> midi_send(Endpoint, std::span<const protocol::UMP>);
  Result<void> close(Endpoint);
  void collect();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace ii::io::detail
