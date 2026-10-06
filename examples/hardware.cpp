#include <iiSDKCore/IO/Hardware.h>
#include <iostream>
int main() {
  ii::runtime::EventLoop loop;
  ii::io::Hardware hardware(loop, [](ii::io::HardwareEvent) {});
  for (const auto kind :
       {ii::io::DeviceKind::usb, ii::io::DeviceKind::hid,
        ii::io::DeviceKind::audio_output, ii::io::DeviceKind::midi_output}) {
    auto devices = hardware.enumerate(kind);
    if (!devices) {
      std::cout << "kind=" << static_cast<int>(kind)
                << " unavailable=" << static_cast<int>(devices.error().code)
                << '\n';
      continue;
    }
    for (const auto &device : *devices)
      std::cout << "kind=" << static_cast<int>(kind) << " id=" << device.id
                << " name=" << device.name << '\n';
  }
  return hardware.close() ? 0 : 1;
}
