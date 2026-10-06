# iiSDKCore

iiSDKCore is the C++23 foundation runtime for iiSDK applications and games. It combines bounded scheduling, object communication and native I/O without Qt. Platform services use operating system APIs; Linux hardware I/O uses libudev, D-Bus and ALSA. Package version is 0.10.0. The [Qt Core comparison and adoption assessment](docs/qtcore-assessment.md) records the 0.6.0 baseline and remaining evidence gaps. [Time-driven runtime](docs/realtime.md) adds cooperative quanta, per-period budgets, EDF dispatch and timestamped live playout.

<a id="제공-기능"></a>

## Provided Features

- [Implicit sharing](docs/implicit-sharing.md): normal value copies share storage; `SharedValue::modify` automatically separates writes and reuses unique private data for noexcept edits. Published snapshots and weak aliases remain immutable. An optional standard-only serialization bridge persists logical data and restores a fresh sharing owner. Shared values also compose with DynamicValue and typed runtime queries.

- [Data type serialization](docs/serialization.md): deterministic CBOR with checked integers/floats, UTF-8 text, byte strings, standard containers and explicit record codecs; bounded complete-message decoding; versioned DynamicValue type envelopes. Static serialization is independent through `iiSDKCore::Values`, and existing Runtime queries/workers and AtomicFile compose with owned serialized bytes.

- [Modern core families](docs/modern-core.md): typed keyframe/easing timelines and timed playback; cancellable parallel transforms with ordered standard futures; immutable shared snapshots with transactional copy-on-write; versioned native/static plugins; atomic file replacement; serial event filtering. Standard containers, ranges, PMR memory resources and synchronization are used directly. The `iiSDKCore::Values` target supports independent use of standard-only value, time and animation primitives without linking the runtime.

- `ii::io::Hardware` adds native USB/HID enumeration and hotplug, USB control transfers, HID reports, BLE GATT, float32 PCM audio, and MIDI UMP. Bounded events reach the existing EventLoop; endpoint close discards stale input and waits for active delivery before returning capacity. `IO/Protocols.h` provides USB/HID, BLE, PCM and MIDI parsers. See [native hardware I/O](docs/hardware.md) for platform requirements and limits.
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

CMake 3.31 or later and a C++23 compiler are required. On Linux, install the system development packages `libudev-dev`, `libdbus-1-dev`, and `libasound2-dev`; on Windows use the Windows SDK with C++/WinRT projections. Native Windows MIDI 2.0 UMP requires the new Windows MIDI Services projections and service; a stock Windows SDK builds the explicit WinRT MIDI 1.0 transport. Check both `midi` and `midi2` capabilities. Generated files are placed in `build/`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/iiSDKCoreLive
cmake --install build --prefix "$PWD/build/stage"
```

Consumers are connected with `find_package(iiSDKCore 0.10 CONFIG REQUIRED)` and `iiSDKCore::iiSDKCore`. C++23 and thread dependencies are propagated from the target. `BUILD_SHARED_LIBS=OFF` can be selected for static libraries. iOS and Emscripten generate static libraries. Existing `hello()` is kept. Headers must be included via `<iiSDKCore/...>` path to avoid conflicts with other SDKs.

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

Linux core has no X11/Wayland or CPU instruction dependency. Native hardware I/O requires libudev, D-Bus and ALSA 1.2.10 or later (development packages are needed to build). However, verifying all Linux distributions/architectures does not mean actual verification results. Actual verification results are recorded separately in [verification record](docs/validation.md).

This runtime operates in native **user space**, and WASM follows its host execution environment. The time budget is cooperative and does not guarantee hard real-time execution. Files, serial ports, and accessible POSIX character devices/FIFOs can use the same EventLoop for readiness detection, input, and output. POSIX `run()` waits on multiple channels and a wake pipe; host-driven `poll()` probes readiness without a sleeping kernel wait. Hardware provides native device discovery, hotplug, standard control operations and protocol decoding on the backends listed in [hardware I/O](docs/hardware.md). GUI entry points, windows, rendering, app packaging, audio synthesis/effects, and manufacturer-specific command meanings remain host or upper-module responsibilities. Slow filesystem or driver calls can still delay dispatch; AsyncIO retains worker isolation for those calls. See [event loop I/O](docs/loop-io.md) for platform and lifetime limits.

Detailed semantics and cross-build are defined in [runtime design](docs/runtime.md), project principles are defined in [AGENT .md](AGENT.md). IDE  settings, build artifacts, and empty design directories are not included in the repository.

Communication between objects is configured according to [query contract](docs/query.md). [object communication example](examples/query.cpp)has the Controller passing `SetLevel{42}` to `device.audio.output.set_level` and requesting only one value with `ReadDevice{.audio = {.output = {.level = true}}}` . The response also maintains the `audio.output.level`  structure, omitting name, object reference, and unselected sibling fields. It is executed with `./build/iiSDKCoreQueryExample` .

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
