# Implicit sharing (0.10.0)

`ii::core::SharedValue<T>` gives copyable data value semantics with implicit shared storage. Normal C++ copy construction and assignment share the payload without copying T. Modification detaches automatically; no detach call, custom container family or inheritance is required. Strings, standard containers and application records can be the owned T. Link the header-only `iiSDKCore::Values` target; Qt and the runtime are unnecessary.

This follows the copy-on-write requirement described by [Qt's implicit sharing documentation](https://doc.qt.io/qt-6/implicit-sharing.html), using C++23 standard ownership and synchronization. Qt classes and implementations are not dependencies.

## Operations

| Operation | Contract |
| --- | --- |
| Copy constructor | Share the current block, copy revision, create an independent mutation boundary; zero T copies |
| Copy assignment | Share the source block, retire the destination's old block, advance destination revision; zero T copies |
| `snapshot()` | Retain immutable const T and its revision; ownership lasts beyond source changes/destruction |
| `modify(edit)` | Short serialized callback with automatic separation; shared data is copied once before editing |
| `update(edit)` | Existing optimistic transaction: private copy, callback outside lock, explicit busy conflict if the source changed |
| Destruction | Release this owner's block; T is destroyed at the last strong owner |

```cpp
#include <iiSDKCore/Core/Memory/SharedValue.h>
#include <vector>

ii::core::SharedValue<std::vector<int>> first(std::vector{1, 2, 3});
auto second = first;                         // payload shared, no vector copy
auto edited = second.modify([](auto& data) noexcept {
    data[0] = 9;                             // automatically detach from first
});
if (!edited) { /* handle edited.error() */ }
// first remains {1,2,3}; second is {9,2,3}
```

`modify` callbacks return void and borrow mutable T only during the call. Do not retain references, pointers or iterators into that mutable candidate after the callback returns. Read through an owning Snapshot. Each instance serializes modification and protects acquisition of snapshots/copies; separate instances can modify their independent values in parallel.

## Copy avoidance and snapshot publication

`modify` reuses the existing allocation when it has one strong owner, has never been exposed as a public Snapshot, and the callback is statically noexcept. Shared storage is copied before editing. A potentially throwing callback also receives a private candidate, even with one owner, so a failed edit preserves both value and revision.

Snapshot publication is sticky on that block. Once `snapshot()` has exposed its `shared_ptr<const T>`, that allocation remains immutable, even if its strong count later falls to one. A later modification creates a new private block, which can again be reused by subsequent noexcept writes until it is shared or published. This conservative rule protects weak aliases. The [C++ shared_ptr observer contract](https://eel.is/c++draft/util.smartptr.shared.obs) does not make use_count()==1 a synchronization or exclusive-access guarantee: a weak pointer may create a new strong owner. The implementation combines the instance gate, a publication marker observed with an atomic read-modify-write, and the count check. It never treats reference counting alone as permission to mutate a published payload.

Copying a SharedValue uses a private shared state operation and does not publish a Snapshot. Constructor, copy and assignment do not deep-copy T; payload moves during construction follow T's standard move/copy behavior. Immutable snapshot acquisition does not clone T. Serialization acquires a Snapshot and therefore publishes that allocation too.

Noexcept is a C++ callback contract, not a performance hint. Use potentially throwing callbacks for edits that can fail or allocate; the private candidate provides rollback. T must own its mutable contents or implement appropriate copy semantics: copying T does not recursively clone mutable pointees in an arbitrary shared_ptr, reference_wrapper or raw pointer member. Nested SharedValue members retain their own separation contract.

## Failure, revision and reentry

A completed `modify` advances revision once. Copy or callback failure returns `failed` with its exception cause, leaves the current payload/revision intact, and releases the candidate. Revision exhaustion returns full without invoking the editor. Assignment still throws overflow_error if the destination revision is exhausted.

`update` retains the 0.8/0.9 contract: it can overlap other work and commits only if payload identity and revision still match. A successful `modify` or assignment invalidates an older update. Conflict returns busy; no automatic retry repeats callback side effects. Use modify for short writes and update for lengthy optimistic work.

A modification callback may only edit the supplied value; it must not recursively observe, copy or assign its owning SharedValue or acquire interdependent owner locks. Recursive modify/update returns busy; recursive snapshot/copy/assignment throws logic_error instead of deadlocking. A throwing callback can catch this error or let modify return failed. Independent callers block until a serialized edit completes. A callback marked noexcept must obey its declared exception contract.

Fully constructed rejected candidates and replaced blocks retire after the instance lock is released. Construction-time partial fields unwind according to normal C++ exception rules. An external Snapshot retains its own explicit ownership. Weak references retain no payload after the last strong owner, although the standard control block can live until the last weak reference is released. Destruction requires no simultaneous calls on that same C++ instance.

## Dynamic values, queries and serialization

A `DynamicValue` can hold `SharedValue<T>` directly. Copying the any copies the small sharing owner rather than T; modifying the copied owner separates it. Raw standard string/vector values inside any keep their ordinary standard copy semantics. The sharing policy is explicit in the value type; there is no global state or automatic mutation of every std type.

Typed QueryPort requests/responses can carry SharedValue values. Admission copies the owner, and a receiving edit separates from the sender. Existing TaskScope/port removal destroys owned captures before returning runtime slots. Sharing does not change receiver cancellation, serial ordering or explicit ownership of retained snapshots.

Include `<iiSDKCore/Core/Serialization/SharedValue.h>` to enable `Codec<SharedValue<T>>` when T has a codec. The wire bytes are exactly T's logical data; control blocks, addresses, refcounts, mutexes, publication state and revisions are not persisted. Decoding creates fresh process-local storage at revision zero. Future copies share it normally. The typed payload storage is accounted against the decoder's requested-storage limit; standard control metadata is not an exact heap/RSS measure. For DynamicSerializer, register the SharedValue type under an explicit stable ID, just as other application records.

```cpp
#include <iiSDKCore/Core/Serialization/SharedValue.h>

auto bytes = ii::serialization::serialize(second);
if (bytes) {
    auto restored = ii::serialization::deserialize<
        ii::core::SharedValue<std::vector<int>>>(*bytes);
}
```

The bridge is an optional standard-only header. Core/Memory does not depend on serialization, and the core CBOR parser remains independent of both shared values and Runtime.

## Validation

[tests/sharing.cpp](../tests/sharing.cpp) verifies zero payload copies for copy/assignment, one copy for a shared write, 1,000 private noexcept writes without payload copying, immutable retained snapshots, weak publication protection, copy/edit exception rollback, reentry rejection, last-owner payload destruction, fresh serialized storage and decode storage limits.

[tests/sharing_runtime.cpp](../tests/sharing_runtime.cpp) verifies SharedValue inside any and registered dynamic serialization, a 64 KiB query payload with exactly one data copy at receiver modification, weak alias upgrade while a write is paused, 2,000 concurrent serialized counter writes, stale optimistic commit rejection and payload retirement. Actual configuration results are in [validation.md](validation.md). Copy counters describe these tested operations, not a general zero-allocation or throughput claim.

The internal storage layout changes in 0.10.0 to distinguish private mutable blocks from published immutable aliases. Rebuild consumers using this header; cross-version binary compatibility of template object layouts is not promised before 1.0.
