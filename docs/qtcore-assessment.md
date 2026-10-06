# iiSDKCore 0.6.0 runtime assessment against Qt Core

This is the 0.6.0 evidence snapshot. The later 0.7.0 time-driven scheduling and live playout contract is documented in [realtime.md](realtime.md), with its own validation record. The later 0.8.0 [modern core families](modern-core.md) add implemented animation, parallel transforms, immutable shared snapshots, plugin loading, atomic file replacement and event filtering. The later 0.9.0 [data type serialization](serialization.md) supplies a bounded typed CBOR profile and registered DynamicValue envelopes. The test counts and missing-feature statements below belong to the 0.6.0 snapshot.

Assessment date: 2026-10-06. This evaluates the current source and recorded build/runtime evidence, not API names or planned capabilities. The conclusion is that iiSDKCore now provides an implemented, tested foundation for bounded scheduling, object communication, cleanup and native I/O. It remains an early platform foundation with incomplete device coverage and production validation. It cannot yet be treated as a complete replacement for Qt Core across the supported product fleet.

## Comparison boundary

The official [Qt Core overview](https://doc.qt.io/qt-6/qtcore-index.html) describes object communication/properties/ownership, threading, resources, I/O, containers, serialization, IPC and plugins. [QObject](https://doc.qt.io/qt-6/qobject.html) anchors its object model. Bluetooth and audio belong to the separate [Qt Bluetooth](https://doc.qt.io/qt-6/qtbluetooth-index.html) and [Qt Multimedia](https://doc.qt.io/qt-6/qtmultimedia-index.html) modules. iiSDKCore's requested hardware additions therefore extend the comparison beyond Qt Core's module boundary. Neither a larger API count nor folding more families into Core proves greater completeness.

| Area | Qt reference | iiSDKCore implementation and evidence | Assessment |
|---|---|---|---|
| Scheduling and event dispatch | Event system, threading | EventLoop post/timer/periodic scheduling, priority, bounded capacity, cancellation identities, pump budgets, native Channel readiness | Functional and tested. A callback/native call can exceed the cooperative budget; no hard realtime claim. |
| Object communication and properties | Signals/slots, meta-object and property systems | Typed QueryPort, ObjectProperties, selected-field responses, member trees and DynamicValue | Tested in-process contracts. Runtime type metadata is not a stable plugin/wire ABI or a QML engine. |
| Ownership and removal | QObject ownership and destruction | TaskScope, explicit close, SerialQueue/QueryPort teardown, Scene subtree retirement, Hardware endpoint quiescence | Strong explicit lifecycle contracts and reuse tests. Owners must respect callback/worker close restrictions. OS pairing state and allocator RSS are separate ownership domains. |
| Threads and parallel work | Threading classes | Owned Thread, Runtime workers and per-object SerialQueue | Cancellation, join, ordering and exception behavior tested. Shutdown is cooperative. |
| I/O and device families | Core I/O; additional device/media modules | Channel/AsyncIO plus native USB/HID, BLE, PCM and MIDI | macOS native execution is proven for inventory, audio queue setup and virtual MIDI. Linux/Windows are cross-build evidence; Android/WASM device bridges remain absent. |
| General application utilities | Resources, serialization, IPC and plugins | PathRoot, File, BinaryCursor, Deadline, diagnostics, dependency graph and service references | Useful bounded building blocks. No corresponding complete resource compiler, JSON/CBOR framework, plugin ABI, localization or IPC framework is delivered by this source tree. |
| Application and game hosting | Host/application event system | Standalone run or externally pumped poll; Scene, HostSession and Timeline foundations | Shared runtime use is viable. Windowing, rendering, UI, physics and asset pipelines require upper modules and are not established by these tests. |
| Packaging and platform proof | Framework module packaging | CMake package 0.6.0, 33 matching installed headers, installed consumer 6/6; native/sanitizer 24/24 and WASM 22/22 | Reproducible local proof exists. Remote CI execution, broad physical hardware and shipped product integration remain unverified. |

The Qt references establish the comparison categories, not comparative speed, memory use or reliability. No paired Qt benchmark or endurance run was performed. A numerical completeness percentage would mix unrelated scope and evidence levels and is not justified.

## Use decision

Use iiSDKCore now for new Qt-free runtime logic that fits its documented queue, query, threading, file/serial and supported device contracts. Keep platform service requirements explicit, including BlueZ/ALSA, Windows COM lifetime, Apple usage descriptions and the distinction between Windows MIDI 1.0 and optional MIDI 2.0.

Treat full cross-platform product adoption as requiring native execution on Linux/Windows/iOS, an Android device host bridge, physical USB/HID/BLE/audio/MIDI regression coverage and long-running teardown/load tests. Integrate an actual application and a game host before asserting fleet-wide replacement. Do not add unrelated Qt features solely to match a catalog; implement a missing facility when a real consumer requires it.

Detailed hardware limits and cleanup rules are in [hardware.md](hardware.md). Exact build/runtime results and unverified remainder are in [validation.md](validation.md).
