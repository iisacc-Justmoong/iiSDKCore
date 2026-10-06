#pragma once
#include "Channel.h"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ii::io::protocol {
struct USBSetup final {
  std::uint8_t request_type = 0, request = 0;
  std::uint16_t value = 0, index = 0, length = 0;
  [[nodiscard]] std::array<std::byte, 8> encode() const noexcept;
  [[nodiscard]] static Result<USBSetup> decode(std::span<const std::byte>);
};
struct USBDescriptor final {
  std::uint16_t usb_version = 0, vendor = 0, product = 0, device_version = 0;
  std::uint8_t device_class = 0, subclass = 0, protocol = 0, configurations = 0;
};
[[nodiscard]] Result<USBDescriptor>
    usb_device_descriptor(std::span<const std::byte>);
enum class HIDReportType { input, output, feature };
struct HIDField final {
  HIDReportType type = HIDReportType::input;
  std::uint8_t report_id = 0;
  std::uint32_t usage_page = 0, usage = 0, usage_max = 0;
  std::uint32_t bit_offset = 0, bit_size = 0, count = 0, flags = 0;
  std::int64_t logical_min = 0, logical_max = 0;
};
struct HIDValue final {
  std::size_t field = 0, element = 0;
  std::int64_t value = 0;
};
struct HIDLayout final {
  std::vector<HIDField> fields;
  bool numbered = false;
  [[nodiscard]] Result<std::vector<HIDValue>>
      decode(HIDReportType, std::span<const std::byte>) const;
  [[nodiscard]] Result<std::vector<std::byte>>
  encode(HIDReportType, std::uint8_t, std::span<const HIDValue>) const;
};
[[nodiscard]] Result<HIDLayout>
hid_report_descriptor(std::span<const std::byte>,
                      std::size_t max_fields = 4096);

struct BluetoothUUID final {
  std::array<std::uint8_t, 16> bytes{};
  [[nodiscard]] static Result<BluetoothUUID> parse(std::string_view);
  [[nodiscard]] std::string string() const;
  friend bool operator==(const BluetoothUUID &,
                         const BluetoothUUID &) = default;
};
struct Advertisement final {
  std::uint8_t type;
  std::vector<std::byte> data;
};
[[nodiscard]] Result<std::vector<Advertisement>>
    advertisements(std::span<const std::byte>);
struct HeartRate final {
  std::uint16_t beats_per_minute = 0;
  std::optional<bool> contact;
  std::optional<std::uint16_t> energy;
  std::vector<double> rr_seconds;
};
[[nodiscard]] Result<HeartRate> heart_rate(std::span<const std::byte>);
[[nodiscard]] Result<std::vector<std::byte>> pcm_s16le(std::span<const float>);
[[nodiscard]] Result<std::vector<float>> pcm_s16le(std::span<const std::byte>);

// Native-endian 32-bit UMP words; wire byte order is transport-specific.
struct UMP final {
  std::array<std::uint32_t, 4> words{};
  std::uint8_t size = 0;
  [[nodiscard]] std::uint8_t type() const noexcept { return words[0] >> 28; }
  [[nodiscard]] std::uint8_t group() const noexcept {
    return (words[0] >> 24) & 15;
  }
};
[[nodiscard]] Result<std::vector<UMP>>
    ump_packets(std::span<const std::uint32_t>);
struct Note final {
  std::uint8_t group = 0, channel = 0, key = 0;
  std::uint16_t velocity = 0;
  bool on = false, midi2 = false;
};
[[nodiscard]] Result<Note> note(const UMP &);
struct MIDI1Message final {
  std::vector<std::byte> bytes;
};
// Stateful byte stream decoder: running status, interleaved realtime, bounded
// SysEx.
class MIDI1Decoder final {
public:
  explicit MIDI1Decoder(std::size_t max_sysex = 65536);
  [[nodiscard]] Result<std::vector<MIDI1Message>>
      feed(std::span<const std::byte>);
  void reset() noexcept;

private:
  std::size_t maximum_;
  std::vector<std::byte> pending_;
  std::uint8_t running_ = 0, expected_ = 0;
  bool sysex_ = false;
};
[[nodiscard]] Result<UMP> midi1_ump(const MIDI1Message &,
                                    std::uint8_t group = 0);
// MIDI 1.0 channel/system UMP only; reject lossy MIDI 2.0 downconversion.
[[nodiscard]] Result<MIDI1Message> midi1_message(const UMP &);
} // namespace ii::io::protocol
