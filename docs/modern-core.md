# Modern core families (0.8.0)

The 0.17 native resource archive/mapping extension is documented in [resources.md](resources.md); the original 0.8 scope below remains historical.

This implementation extracts runtime requirements from the seven Qt reference families, rather than reproducing Qt's classes, inheritance, meta-object ABI or signals. All new public records are final. `iiSDKCore::Values` is an independently consumable header-only target for standard-only value/time/animation primitives. `find_package(iiSDKCore 0.8 CONFIG REQUIRED COMPONENTS Values)` omits worker/hardware dependency discovery; link only that target. The separately built `tests/value_modules.cpp` consumer neither links nor loads iiSDKCore's runtime library. Runtime integration uses `iiSDKCore::iiSDKCore`. The Values target propagates exception support for Emscripten, including when headers are imported from a package installed on another platform. Values has its own target export and imports no runtime target. Only the Values component bypasses the binary pointer-width check; full runtime imports retain it. When a cross toolchain restricts package searches to its sysroot, pass iiSDKCore_DIR explicitly for this value-only package. C++23 standard containers, algorithms, synchronization, futures and ownership types are used directly. This is an implemented common-runtime subset, not full compatibility with the Qt catalog.

## Coverage and module boundaries

| Reference family | iiSDKCore contract | Boundary |
| --- | --- | --- |
| Animation | `Animation/Timeline.h`: typed keyframes, caller-provided interpolation, easing, overlapping and sequential tracks, pauses. `Runtime/Animation.h`: timed playback, pause/resume, seek, finite/infinite iterations, alternate direction and completion. | No GUI property reflection, widget or rendering dependency. Parallel tracks share animation time; they do not imply simultaneous CPU execution. |
| Threading | Existing Thread, Runtime, SerialQueue, TaskGroup; `Core/Task/Parallel.h` adds owned-input asynchronous transforms, ordered results, futures, cancellation and exception reporting. | Persistent workers and bounded admission. Use standard mutex/shared_mutex, condition_variable, atomic, semaphore, latch, barrier and thread_local directly. No detached thread per task. |
| Containers | Standard vector/array/deque/list/map/unordered_map/set/unordered_set/stack/queue, span and ranges; PMR containers with standard memory resources when a fixed budget is needed. Existing Stream and LiveStream add bounded concurrent and timed queues. | No iiVector/iiMap clone. Iterator/reference invalidation follows the chosen standard container. A PMR resource must outlive its containers. |
| Plugins | `Plugin/Plugin.h`: versioned C-layout entry table, per-instance native calls, async Runtime submission, static registration, clean close. | Trusted in-process code, not a sandbox. Host and plugin must share the target C ABI, layout/alignment and calling convention. |
| Shared data | `Core/Memory/SharedValue.h`: cheap copy sharing immutable data, retained const snapshots, private edit copy, optimistic commit, revision and conflict detection. | 0.10 adds automatic separation through modify, with unique private noexcept reuse; see [implicit-sharing.md](implicit-sharing.md). No escaping mutable iterator access. T must own its mutable contents; copying T does not deep-clone shared mutable pointees. |
| I/O | Existing Channel/AsyncIO, loop readiness/transfers, File/PathRoot, BinaryCursor, reusable buffers and hardware protocol modules; the separate iiFileProvider CoreIO adapter owns atomic file replacement and temporary retirement. Standard filesystem supplies directory traversal, metadata and space; standard streams/from_chars supply text primitives. | Filesystem watcher, process/IPC supervisor, URL parser, settings schema and resource archive are not implemented by this change. No network/TLS/HTTP claim. |
| Events | Existing EventLoop timers/IO plus typed query delivery; `Core/Event/Receiver.h` adds filtering and an accepted/rejected reply in the receiver's SerialQueue. `Core/Event/Event.h` now depends only on Core/Time. | Notifications remain queries. No global bus, QObject hierarchy or GUI event catalog. |

