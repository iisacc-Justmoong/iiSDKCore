# iiSDKCore

Package release: **0.18.1** (2026-10-19). This patch packages the current
source snapshot; build, test and installation evidence is recorded in the SDK
patch-release report. Earlier versioned feature notes remain historical.


iiSDKCore is the C++23 foundation runtime for iiSDK applications and games. Version 0.18.0 adds typed live query streams with per-subscriber selection, bounded delivery and explicit removal. Native resource packs, aligned immutable views, content deduplication and read-only OS memory mapping from 0.17 remain available. The external-host integration from 0.16 and native process/pipe/environment support from 0.15 remain available. USB/HID, BLE, PCM and live MIDI implementations belong to the independent [iiDeviceMultimedia](../iiDeviceMultimedia/README.md) module. Core does not link its device/media frameworks or Linux libudev/D-Bus/ALSA dependencies. See [native resources](docs/resources.md), [host integration](docs/host-loop.md), [app/game frames](docs/frame-loop.md) and the [extraction boundary](docs/device-extraction.md).

The foundation has no feature-SDK dependency. `iiSDKCore::Contracts` supplies
standard-only UTF-8/bytes/structured-value and execution contracts;
`iiSDKCore::Runtime` provides the existing native runtime. File creation, atomic
publication and file presentation live in the optional
[iiFileProvider CoreIO adapter](../iiFileProvider/CoreIO/README.md), above both
independent libraries. See the [foundation architecture](docs/foundation-architecture.md)
for package selection, ownership and the 0.12 migration.

<a id="제공-기능"></a>

## Provided Features

- [Selective live query streams](docs/query-stream.md): typed filter/projection queries, independent subscribers, query revisions, bounded output rings, executor backpressure/retry and removal barriers. Existing Runtime workers, EventLoop/HostLoop and QueryPort supply execution and control.
- [Native resources](docs/resources.md): deterministic indexed packs, 64-byte-aligned shared payloads, immutable embedded/owned/native-mapped views, explicit integrity/paging hints and native lifetime after channel/pack removal. iiSDKBuilder supplies the build-host compiler and target selection; iiFileProvider owns outputs. Core imports neither module.

- [External hosts](docs/host-loop.md): coalesced wake notifications from all EventLoop mutation paths, owner-thread dispatch, native readiness waiting with completion on the host thread, disconnect barriers, weak producer endpoints and host-context diagnostics. iiGUI consumes this foundation; Core has no iiGUI/window/GPU/font dependency.

- [Native processes](docs/process.md): literal argument vectors, child-only environment/cwd, binary stdin/stdout/stderr Channels, bounded capture and streaming sinks, exit/signal/deadline/cancellation results and owned-tree cleanup. iiSDKBuilder uses this common API while FileProvider owns logs.
- [Text boundaries](docs/text.md): strict or explicitly replacing UTF-8/UTF-16 conversions, native path conversion, preserved NUL in text/bytes and explicit byte/code-unit indexing. Owned payloads cross asynchronous boundaries.
- [Tracked operations](docs/operation.md): immediate admission errors, immutable results, cooperative cancellation/deadlines, owner/executor removal, side-effect state and one receiver-scoped completion on an explicit reply executor. Serial execution and fixed OS-thread execution remain distinct.
- [Qt migration P0](docs/qt-migration-p0.md): the independent [iiJson](../iiJson/README.md) codec consumes Contracts, and [iiCSMIDI](../iiCSMIDI/README.md) uses Native storage and optional Runtime decoding. Core imports neither feature module.

- `FrameLoop`: ten ordered app/game stages, bounded fixed-step catch-up and interpolation, scaled/unscaled clocks, pause, per-tick intervals/enablement, acyclic prerequisites and frame diagnostics. Native `run()`, host-driven `poll()/step()` and `BrowserLoop(frames)` use the same C++ implementation. See the [contract](docs/frame-loop.md) and [app/game example](examples/frame_loop.cpp). Unity and Unreal documentation inform requirements without adding engine dependencies.

- [Implicit sharing](docs/implicit-sharing.md): normal value copies share storage; `SharedValue::modify` automatically separates writes and reuses unique private data for noexcept edits. Published snapshots and weak aliases remain immutable. An optional standard-only serialization bridge persists logical data and restores a fresh sharing owner. Shared values also compose with DynamicValue and typed runtime queries.

- [Data type serialization](docs/serialization.md): deterministic CBOR with checked integers/floats, UTF-8 text, byte strings, standard containers and explicit record codecs; bounded complete-message decoding; versioned DynamicValue type envelopes. Static serialization is independent through `iiSDKCore::Values`, and existing Runtime queries/workers compose with owned serialized bytes; the optional storage adapter persists them.

