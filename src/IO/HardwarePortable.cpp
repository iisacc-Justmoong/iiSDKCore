#include "HardwareInternal.h"
namespace ii::io {
HardwareCapabilities hardware_capabilities() noexcept { return {}; }
namespace detail {
namespace {
auto unsupported() {
  return std::unexpected(Error{ErrorCode::unsupported, {}});
}
} // namespace
struct NativeHardware::Impl {};
NativeHardware::NativeHardware(std::shared_ptr<HardwareState>)
    : impl_(std::make_unique<Impl>()) {}
NativeHardware::~NativeHardware() = default;
Result<std::vector<DeviceInfo>> NativeHardware::enumerate(DeviceKind) {
  return unsupported();
}
Result<void> NativeHardware::watch(DeviceKind) { return unsupported(); }
Result<Endpoint> NativeHardware::open(DeviceKind, std::string_view,
                                      AudioConfig) {
  return unsupported();
}
Result<std::vector<std::byte>>
NativeHardware::usb_control(Endpoint, protocol::USBSetup,
                            std::span<const std::byte>,
                            std::chrono::milliseconds) {
  return unsupported();
}
Result<std::vector<std::byte>> NativeHardware::hid_descriptor(Endpoint) {
  return unsupported();
}
Result<std::vector<std::byte>> NativeHardware::hid_get(Endpoint,
                                                       protocol::HIDReportType,
                                                       std::uint8_t,
                                                       std::size_t) {
  return unsupported();
}
Result<void> NativeHardware::hid_set(Endpoint, protocol::HIDReportType,
                                     std::uint8_t, std::span<const std::byte>) {
  return unsupported();
}
Result<void> NativeHardware::scan(std::span<const protocol::BluetoothUUID>) {
  return unsupported();
}
Result<void> NativeHardware::stop_scan() { return unsupported(); }
Result<void> NativeHardware::gatt(Endpoint, protocol::BluetoothUUID,
                                  protocol::BluetoothUUID, int,
                                  std::span<const std::byte>, bool) {
  return unsupported();
}
Result<std::size_t> NativeHardware::audio_write(Endpoint,
                                                std::span<const float>) {
  return unsupported();
}
Result<Endpoint> NativeHardware::virtual_midi(std::string_view, bool) {
  return unsupported();
}
Result<void> NativeHardware::midi_send(Endpoint,
                                       std::span<const protocol::UMP>) {
  return unsupported();
}
Result<void> NativeHardware::close(Endpoint) {
  return std::unexpected(Error{ErrorCode::closed, {}});
}
void NativeHardware::collect() {}
} // namespace detail
} // namespace ii::io
