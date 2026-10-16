# Foundation boundary (0.13)

For the 0.14 text/result contracts and the completed native MIDI consumer, see [Qt migration P0](qt-migration-p0.md). The dependency/component separation below remains in force.

iiSDKCore provides common C++23 value, lifetime, execution and error contracts for
applications and games. It is the bottom of the SDK graph. Qt compatibility,
file provisioning, previews, HTTP/TLS, SQL engines, image codecs and document
parsers belong above it. Platform runtime APIs remain internal where standard
C++ cannot implement native waiting or handle ownership.

## Packages and responsibility

| Consumer | CMake component / target | Dependencies |
| --- | --- | --- |
| Shared data and execution contracts | `Contracts` / `iiSDKCore::Contracts` | C++23 headers only |
| Existing value/CBOR/sharing utilities | `Values` / `iiSDKCore::Values` | Contracts; no runtime or OS services |
| Apps/games using the existing runtime | `Runtime` / `iiSDKCore::Runtime` | Contracts, platform threads and native runtime |
| Existing default consumer | no component / `iiSDKCore::iiSDKCore` | Same runtime; also makes Values available |
| Native file creation, atomic saves and presentation | separate `iiFileProviderCoreIO` / `iiFileProvider::CoreIO` | Core runtime + iiFileProvider Native |

Header-only component combinations do not import Runtime, Threads or a feature
SDK, and their package version is independent of binary pointer size. A runtime
consumer still needs the correct target ABI. Unknown required components fail
package discovery.

The optional CoreIO adapter sits above two independent libraries. Neither Core
nor the base Native file provider discovers or links the adapter or each other.
The parent file-provider build enables it only through
`IIFILEPROVIDER_BUILD_CORE_IO=ON`; it can also be built as its own CMake project.

## Common data

Use standard containers, smart pointers, `std::string`, `std::span`, `std::chrono`
and `std::expected` directly. SDK types add semantics where needed:

* `Core/Bytes/Bytes.h`: owned `Bytes` and borrowed read-only `ByteView`; the owner
  must outlive a borrowed view. `byte_view` does not transcode text.
* `Core/Text/Utf8.h`: Unicode-scalar UTF-8 validation shared by structured values
  and the existing CBOR codec. It rejects overlong, surrogate, truncated and
  out-of-range sequences. Embedded NUL is valid text; protocols/paths impose
  their own restrictions. Normalization and extended encodings stay outside.
* `Core/Value/Value.h`: owned null/bool/signed integer/unsigned integer/finite
  real/UTF-8 text/bytes/array/object values. Construction and exact `get<T>` are
  explicit. Signed, unsigned, real and boolean types stay distinct; missing and
  explicit null stay distinct. Failed lookup returns a typed result. Copies own
  independent logical data; `SharedValue<Value>` is available when sharing is
  appropriate. Pointer truthiness and narrowing long-double construction are
  rejected. Borrowing from temporaries is rejected at compile time. Only const
  references escape; reassignment/destruction retires
  views. The model performs no JSON parsing or serialization. Parsers must enforce
  input/depth/size budgets before constructing untrusted trees.
* `Core/Result/Result.h`, Time/Deadline and PathRoot retain their established
  error, monotonic-time and lexical-path contracts. PathRoot is not a filesystem
  security sandbox.

`runtime::DynamicValue` remains process-local `std::any` for typed object values
and custom records. Its explicit casts and registered CBOR codecs preserve that
existing purpose. Structured `core::Value` does not replace arbitrary C++ query
payloads or introduce a global type/serialization registry.

## Execution and lifetime

`Runtime/Execution.h` defines Task, Priority, Submission, TaskToken, PumpBudget
and the structural `TaskExecutor` concept without EventLoop, I/O or worker
implementations. EventLoop, Runtime and SerialQueue satisfy it. QueryPort and
TaskScope use that same contract rather than repeating private constraints.

Admission is distinct from delivery and task results. Submission reports queue
admission; typed QueryReply and standard futures carry results. TaskToken is an
opaque, lifetime-safe cancellation handle for queued work. Already running work
remains cooperative. TaskScope/QueryPort/SerialQueue retain admission shutdown,
cancellation, active-call waiting and capture retirement barriers. The executor
must outlive calls through its reference.

Object communication remains instance-owned typed queries, including notification
queries and EventReceiver filters. Closing the port/receiver disconnects delivery.
There is no QObject base, inheritance-based executor, global event bus or parallel
signal/slot system. Existing frame stages, fixed-step timing and browser/native
drivers remain in the opt-in runtime implementation.

## Native streams and storage

Channel owns byte transfer, readiness, cursor, flush, shared lifetime and close.
Core no longer opens/creates regular files or publishes an atomic transaction.
`NativeFile.h` is the explicit platform-adapter extension that consumes an owned
POSIX descriptor or Windows handle represented by intptr_t. It validates a regular
file, closes rejected resources, preserves ownership on allocation failure, and
never returns a handle through common Channel APIs. A noexcept close notification
runs exactly once after successful native close, including last-owner destruction.

`iiFileProvider::core::open_file` opens/creates through Native provider APIs and
supplies its close notification. That layer owns exclusive creation, truncation
and preview/details scheduling. Its AtomicFile creates/flushes a same-directory
temporary, publishes through the provider's native atomic replacement, and cleans
temporary presentation data. Cancellation/failure has no direct-write fallback.
Core's rooted File object takes an opener callback and owns its resulting Channel.
Serial/character-device byte channels remain available under their existing
documented platform contracts; USB/BLE/audio/MIDI stay in iiDeviceMultimedia.

## Migration and enforcement

```cmake
find_package(iiSDKCore 0.13 CONFIG REQUIRED COMPONENTS Contracts)
target_link_libraries(MyDataModule PRIVATE iiSDKCore::Contracts)
# A host that executes work instead selects COMPONENTS Runtime and ::Runtime.
```

For Core 0.12 regular-file consumers, include `<iiFileProviderCoreIO.h>`, link
`iiFileProvider::CoreIO`, and use its `open_file` / `AtomicFile` namespace.
Pass the opener to `core::File::open`. Old generated/installed Core AtomicFile
headers are removed on upgrade. Existing SDKBuilder manifests/reports use their
already-owned Native provider's `File::write` directly.

The mandatory `iiSDKCoreFoundationBoundary` build target rejects upper SDK/Qt
includes, package dependencies and feature links even with tests disabled. CTest
also runs that guard. Installed consumers disable Qt and the file provider;
Contracts consumers additionally disable Threads. Native-handle failure/lifetime,
values, existing runtime regressions and relocated storage assertions exercise
the implementation boundary.

This establishes the foundation for non-GUI module migration. Existing Qt-based
modules and GUI facades still need consumer-by-consumer migration; this change
does not claim their public APIs or application binaries have all shed Qt.
`iiStorage` and `iiGraphics` in architectural diagrams describe roles, not newly
created empty SDKs. Concrete extraction follows observed consumers and tested
replacement boundaries rather than reproducing every QtCore facility.

## Resource-byte foundation (0.17)

Core supplies the in-memory resource pack, immutable shared views and native read-only Channel mappings. The build-host resource compiler belongs to Builder and file publication belongs to FileProvider. Neither is linked/imported by Core. Prepared codec/GPU payloads remain opaque bytes; native format conversion stays in its feature module. Contracts/Values-only imports still omit Runtime and its resource implementation. See [resources.md](resources.md).