- [Modern core families](docs/modern-core.md): typed keyframe/easing timelines and timed playback; cancellable parallel transforms with ordered standard futures; immutable shared snapshots with transactional copy-on-write; versioned native/static plugins; serial event filtering. Atomic file replacement is provided by the separate storage adapter. Standard containers, ranges, PMR memory resources and synchronization are used directly. The `iiSDKCore::Values` target supports independent use of standard-only value, time and animation primitives without linking the runtime.

- Device/media I/O is consumed separately through `iiDeviceMultimedia::iiDeviceMultimedia`. Generic Core APIs do not depend on that package. The device package is also independent of Core. Applications own their connection using device poll(); see [composition and migration](docs/device-extraction.md).
- Multiple inheritance is forbidden in project records. Public SDK classes and value records are `final`, and a mandatory build check also rejects multiple bases and expanding base packs in source and preprocessed declarations. Extend objects through composition. The host build requires Python 3.9 or later; installed runtime consumers do not. See the [inheritance policy](docs/inheritance.md).
- `EventLoop::watch/read/write` detects channel readiness and delivers one-shot input/output on the dispatch thread. POSIX uses a shared native poll set and wake pipe; Windows retains COM status probes with a wake event. Existing files, serial ports, and accessible POSIX character devices/FIFOs share bounded task slots and cancellation cleanup. See [event loop I/O](docs/loop-io.md).
- Clean object removal through `TaskScope`, `QueryPort::close()`, and `SerialQueue::close()`: stop admission, cancel queued work, await active handlers and completions, destroy owned captures, then return capacity for the next object. `Scene::erase()` applies this order to a subtree. See the [cleanup contract](docs/cleanup.md) and `tests/cleanup.cpp`.
- Provides path, file, binary cursor, job bundle, service reference, diagnostics, dependency graph, Scene, script binding, host pump, and timeline object based on 18 folders. [based object contract](docs/foundations.md), [sensor value asynchronous processing example](examples/foundations.cpp)define responsibility and lifecycle rules.
- `ii::io::Channel`, `AsyncIO`, `Buffer`: Native file, serial byte I/O, prepared wait, timeout, cancel, reusable buffer, and asynchronous query. OS hides handles and specifies the support difference between WASM virtual files and real devices. Defined in [I/O contract](docs/io.md).
- `DynamicValue`, `value_cast<T>()`, `value_ref<T>()`: Dynamic C++ values and explicit type conversion and reference access. Supports number range and precision check, string conversion, dynamic type policy of properties, and casting at query time. Defined in [dynamic type contract](docs/dynamic-value.md).
- The default execution model is `Runtime`'s multi-threaded, parallel, and asynchronous processing. Start workers at creation and reuse work slots. `SerialQueue` processes queries of the same object in order and executes different objects in parallel. Defined in [default runtime contract](docs/parallel-runtime.md)and [object asynchronous example](examples/parallel.cpp).
- `Thread`: Platform independent thread ownership, start, cooperative cancellation, wait for termination, status, and exception delivery. EventLoop connects execution and cancellation and reuses the instance after join. Defined in [thread contract](docs/thread.md), [query execution example](examples/thread.cpp).
- `ObjectProperties` : Defines, replaces, and deletes per-instance values, methods, and child trees at creation and after creation. It supports queryable/designable policies, private path protection, selective queries, type and value validation, and change queries. [Dynamic property contracts](docs/object-properties.md)and [execution examples](examples/properties.cpp)are defined.
- Object member tree: Constructs values, general methods, and query ports for each node of nested instances, preserving per-level public/private access control. [Tree contracts and implementation examples](docs/member-tree.md)are defined.
- `QueryPort<Request, Response>` : Queries between objects that pass only required inputs and selected response fields. Passing the entire object is an explicit option, and it supports synchronous/queued delivery, value/void responses, and closed receiver/handling failures. Removal cancels queued completions and waits for active delivery.
- `EventLoop` : Multi-producer/single-executor work queues, immediate/scheduled/periodic tasks, cancel tokens, 4:2:1 weighted priority, callback count/time budget, suspend/resume/terminate, and statistics.
- `Stream<T>` : Preallocated circular buffer, FIFO , rejection or discarding of old data, consumption of remaining data after termination, and backpressure/discard statistics.
- `BrowserLoop` : An instance that drives the event loop as `requestAnimationFrame` in Emscripten.
- `run()` : A native independent loop. It waits on a condition variable when there is no work and wakes up on new work, timers, or termination.
- `poll()` : Non-blocking execution units that insert into UIKit where the host owns the loop, Android , desktop GUI , and browsers.

