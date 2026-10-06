#include <cmath>
#include <iiSDKCore/IO/Protocols.h>
#include <iostream>
#include <stdexcept>
using namespace ii::io;
using namespace ii::io::protocol;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
  std::vector<std::byte> out;
  for (auto v : values)
    out.push_back(std::byte(v));
  return out;
}
void usb() {
  USBSetup setup{0xa1, 1, 0x1234, 0x5678, 0xabcd};
  auto encoded = setup.encode();
  check(encoded[2] == std::byte{0x34} && encoded[7] == std::byte{0xab},
        "USB LE setup");
  auto parsed = USBSetup::decode(encoded);
  check(parsed && parsed->value == setup.value &&
            parsed->length == setup.length,
        "USB setup roundtrip");
  check(!USBSetup::decode(std::span(encoded).first(7)), "USB setup truncation");
  auto d = bytes(
      {18, 1, 0, 3, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 1, 2, 1, 2, 3, 1});
  auto descriptor = usb_device_descriptor(d);
  check(descriptor && descriptor->vendor == 0x1234 &&
            descriptor->usb_version == 0x300,
        "USB descriptor");
  d[0] = std::byte{17};
  check(!usb_device_descriptor(d), "USB descriptor length");
}
void hid() {
  auto mouse =
      bytes({0x05, 1,    0x09, 2,    0xa1, 1, 0x85, 7, 0x05, 9,    0x19, 1,
             0x29, 3,    0x15, 0,    0x25, 1, 0x75, 1, 0x95, 3,    0x81, 2,
             0x75, 5,    0x95, 1,    0x81, 1, 0x05, 1, 0x09, 0x30, 0x09, 0x31,
             0x15, 0x81, 0x25, 0x7f, 0x75, 8, 0x95, 2, 0x81, 6,    0xc0});
  auto layout = hid_report_descriptor(mouse);
  check(layout && layout->numbered, "HID mouse descriptor");
  auto report = bytes({7, 5, 0xfb, 12});
  auto values = layout->decode(HIDReportType::input, report);
  check(values && values->size() == 5 && values->at(3).value == -5 &&
            values->at(4).value == 12,
        "HID signed packed fields");
  auto encoded = layout->encode(HIDReportType::input, 7, *values);
  check(encoded && *encoded == report, "HID report encode");
  check(!layout->decode(HIDReportType::input, std::span(report).first(3)),
        "HID report truncation");
  check(!layout->decode(HIDReportType::output, report),
        "HID wrong report type");
  auto bad = *values;
  bad[3].value = 128;
  check(!layout->encode(HIDReportType::input, 7, bad), "HID logical range");
  bad = *values;
  bad.push_back(bad.front());
  check(!layout->encode(HIDReportType::input, 7, bad),
        "HID duplicate assignment");
  check(!hid_report_descriptor(bytes({0xb4})), "HID global pop underflow");
  check(!hid_report_descriptor(bytes({0xfe, 8, 1, 0})),
        "HID long item truncation");
  check(!hid_report_descriptor(mouse, 2), "HID bounded field count");
  for (std::size_t i = 1; i < mouse.size(); ++i) {
    auto prefix = std::span(mouse).first(i);
    auto p = hid_report_descriptor(prefix);
    if (p)
      check(!p->fields.empty(),
            "HID truncated input never fabricates empty success");
  }
}
void bluetooth() {
  auto a = BluetoothUUID::parse("180D"),
       b = BluetoothUUID::parse("0000180d-0000-1000-8000-00805f9b34fb");
  check(a && b && *a == *b && a->string() == b->string(),
        "Bluetooth UUID canonical form");
  check(!BluetoothUUID::parse("180z") &&
            !BluetoothUUID::parse("0000180d_0000-1000-8000-00805f9b34fb"),
        "invalid UUID");
  auto ad = advertisements(bytes({2, 1, 6, 3, 3, 0x0d, 0x18, 0, 0}));
  check(ad && ad->size() == 2 && ad->at(1).type == 3, "BLE AD structures");
  check(!advertisements(bytes({4, 1, 2})), "BLE AD truncation");
  auto hr = heart_rate(bytes({0x1f, 0x2c, 1, 0x34, 0x12, 0, 4, 0, 2}));
  check(hr && hr->beats_per_minute == 300 && hr->contact == true &&
            hr->energy == 0x1234 &&
            hr->rr_seconds == std::vector<double>{1, 0.5},
        "standard heart rate profile");
  check(!heart_rate(bytes({1, 60})) && !heart_rate(bytes({0, 60, 1})),
        "heart rate malformed flags/length");
}
void pcm() {
  const std::array<float, 5> input{-1, -0.5f, 0, 0.5f, 1};
  auto encoded = pcm_s16le(input);
  check(encoded && encoded->front() == std::byte{0} &&
            encoded->at(1) == std::byte{0x80},
        "PCM signed LE");
  auto decoded = pcm_s16le(*encoded);
  check(decoded && decoded->front() == -1 && decoded->back() == 1 &&
            std::abs(decoded->at(3) - 0.5f) < 0.00004f,
        "PCM roundtrip");
  check(!pcm_s16le(bytes({0})), "PCM partial sample");
  const std::array<float, 1> nan{NAN};
  check(!pcm_s16le(nan), "PCM rejects NaN");
}
void edges() {
  HIDLayout layout;
  layout.fields.push_back(
      {HIDReportType::output, 0, 1, 1, 1, 0, 1, 1, 2, 0, 100});
  HIDValue value{0, 0, 2};
  check(!layout.encode(HIDReportType::output, 0, std::span(&value, 1)),
        "HID value must fit actual bit width");
  layout.fields[0].bit_size = 63;
  layout.fields[0].logical_min = INT64_MIN;
  layout.fields[0].logical_max = INT64_MAX;
  auto decoded =
      layout.decode(HIDReportType::output,
                    bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}));
  check(decoded && decoded->front().value == -1, "HID 63 bit sign extension");
  value.value = INT64_MIN;
  check(!layout.encode(HIDReportType::output, 0, std::span(&value, 1)),
        "HID signed width range");
  MIDI1Decoder decoder;
  auto aborted = decoder.feed(bytes({0x90, 60, 0x80, 61, 64}));
  check(aborted && aborted->size() == 1 &&
            aborted->front().bytes == bytes({0x80, 61, 64}),
        "MIDI status aborts incomplete message");
  aborted = decoder.feed(bytes({0xf0, 1, 0xfd, 0x90, 60, 127}));
  check(aborted && aborted->size() == 1,
        "MIDI status aborts SysEx and reserved realtime ignored");
}
void midi() {
  const std::array<std::uint32_t, 3> words{0x20903c7f, 0x41913d00, 0x80000000};
  auto packets = ump_packets(words);
  check(packets && packets->size() == 2, "UMP packet framing");
  auto first = note(packets->front()), second = note(packets->back());
  check(first && first->on && first->key == 60 && !first->midi2 && second &&
            second->midi2 && second->velocity == 32768 && second->group == 1,
        "MIDI 1/2 note interpretation");
  check(!ump_packets(std::span(words).subspan(1, 1)), "UMP partial packet");
  MIDI1Decoder decoder(8);
  auto prefix = decoder.feed(bytes({0x90, 60, 0xf8}));
  check(prefix && prefix->size() == 1 &&
            prefix->front().bytes[0] == std::byte{0xf8},
        "interleaved MIDI realtime");
  auto rest =
      decoder.feed(bytes({100, 61, 0, 0xc0, 4, 0xf0, 1, 2, 0xfa, 0xf7}));
  check(rest && rest->size() == 5, "MIDI running status and SysEx");
  auto up = midi1_ump(rest->front(), 3);
  check(up && up->words[0] == 0x23903c64, "MIDI1 UMP conversion");
  check(!midi1_message(*up), "byte port rejects group loss");
  const auto zero_group = midi1_ump(rest->front());
  auto back = midi1_message(*zero_group);
  check(back && back->bytes == rest->front().bytes, "MIDI1 UMP round trip");
  for (auto status : {0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf1, 0xf2,
                      0xf3, 0xf6, 0xf8, 0xfa, 0xfb, 0xfc, 0xfe, 0xff}) {
    const unsigned length = status < 0xf0    ? ((status & 0xe0) == 0xc0 ? 2 : 3)
                            : status == 0xf2 ? 3
                            : status == 0xf1 || status == 0xf3 ? 2
                                                               : 1;
    MIDI1Message original{bytes({static_cast<unsigned>(status)})};
    if (length > 1)
      original.bytes.push_back(std::byte(12));
    if (length > 2)
      original.bytes.push_back(std::byte(20));
    auto encoded = midi1_ump(original);
    auto decoded = encoded
                       ? midi1_message(*encoded)
                       : Result<MIDI1Message>(std::unexpected(encoded.error()));
    check(decoded && decoded->bytes == original.bytes,
          "channel/system MIDI1 round trip");
  }
  UMP lossy{{0x40903c00, 0xffff0000, 0, 0}, 2};
  check(!midi1_message(lossy), "reject lossy MIDI2 conversion");
  UMP malformed{{0x2090ff00, 0, 0, 0}, 1};
  check(!midi1_message(malformed), "reject invalid MIDI data bits");
  check(!midi1_ump(rest->back()), "SysEx requires explicit UMP segmentation");
  check(!decoder.feed(bytes({1})), "no running status after SysEx");
  check(!decoder.feed(bytes({0xf0, 1, 2, 3, 4, 5, 6, 7, 8})), "bounded SysEx");
  decoder.reset();
  check(decoder.feed(bytes({0x90, 60, 100})).has_value(),
        "MIDI reset after malformed input");
}
} // namespace
int main() {
  try {
    usb();
    hid();
    bluetooth();
    pcm();
    midi();
    edges();
    std::cout << "hardware protocol contracts passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