References checked on 2026-10-06: [Animation](https://doc.qt.io/qt-6/animation.html), [Threading](https://doc.qt.io/qt-6/thread.html), [Containers](https://doc.qt.io/qt-6/containers.html), [Plugins](https://doc.qt.io/qt-6/plugins.html), [Shared classes](https://doc.qt.io/qt-6/shared.html), [I/O](https://doc.qt.io/qt-6/io.html), [Events](https://doc.qt.io/qt-6/events.html). These are source references for requirements, not copied implementations.

## Animation

A Timeline is a prepared value. Add all tracks before playback. `append` starts after the current total duration, `parallel` takes an explicit start, and `pause` extends the duration. Zero-duration tracks sample their endpoint. Sampling executes callbacks in insertion order; shared target state therefore has deterministic ordering. Callbacks choose a typed setter or notification query explicitly. Keyframe times must be nonnegative and strictly increasing. The left key chooses easing for the next segment; outside the key range sampling clamps to endpoints. Floating values default to std::lerp; other types require an interpolator.

Playback owns one-shot scheduled work registered with TaskScope::at. It samples actual monotonic elapsed time and schedules the next phase-aligned frame; it skips missed frames rather than bursting. Frame callbacks are serialized with controls. A generation suppresses obsolete callbacks across pause/resume. Status observation is allowed inside a callback; recursive play/pause/seek reports busy, and close reports self_join. Close blocks admission, cancels pending frames, waits for active callbacks and destroys all track/completion captures before returning loop capacity. A loop must outlive playback. Callback failures set failed and retain the exception. Replaying completed/failed animation begins at zero. Seek uses total elapsed playback time, not normalized single-cycle time. Infinite zero-duration playback completes once.

```cpp
animation::Timeline timeline;
animation::Keyframes<double> gain({{0ms, 0.0}, {200ms, 1.0}});
timeline.append(200ms, [gain, &target](double p) {
    target = gain.sample(std::chrono::duration_cast<core::Clock::duration>(200ms * p));
});
runtime::Animation playback(loop, std::move(timeline));
auto started = playback.play();
// Drive loop with poll/run; close playback before removing target or loop.
```

## Parallel work and containers

`parallel_transform(runtime, owned_vector, mapper, grain)` returns a standard shared_future of Result<vector<Out>>. Input ownership is retained by submitted tasks; output order equals input order. The mapper receives const input references, and each chunk owns a mapper copy. Shared/reference captures must be synchronized by the caller. The number of chunk tokens is capped by Runtime capacity by increasing effective grain. Capacity shared with unrelated work may still reject a batch after partial submission; its future settles with an error, queued chunks are discarded, and running chunks see cooperative cancellation. Cancel discards queued chunks and requests running chunks to stop between elements. An arbitrary mapper cannot be preempted.

A discarded callable settles its ticket; cancellation and shutdown never leave an admitted batch's future unresolved. Submission failure, mapper exceptions and result construction exceptions are retained in Result. Reading a future's ready result does not promise that its worker slot has already retired; wait_idle/join is the executor removal boundary. Do not block a worker waiting on dependent work submitted to the same exhausted pool. Destroying a ParallelResult does not cancel work; submitted work owns its data independently, matching the explicit ownership contract of standard futures.

Use standard containers without opaque compatibility adapters:

| Qt container role | Standard choice |
| --- | --- |
| Contiguous list/array | std::vector / std::array / std::span |
| Ordered keys | std::map / std::set |
| Hashed keys | std::unordered_map / std::unordered_set |
| Stack/FIFO/priority | std::stack / std::queue / std::priority_queue |
| Iteration/projection | std::ranges / std::views |
| Prepared bounded memory | std::pmr containers + monotonic_buffer_resource + null_memory_resource |

The PMR regression destroys containers before releasing the arena, repeats reuse 1,000 times, and verifies budget exhaustion raises bad_alloc instead of silently allocating from the heap. An arena is not inherently concurrent. Give independent jobs independent arenas or synchronize resource use. C++23 flat containers are optional only when the target standard library advertises support; public API does not require them. C++26 senders/receivers are not presented as C++23 standard features.

## Shared values

Copying a SharedValue creates an independent commit boundary sharing the current immutable allocation. A snapshot owns const T and remains valid after the source changes or disappears. Updating copies T, runs the edit outside the mutex, then commits only if identity and revision still match. Conflict returns busy; no automatic retry repeats side effects. A throwing edit returns failed without committing. Old and rejected allocations retire outside the instance mutex. Assigning another SharedValue advances the destination revision, invalidating edits based on the previous state. Revision exhaustion is explicit. Destruction releases owned allocations; an external snapshot retains its explicit shared ownership.

## Plugins

A plugin exports `extern "C" const ii::plugin::Descriptor* ii_sdk_plugin_v1() noexcept`. The function table uses version/size, an identifier with explicit length, create/destroy and a byte-span invoke function. Define the ABI calling convention consistently (default target C convention, cdecl on 32-bit Windows). Create must return a nonnull owned context. Only the plugin destroys that context. No standard container/string/exception/allocator ownership crosses the table. The table uses fixed-width lengths; a response length beyond capacity is rejected. Input/output requests are bounded to 16 MiB each. Nonzero plugin status becomes a failed Result with the numeric status.

Native load requires an absolute path without embedded NUL. POSIX uses [dlopen](https://pubs.opengroup.org/onlinepubs/9799919799/functions/dlopen.html) with RTLD_NOW/RTLD_LOCAL; Windows uses [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw) with DLL directory and default safe search directories. OS APIs remain private. iOS and WASM use attach(entry) and return unsupported for load. Android supports native loading subject to its linker namespace and application packaging. ABI validation cannot make arbitrary native code safe. A plugin must stop and join its own background activity in destroy and must not retain borrowed buffers.

Calls on one instance are serialized. `post` runs on existing Runtime workers, and completion participates in the same lifetime barrier. Close rejects new calls, cancels queued calls, waits for active calls/completions, invokes destroy and then releases the module. Recursive invocation returns busy. Closing/destroying from an own callback is invalid and returns self_join for close; foreign Runtime workers may receive would_block when removal would wait. Perform removal from a host/owner domain. Native fixtures verify ABI rejection and the actual destroy-then-unload marker order. An OS may retain a module for independent references; the loader releases only its own reference.

## Atomic file and events

`iiFileProvider::core::AtomicFile` (moved outside Core in 0.13) exclusively creates a temporary in the target directory. Writes loop across short transfers. Commit flushes and closes, then replaces the target name atomically (MoveFileExW on Windows, filesystem rename on POSIX/WASM). There is no direct-write fallback. Cancel/destruction closes the handle and removes the temporary. A rename failure attempts temporary cleanup and preserves the original target. Cancel reports cleanup failure so the owner can retry; destructor is best effort when filesystem errors prohibit removal. It does not guarantee directory metadata survival after power loss, preserve the old file's permissions, or provide a symlink/security sandbox. Commit can block on filesystem work: isolate it with Runtime/SerialQueue when used in latency-sensitive applications. Persistent files are user data, not resources to erase when an object is removed.

EventReceiver<T> composes QueryPort<Event<T>,bool>. Filters run before the handler, in the same serial domain; false produces an explicit rejected reply. A thrown filter/handler produces the existing handler_failed reply. Sequence and monotonic timestamp are payload metadata, not a global ordering across producers or queues. Cancellation suppresses handler/completion. Close destroys filter/handler captures and queued event payloads through the existing query removal barrier. Timer/device readiness stays in EventLoop; GUI focus/paint/window events belong to higher modules.

## Evidence

`tests/modern.cpp` and `tests/plugins.cpp` are consumer contracts; `tests/plugin_fixture.cpp` is a separately compiled native plugin. The mandatory architecture gate includes MODULE libraries as well as the runtime and tests. Public headers compile independently. Current build/runtime evidence and platform limits are recorded in [validation](validation.md). Cross-compilation is not platform execution, hardware operation, hard real-time or third-party plugin compatibility proof.

## Implicit sharing extension (0.10.0)

`SharedValue::modify` provides serialized implicit separation and private noexcept reuse while `update` retains optimistic transaction semantics. Snapshot publication also protects weak aliases. DynamicValue, typed queries and the optional serialization codec compose with the sharing value; the current complete contract is [implicit-sharing.md](implicit-sharing.md).