Public objects are composed of instances and callbacks without inheritance. Dependent objects receive references, and data is passed as values or explicit references. Pointers are required for RAII ownership within the implementation and at the browser C ABI boundary, but they are not exposed as a consumer ownership API.

<a id="최소-사용-예"></a>

## Minimal usage example

```cpp
#include <iiSDKCore/Runtime/Query.h>

int main() {
    using namespace ii::runtime;
    Runtime runtime;             // The default 2~4 workers start automatically.
    SerialQueue object(runtime);
    QueryPort<int, int> calculate(object, [](const int& value) { return value * 2; });
    int answer = 0;
    auto accepted = calculate.post(21, [&](QueryReply<int> result) {
        if (result) answer = *result;
    });
    if (!accepted) return 1;
    if (!runtime.wait_idle() || answer != 42) return 2;
    return runtime.join() ? 0 : 3;
}
```

Executable [live example](examples/live.cpp)processes 120 samples in the same loop with a periodic producer and consumer. The same file is built as a native executable or WASM HTML.

`./build/iiSDKCorePropertiesExample` adds a gain property and a scale method to a single Device instance after creation and queries only the required values. Changes do not propagate to other instances created with the same schema.

<a id="빌드"></a>

## Build

CMake 3.31 or later, a C++23 compiler and standard platform threading are required. Core no longer requires Linux device development packages, Apple media frameworks or Windows C++/WinRT device projections. Those requirements belong to iiDeviceMultimedia. Generated files stay under `build/`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/iiSDKCoreLive
cmake --install build --prefix "$PWD/build/stage"
```

Consumers are connected with `find_package(iiSDKCore 0.13 CONFIG REQUIRED)` and `iiSDKCore::iiSDKCore`. C++23 and thread dependencies are propagated from the target. `BUILD_SHARED_LIBS=OFF` can be selected for static libraries. iOS and Emscripten generate static libraries. Existing `hello()` is kept. Headers must be included via `<iiSDKCore/...>` path to avoid conflicts with other SDKs.

Package search, header usage, linking, and actual shared library loading are verified by public function call tests and the following separate installation consumers.

```sh
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_PREFIX_PATH="$PWD/build/stage"
cmake --build build/consumer/build
ctest --test-dir build/consumer/build --output-on-failure
```

In the Windows multi-config generator, add `--config Release` for build/install and `-C Release` for CTest, and put `bin` of the installation path into `PATH` when running shared library consumers.

<a id="플랫폼과-범위"></a>

## Platform and scope

|Target|Execution mode|
|---|---|
| macOS / Windows / Linux |A native C++ executable or library. A standalone `run()` or the host's `poll()`|
| iOS |Native static library. UIKit host calls `poll()`, independent worker threads are `run()` possible|
| Android |NDK library. Activity/ NativeActivity host calls `poll()` or `run()` from worker thread|
| WASM |Default pthread ON. Application worker uses Runtime/Thread and browser main thread uses BrowserLoop / `poll()`|

Linux Core has no X11/Wayland, libudev, D-Bus or ALSA package dependency. Native device/media backends and their platform requirements are in [iiDeviceMultimedia](../iiDeviceMultimedia/docs/hardware.md). Actual platform execution and build evidence are recorded separately in [validation](docs/validation.md).

This runtime operates in native **user space**, and WASM follows its host execution environment. The time budget is cooperative and does not guarantee hard real-time execution. Files, serial ports, and accessible POSIX character devices/FIFOs can use the same EventLoop for readiness detection, input, and output. POSIX `run()` waits on multiple channels and a wake pipe; host-driven `poll()` probes readiness without a sleeping kernel wait. The independent iiDeviceMultimedia 0.2 package supplies device discovery, media and device protocols through its own ii::device API and explicit poll(). Neither module depends on the other; an application can call that poll from its Core EventLoop timer. GUI entry points, windows, rendering, app packaging, audio synthesis/effects, and manufacturer-specific command meanings remain host or upper-module responsibilities. Slow filesystem or driver calls can still delay dispatch; AsyncIO retains worker isolation for those calls. See [event loop I/O](docs/loop-io.md) for platform and lifetime limits.

Detailed semantics and cross-build are defined in [runtime design](docs/runtime.md), project principles are defined in [AGENT .md](AGENT.md). IDE  settings, build artifacts, and empty design directories are not included in the repository.

Communication between objects is configured according to [query contract](docs/query.md). [object communication example](examples/query.cpp)has the Controller passing `SetLevel{42}` to `device.audio.output.set_level` and requesting only one value with `ReadDevice{.audio = {.output = {.level = true}}}` . The response also maintains the `audio.output.level`  structure, omitting name, object reference, and unselected sibling fields. It is executed with `./build/iiSDKCoreQueryExample` .

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
