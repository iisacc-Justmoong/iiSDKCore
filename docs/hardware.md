# Device/media I/O moved to iiDeviceMultimedia

Since iiSDKCore 0.12.0, USB/HID, BLE central/GATT, float32 PCM, MIDI UMP,
their protocol parsers and native platform backends are owned by the independent
[iiDeviceMultimedia](../../iiDeviceMultimedia/README.md) package.

The complete [hardware contract](../../iiDeviceMultimedia/docs/hardware.md) and
[migration guide](device-extraction.md) describe current APIs and support limits.
Core retains EventLoop, FrameLoop, Runtime, TaskScope, Thread, Channel and AsyncIO
and has no dependency on the new module. Device 0.2 is likewise independent of
Core; the application owns the connection through ii::device::Hardware::poll().
