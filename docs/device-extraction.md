# Independent runtime and device modules

Core 0.12 owns EventLoop, FrameLoop, Runtime, QueryPort, TaskScope, Thread,
serialization, generic Channel and AsyncIO. iiDeviceMultimedia 0.2 independently
owns USB/HID, BLE central/GATT, float32 PCM, MIDI UMP and their native bridges.
Neither package includes, imports, configures, links or calls the other.
Applications explicitly select both when they need both responsibilities:

```cmake
find_package(iiSDKCore 0.12 CONFIG REQUIRED)
find_package(iiDeviceMultimedia 0.2 CONFIG REQUIRED)
target_link_libraries(app PRIVATE
    iiSDKCore::iiSDKCore iiDeviceMultimedia::iiDeviceMultimedia)
```

```cpp
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiDeviceMultimedia/IO/Hardware.h>

ii::runtime::EventLoop loop;
ii::device::Hardware devices([](ii::device::HardwareEvent) {});
const auto driver = loop.every(std::chrono::milliseconds(2), [&] {
    const auto result = devices.poll();
    // Inspect the independent device result.
});
// Pump on the host; later cancel and retire this driver before closing devices.
```

Device errors/codecs live in `ii::device` / `ii::device::protocol`; they never
redefine or alias `ii::io::Error` or Core scheduling types. The device module
provides explicit host-driven polling and no generic scheduler or Core adapter.
It works independently, and Core can continue after it has closed. The
application owns timer cadence and borrowed lifetime. Follow the
[complete composition/shutdown contract](../../iiDeviceMultimedia/docs/independence.md).

The earlier 0.1 extraction moved implementations out of Core but still depended
on Core's error, TaskScope, Thread and EventLoop APIs. Version 0.2 removes all
those dependencies. Existing device consumers replace `ii::io` device names with
`ii::device`, remove the loop constructor argument and own their polling driver.
The 0.1 embedded-Core option and automatic dispatch interval are removed. Device
SOVERSION 0.2 and SameMinorVersion package checks require rebuilding 0.1 consumers.
Core's generic C++ implementation is unchanged by this refactor.

Core still removes only its obsolete generated/installed Hardware.h and
Protocols.h files when upgrading from the former integrated device API. The
installation prefix and unrelated data are preserved. No forwarding header,
upper dependency, compatibility error alias or shared SDK base is introduced.
Core's existing device-boundary guard remains active. The device package has its
own independence guard and a standalone installed consumer with Core lookup
disabled. A separate application consumer finds both in both package/include
orders and verifies FrameLoop, byte I/O and native UMP delivery via a Core timer.

See [Core verification](validation.md),
[device verification](../../iiDeviceMultimedia/docs/validation.md) and the
[original extraction history](../../iiDeviceMultimedia/docs/extraction.md).
Cross-builds are distinguished from target/device runtime execution.
