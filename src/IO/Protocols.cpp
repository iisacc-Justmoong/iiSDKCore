#include "Protocols.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace ii::io::protocol {
namespace {
auto invalid() {
  return std::unexpected(Error{ErrorCode::invalid_argument, {}});
}
unsigned byte(std::byte b) { return std::to_integer<unsigned>(b); }
std::uint16_t u16(std::span<const std::byte> b, std::size_t i) {
  return byte(b[i]) | (byte(b[i + 1]) << 8);
}
std::int64_t signed_value(std::uint64_t value, unsigned bits) {
  if (bits == 64)
    return std::bit_cast<std::int64_t>(value);
  if (bits && (value & (std::uint64_t(1) << (bits - 1))))
    return std::bit_cast<std::int64_t>(value | (~std::uint64_t(0) << bits));
  return static_cast<std::int64_t>(value);
}
unsigned ump_size(unsigned type) {
  constexpr unsigned sizes[]{1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
  return sizes[type & 15];
}
unsigned midi_size(unsigned status) {
  if (status < 0x80)
    return 0;
  if (status < 0xf0)
    return (status & 0xe0) == 0xc0 ? 2 : 3;
  switch (status) {
  case 0xf1:
  case 0xf3:
    return 2;
  case 0xf2:
    return 3;
  case 0xf6:
  case 0xf7:
  case 0xf8:
  case 0xfa:
  case 0xfb:
  case 0xfc:
  case 0xfe:
  case 0xff:
    return 1;
  default:
    return 0;
  }
}
} // namespace
std::array<std::byte, 8> USBSetup::encode() const noexcept {
  return {std::byte(request_type), std::byte(request),
          std::byte(value & 255),  std::byte(value >> 8),
          std::byte(index & 255),  std::byte(index >> 8),
          std::byte(length & 255), std::byte(length >> 8)};
}
Result<USBSetup> USBSetup::decode(std::span<const std::byte> data) {
  if (data.size() != 8)
    return invalid();
  return USBSetup{static_cast<std::uint8_t>(byte(data[0])),
                  static_cast<std::uint8_t>(byte(data[1])), u16(data, 2),
                  u16(data, 4), u16(data, 6)};
}
Result<USBDescriptor> usb_device_descriptor(std::span<const std::byte> b) {
  if (b.size() < 18 || byte(b[0]) != 18 || byte(b[1]) != 1)
    return invalid();
  return USBDescriptor{u16(b, 2),
                       u16(b, 8),
                       u16(b, 10),
                       u16(b, 12),
                       static_cast<std::uint8_t>(byte(b[4])),
                       static_cast<std::uint8_t>(byte(b[5])),
                       static_cast<std::uint8_t>(byte(b[6])),
                       static_cast<std::uint8_t>(byte(b[17]))};
}
Result<HIDLayout> hid_report_descriptor(std::span<const std::byte> bytes,
                                        std::size_t maximum) {
  if (bytes.empty() || maximum == 0 || maximum > 65536)
    return invalid();
  struct Global final {
    std::uint32_t page = 0, size = 0, count = 0;
    std::uint8_t id = 0;
    std::int64_t min = 0, max = 0;
  } global;
  std::vector<Global> stack;
  std::vector<std::uint32_t> usages;
  std::optional<std::uint32_t> first, last;
  std::map<std::pair<HIDReportType, std::uint8_t>, std::uint32_t> offsets;
  HIDLayout layout;
  unsigned collections = 0;
  for (std::size_t position = 0; position < bytes.size();) {
    const auto prefix = byte(bytes[position++]);
    if (prefix == 0xfe) {
      if (bytes.size() - position < 2)
        return invalid();
      const auto length = byte(bytes[position]);
      position += 2;
      if (length > bytes.size() - position)
        return invalid();
      position += length;
      continue;
    }
    const auto size = (prefix & 3) == 3 ? 4 : prefix & 3;
    if (size > bytes.size() - position)
      return invalid();
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
      value |= byte(bytes[position++]) << (8 * i);
    const auto tag = prefix >> 4, type = (prefix >> 2) & 3;
    if (type == 1) {
      switch (tag) {
      case 0:
        global.page = value;
        break;
      case 1:
        global.min = signed_value(value, size * 8);
        break;
      case 2:
        global.max = global.min < 0 ? signed_value(value, size * 8) : value;
        break;
      case 7:
        global.size = value;
        break;
      case 8:
        if (value == 0 || value > 255)
          return invalid();
        global.id = value;
        layout.numbered = true;
        break;
      case 9:
        global.count = value;
        break;
      case 10:
        if (stack.size() >= 64)
          return invalid();
        stack.push_back(global);
        break;
      case 11:
        if (stack.empty())
          return invalid();
        global = stack.back();
        stack.pop_back();
        break;
      default:
        break;
      }
    } else if (type == 2) {
      const auto usage =
          size == 4 ? value : (global.page << 16) | (value & 65535);
      if (tag == 0) {
        if (usages.size() >= maximum)
          return invalid();
        usages.push_back(usage);
      } else if (tag == 1)
        first = usage;
      else if (tag == 2)
        last = usage;
    } else if (type == 0) {
      if (tag == 10) {
        if (++collections > 64)
          return invalid();
      } else if (tag == 12) {
        if (!collections)
          return invalid();
        --collections;
      } else if (tag == 8 || tag == 9 || tag == 11) {
        if (global.size == 0 || global.size > 64 || global.count == 0 ||
            global.count > 65536 || global.min > global.max ||
            (first && last && *first > *last))
          return invalid();
        auto report = tag == 8   ? HIDReportType::input
                      : tag == 9 ? HIDReportType::output
                                 : HIDReportType::feature;
        auto &offset = offsets[{report, global.id}];
        const auto bits = std::uint64_t(global.size) * global.count;
        if (bits > 65536 || offset + bits > 65536)
          return invalid();
        if (layout.fields.size() >= maximum)
          return invalid();
        // Variable fields may have distinct usages; array fields retain their
        // selector range.
        if (value & 2) {
          if (global.count > maximum - layout.fields.size())
            return invalid();
          for (std::uint32_t i = 0; i < global.count; ++i) {
            const auto usage =
                i < usages.size() ? usages[i]
                : first ? std::min<std::uint64_t>(std::uint64_t(*first) + i,
                                                  last.value_or(*first))
                : usages.empty() ? global.page << 16
                                 : usages.back();
            layout.fields.push_back({report, global.id,
                                     static_cast<std::uint32_t>(usage >> 16),
                                     static_cast<std::uint32_t>(usage & 65535),
                                     static_cast<std::uint32_t>(usage & 65535),
                                     offset + i * global.size, global.size, 1,
                                     value, global.min, global.max});
          }
        } else {
          const auto usage = first.value_or(usages.empty() ? global.page << 16
                                                           : usages.front());
          layout.fields.push_back({report, global.id, usage >> 16,
                                   usage & 65535, last.value_or(usage) & 65535,
                                   offset, global.size, global.count, value,
                                   global.min, global.max});
        }
        offset += static_cast<std::uint32_t>(bits);
      }
      usages.clear();
      first.reset();
      last.reset();
    }
  }
  if (collections || !stack.empty() || layout.fields.empty())
    return invalid();
  if (layout.numbered &&
      std::any_of(layout.fields.begin(), layout.fields.end(),
                  [](const auto &f) { return f.report_id == 0; }))
    return invalid();
  return layout;
}
Result<std::vector<HIDValue>>
HIDLayout::decode(HIDReportType type, std::span<const std::byte> report) const {
  if (numbered && report.empty())
    return invalid();
  const auto id = numbered ? byte(report[0]) : 0;
  if (numbered)
    report = report.subspan(1);
  std::vector<HIDValue> result;
  bool found = false;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto &f = fields[index];
    if (f.type != type || f.report_id != id)
      continue;
    found = true;
    if (f.bit_size == 0 || f.bit_size > 64 || !f.count || f.count > 65536 ||
        std::uint64_t(f.bit_offset) + std::uint64_t(f.bit_size) * f.count >
            65536 ||
        std::uint64_t(f.bit_offset) + std::uint64_t(f.bit_size) * f.count >
            report.size() * 8ULL)
      return invalid();
    if (f.flags & 1)
      continue;
    for (std::uint32_t i = 0; i < f.count; ++i) {
      std::uint64_t value = 0;
      for (unsigned bit = 0; bit < f.bit_size; ++bit) {
        const auto offset = f.bit_offset + i * f.bit_size + bit;
        value |= std::uint64_t((byte(report[offset / 8]) >> (offset % 8)) & 1)
                 << bit;
      }
      if (f.logical_min >= 0 && f.bit_size == 64 &&
          value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        return invalid();
      result.push_back({index, i,
                        f.logical_min < 0 ? signed_value(value, f.bit_size)
                                          : static_cast<std::int64_t>(value)});
    }
  }
  if (!found)
    return invalid();
  return result;
}
Result<std::vector<std::byte>>
HIDLayout::encode(HIDReportType type, std::uint8_t id,
                  std::span<const HIDValue> values) const {
  std::uint64_t bits = 0;
  bool found = false;
  if (!numbered && id != 0)
    return invalid();
  for (const auto &f : fields)
    if (f.type == type && f.report_id == id) {
      if (f.bit_size == 0 || f.bit_size > 64)
        return invalid();
      found = true;
      bits = std::max(bits, std::uint64_t(f.bit_offset) +
                                std::uint64_t(f.bit_size) * f.count);
    }
  if (!found || bits > 65536)
    return invalid();
  std::vector<std::byte> output((bits + 7) / 8 + (numbered ? 1 : 0));
  if (numbered)
    output[0] = std::byte(id);
  std::vector<std::pair<std::size_t, std::size_t>> assigned;
  for (const auto &v : values) {
    if (v.field >= fields.size())
      return invalid();
    const auto &f = fields[v.field];
    if (f.type != type || f.report_id != id || v.element >= f.count ||
        (f.flags & 1) || v.value < f.logical_min || v.value > f.logical_max)
      return invalid();
    if (f.bit_size < 64) {
      if (f.logical_min < 0) {
        const auto limit = std::int64_t(1) << (f.bit_size - 1);
        if (v.value < -limit || v.value >= limit)
          return invalid();
      } else if (v.value < 0 || static_cast<std::uint64_t>(v.value) >
                                    ((std::uint64_t(1) << f.bit_size) - 1))
        return invalid();
    }
    if (std::find(assigned.begin(), assigned.end(),
                  std::pair{v.field, v.element}) != assigned.end())
      return invalid();
    assigned.emplace_back(v.field, v.element);
    const auto value = static_cast<std::uint64_t>(v.value);
    for (unsigned bit = 0; bit < f.bit_size; ++bit) {
      const auto offset = f.bit_offset + v.element * f.bit_size + bit;
      if ((value >> bit) & 1)
        output[offset / 8 + (numbered ? 1 : 0)] |=
            std::byte(1u << (offset % 8));
    }
  }
  return output;
}
Result<BluetoothUUID> BluetoothUUID::parse(std::string_view text) {
  std::string hex;
  if (text.size() == 36) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (text[i] != '-')
          return invalid();
      } else
        hex += text[i];
    }
  } else if (text.size() == 4)
    hex = "0000" + std::string(text) + "00001000800000805f9b34fb";
  else if (text.size() == 8)
    hex = std::string(text) + "00001000800000805f9b34fb";
  else
    return invalid();
  BluetoothUUID result;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 16; ++i) {
    const int a = digit(hex[i * 2]), b = digit(hex[i * 2 + 1]);
    if (a < 0 || b < 0)
      return invalid();
    result.bytes[i] = a * 16 + b;
  }
  return result;
}
std::string BluetoothUUID::string() const {
  constexpr char hex[] = "0123456789abcdef";
  std::string output;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      output += '-';
    output += hex[bytes[i] >> 4];
    output += hex[bytes[i] & 15];
  }
  return output;
}
Result<std::vector<Advertisement>>
advertisements(std::span<const std::byte> data) {
  std::vector<Advertisement> result;
  for (std::size_t i = 0; i < data.size();) {
    const auto length = byte(data[i++]);
    if (!length) {
      if (std::any_of(data.begin() + i, data.end(),
                      [](auto b) { return b != std::byte{}; }))
        return invalid();
      break;
    }
    if (length > data.size() - i)
      return invalid();
    result.push_back({static_cast<std::uint8_t>(byte(data[i])),
                      {data.begin() + i + 1, data.begin() + i + length}});
    i += length;
  }
  return result;
}
Result<HeartRate> heart_rate(std::span<const std::byte> b) {
  if (b.size() < 2)
    return invalid();
  const auto flags = byte(b[0]);
  if (flags & 0xe0)
    return invalid();
  std::size_t offset = 1;
  HeartRate result;
  if (flags & 1) {
    if (b.size() < 3)
      return invalid();
    result.beats_per_minute = u16(b, offset);
    offset += 2;
  } else
    result.beats_per_minute = byte(b[offset++]);
  if (flags & 4)
    result.contact = bool(flags & 2);
  if (flags & 8) {
    if (b.size() - offset < 2)
      return invalid();
    result.energy = u16(b, offset);
    offset += 2;
  }
  if (flags & 16) {
    if (b.size() == offset || (b.size() - offset) % 2)
      return invalid();
    while (offset < b.size()) {
      result.rr_seconds.push_back(u16(b, offset) / 1024.0);
      offset += 2;
    }
  }
  if (offset != b.size())
    return invalid();
  return result;
}
Result<std::vector<std::byte>> pcm_s16le(std::span<const float> samples) {
  std::vector<std::byte> output;
  output.reserve(samples.size() * 2);
  for (float sample : samples) {
    if (!std::isfinite(sample))
      return invalid();
    const auto value = static_cast<std::int16_t>(std::lrint(
        std::clamp(sample, -1.0f, 1.0f) * (sample < 0 ? 32768.0f : 32767.0f)));
    output.push_back(std::byte(static_cast<std::uint16_t>(value) & 255));
    output.push_back(std::byte(static_cast<std::uint16_t>(value) >> 8));
  }
  return output;
}
Result<std::vector<float>> pcm_s16le(std::span<const std::byte> bytes) {
  if (bytes.size() % 2)
    return invalid();
  std::vector<float> result;
  result.reserve(bytes.size() / 2);
  for (std::size_t i = 0; i < bytes.size(); i += 2) {
    const auto sample = static_cast<std::int16_t>(u16(bytes, i));
    result.push_back(sample / (sample < 0 ? 32768.0f : 32767.0f));
  }
  return result;
}
Result<std::vector<UMP>> ump_packets(std::span<const std::uint32_t> words) {
  std::vector<UMP> result;
  while (!words.empty()) {
    const auto size = ump_size(words[0] >> 28);
    if (words.size() < size)
      return invalid();
    UMP packet;
    packet.size = size;
    std::copy_n(words.begin(), size, packet.words.begin());
    result.push_back(packet);
    words = words.subspan(size);
  }
  return result;
}
Result<Note> note(const UMP &p) {
  if (p.size != ump_size(p.type()) || (p.type() != 2 && p.type() != 4))
    return invalid();
  const auto status = (p.words[0] >> 20) & 15;
  if ((status != 8 && status != 9) || ((p.words[0] >> 8) & 255) > 127)
    return invalid();
  const auto velocity = p.type() == 4 ? p.words[1] >> 16 : p.words[0] & 255;
  if (p.type() == 2 && velocity > 127)
    return invalid();
  return Note{p.group(),
              static_cast<std::uint8_t>((p.words[0] >> 16) & 15),
              static_cast<std::uint8_t>((p.words[0] >> 8) & 127),
              static_cast<std::uint16_t>(velocity),
              status == 9 && velocity != 0,
              p.type() == 4};
}
MIDI1Decoder::MIDI1Decoder(std::size_t maximum) : maximum_(maximum) {
  if (maximum < 2 || maximum > 1024 * 1024)
    throw std::invalid_argument("invalid SysEx capacity");
}
void MIDI1Decoder::reset() noexcept {
  pending_.clear();
  running_ = expected_ = 0;
  sysex_ = false;
}
Result<std::vector<MIDI1Message>>
MIDI1Decoder::feed(std::span<const std::byte> bytes) {
  std::vector<MIDI1Message> result;
  auto fail = [&]() -> Result<std::vector<MIDI1Message>> {
    reset();
    return invalid();
  };
  for (auto b : bytes) {
    const auto value = byte(b);
    if (value >= 0xf8) {
      if (midi_size(value))
        result.push_back({{b}});
      continue;
    }
    if (sysex_) {
      if (value < 0x80 || value == 0xf7) {
        if (pending_.size() >= maximum_)
          return fail();
        pending_.push_back(b);
        if (value == 0xf7) {
          result.push_back({std::move(pending_)});
          pending_.clear();
          sysex_ = false;
        }
        continue;
      }
      pending_.clear();
      sysex_ = false;
    }
    if (value >= 0x80) {
      pending_.clear();
      if (value >= 0xf0)
        running_ = 0;
      if (value == 0xf4 || value == 0xf5)
        continue;
      if (value == 0xf0) {
        sysex_ = true;
        pending_.push_back(b);
        continue;
      }
      expected_ = midi_size(value);
      if (!expected_)
        return fail();
      if (value < 0xf0)
        running_ = value;
      pending_.push_back(b);
    } else {
      if (pending_.empty()) {
        if (!running_)
          return fail();
        pending_.push_back(std::byte(running_));
        expected_ = midi_size(running_);
      }
      pending_.push_back(b);
    }
    if (pending_.size() == expected_) {
      result.push_back({std::move(pending_)});
      pending_.clear();
    }
  }
  return result;
}
Result<UMP> midi1_ump(const MIDI1Message &message, std::uint8_t group) {
  if (group > 15 || message.bytes.empty())
    return invalid();
  const auto status = byte(message.bytes[0]);
  if (message.bytes.size() != midi_size(status))
    return invalid();
  for (std::size_t i = 1; i < message.bytes.size(); ++i)
    if (byte(message.bytes[i]) > 127)
      return invalid();
  UMP p;
  p.size = 1;
  p.words[0] = ((status < 0xf0 ? 2u : 1u) << 28) |
               (std::uint32_t(group) << 24) | (status << 16);
  if (message.bytes.size() > 1)
    p.words[0] |= byte(message.bytes[1]) << 8;
  if (message.bytes.size() > 2)
    p.words[0] |= byte(message.bytes[2]);
  return p;
}
Result<MIDI1Message> midi1_message(const UMP &packet) {
  if (packet.size != 1 || (packet.type() != 1 && packet.type() != 2))
    return std::unexpected(Error{ErrorCode::unsupported, {}});
  // A MIDI 1.0 byte port has no group addressing. Reject loss of that field.
  if (packet.group() != 0)
    return std::unexpected(Error{ErrorCode::unsupported, {}});
  const auto status = static_cast<std::uint8_t>(packet.words[0] >> 16);
  const auto size = midi_size(status);
  if (!size || (status < 0xf0 ? 2u : 1u) != packet.type())
    return invalid();
  MIDI1Message message;
  message.bytes.push_back(std::byte(status));
  if (size > 1)
    message.bytes.push_back(std::byte((packet.words[0] >> 8) & 255));
  if (size > 2)
    message.bytes.push_back(std::byte(packet.words[0] & 255));
  const auto encoded = midi1_ump(message);
  if (!encoded || encoded->words[0] != packet.words[0])
    return invalid();
  return message;
}
} // namespace ii::io::protocol
