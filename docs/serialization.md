# Data type serialization (0.9.0)

`ii::serialization` serializes owned C++ values to CBOR and reconstructs new owned values. `Core/Serialization/CBOR.h` owns the wire parser, limits and errors; `Core/Serialization/Serialization.h` owns typed codecs. Both are header-only C++23 and use only the standard library. Link `iiSDKCore::Values` with `find_package(iiSDKCore 0.9 CONFIG REQUIRED COMPONENTS Values)` to use them independently of Runtime, Threads, Qt and native hardware libraries.

`Runtime/Serialization.h` adds an instance-owned `DynamicSerializer` for the existing `DynamicValue` (`std::any`). It uses explicit type registration and standard synchronization. It never exports an object's layout, addresses, `typeid().name()`, implementation hashes, private fields, methods or query ports.

## Wire profile

The format is [CBOR, RFC 8949](https://www.rfc-editor.org/rfc/rfc8949), restricted to these application data types:

- Unsigned integers up to `UINT64_MAX` and negative integers using major type 1. Typed signed decoding checks the destination range, including `INT64_MIN`. Positive CBOR integers have no C++ signedness or width; the static destination codec or dynamic envelope supplies that information.
- Boolean, null, IEEE 754 binary16/binary32/binary64 floating point, definite-length byte strings and valid UTF-8 text.
- Definite-length arrays and maps with unique UTF-8 text keys.

Encoding follows core deterministic encoding for this profile: shortest integer and length arguments, the shortest floating-point representation that preserves the value, canonical NaN `f9 7e 00`, and bytewise lexical order of encoded map keys. Signed zero is preserved; floating point remains floating point even when integral. Standard map codecs sort keys by UTF-8 byte length then byte order, equivalent to encoded byte order for these text keys. Encoder finalization checks the complete output, including order and uniqueness of custom codec map keys.

Decoding accepts valid nonpreferred integer lengths, binary16/32/64 floats and unsorted maps, then normalizes them when re-encoded. Duplicate keys, invalid UTF-8, reserved additional-information values, trailing bytes and incomplete input fail. Indefinite lengths, tags (including self-described CBOR and typed-array tags), arbitrary map key types, undefined and other simple values are outside this profile and are rejected. This is a defined subset, not an implementation of every CBOR feature. Text validation does not perform Unicode normalization.

Examples of stable bytes:

| Value | Bytes (hex) |
| --- | --- |
| `uint64_t{24}` | `18 18` |
| `int64_t{-1000}` | `39 03 e7` |
| `uint64_t{UINT64_MAX}` | `1b ff ff ff ff ff ff ff ff` |
| `1.5` | `f9 3e 00` |
| `-0.0` | `f9 80 00` |
| UTF-8 `"한"` | `63 ed 95 9c` |
| `vector<int>{1,2,3}` | `83 01 02 03` |
| map `{"b":1,"aa":2}` | `a2 61 62 01 62 61 61 02` |

No SDK magic prefix is added to static CBOR. The caller selects the static type; custom record schemas explicitly include their own version when evolution is needed. CBOR does not authenticate, encrypt, compress or frame an external stream. Read a complete bounded message before decoding; channel/file/transport ownership remains with the existing I/O layer. USB/HID/MIDI protocol wire formats remain separate contracts.

## Supported C++ types

| Type | Mapping |
| --- | --- |
| Integral types of at most 64 bits, except bool | CBOR integer; checked range on decode |
| `bool`, `nullptr_t`, `monostate` | Boolean or null |
| `float`, `double` | Floating point; float decoding rejects finite precision loss or overflow |
| `string` | Owned UTF-8 text, embedded NUL supported |
| `vector<byte>`, `array<byte,N>` | Byte string; fixed size checked |
| `vector<T>` (including bool), `array<T,N>` | Array; element codecs, fixed size checked |
| `map<string,T>`, `unordered_map<string,T>` | Unique-key map, sorted during encoding |
| `optional<T>` | Zero-or-one-element array; nested optional states remain distinct |
| `tuple<T...>`, `pair<A,B>` | Positional array with exact length |
| `variant<T...>` | `[alternative_index, payload]`; declaration order is part of the schema |
| Application record or enum | Explicit `Codec<T>` specialization |

Pointers, references, `reference_wrapper`, `string_view`, spans, `long double`, native handles, arbitrary `std::any`, class memory dumps and automatic reflection have no implicit codec. This prevents dangling decoded views and ABI-dependent persistence. Enums need explicit validation of their permitted values. Time points and durations need an explicit schema identifying epoch and units. Stateful custom comparators/allocators are not serialized; decoded standard maps use default-constructed policy objects, whose equality rules can also reject colliding keys.

```cpp
#include <iiSDKCore/Core/Serialization/Serialization.h>

std::vector<double> samples{0.25, 0.5, 0.75};
auto bytes = ii::serialization::serialize(samples);
if (!bytes) {
    // error().code, offset, message and optional exception cause
    return;
}
auto restored = ii::serialization::deserialize<std::vector<double>>(*bytes);
```

The free functions perform complete-message validation. Direct `Encoder`/`Decoder` primitives are available for custom schemas; direct readers must call `finish()` and perform schema validation. A failed operation is sticky, so ignoring an error inside a codec cannot turn a partial operation into successful output. `Encoder::finish()` moves out a valid payload; encoder and decoder instances are per-message state and should not be reused for another message.

## Record extension and schema versions

Specialize `Codec<T>` with `static Result<void> encode(Encoder&, const T&)` and `static Result<T> decode(Decoder&)`. It is a final, composed value contract. Check every primitive result, validate field count/version and return a new value. Call `write`/`read` for nested fields to retain limits and exception handling. Use `reject<T>(code, "static message")` for domain errors; error message text must have static storage duration. Custom allocations must use `allocate(count, sizeof(Element))` before requesting storage when they are to count toward the storage budget.

[tests/serialization_record.h](../tests/serialization_record.h) is a concrete audio/sample DTO schema: `[1, sequence, label, samples]`. Its decoder migrates `[0, sequence, samples]` by supplying `label="legacy"`; it rejects unknown versions and wrong field counts. This is explicit application-controlled migration, not inferred reflection. Record field order, variant alternative order, meanings and units are schema decisions.

Codecs may throw. The wrapper returns `codec_failed` with an `exception_ptr`; already constructed local values and buffers retire through RAII before returning. The returned error owns its exception until that error is released. Arbitrary codec external effects, allocations without accounting or global mutable state are the codec author's responsibility.

## Dynamic values

`DynamicSerializer` emits either `[]` for an empty `std::any`, or `[stable_id, codec_version, payload]`. Explicit `nullptr` is a registered value and differs from an empty any. The outer codec version is a positive `uint32_t` and must match the receiving registry exactly. Migration inside a compatible codec is explicit in its payload schema. Unknown IDs, unsupported C++ types and version mismatches return errors.

```cpp
#include <iiSDKCore/Runtime/Serialization.h>

ii::runtime::DynamicSerializer serializer;
auto bytes = serializer.serialize(ii::runtime::DynamicValue{42});
if (bytes) {
    auto value = serializer.deserialize(*bytes);
    // successful result has exact type int; use value_cast<int>() or any_cast
}
// For a copyable application DTO with Codec<Sample>:
// auto registered = serializer.register_type<Sample>("app.sample", 1);
```

The default registry installs 17 exact C++ types with stable IDs: `cpp.bool`, `cpp.char`, `cpp.signed-char`, `cpp.unsigned-char`, `cpp.short`, `cpp.unsigned-short`, `cpp.int`, `cpp.unsigned-int`, `cpp.long`, `cpp.unsigned-long`, `cpp.long-long`, `cpp.unsigned-long-long`, `cpp.float`, `cpp.double`, `cpp.string`, `cpp.bytes`, `cpp.null`. C++ typedef aliases are the same registered type. Platform integer widths and char signedness can differ; range checks apply on the receiver. Prefer static fixed-width schemas for protocols with strict numerical widths.

`DynamicSerializer(false, capacity)` starts empty. IDs are 1..128 ASCII letters/digits or `._-/`. Duplicate IDs or C++ types are rejected; the default capacity is 64 including builtins. A builtin registry capacity below 17 throws at construction. Registrations and lookups are synchronized; each operation copies a small stateless codec entry and runs the codec after releasing the lock, allowing independent calls in parallel. A custom codec must be thread-safe. Registry destruction must follow completion of callers referencing that instance, as with other C++ objects.

There is no process-wide registry, runtime object base, inheritance registration or automatic traversal of ObjectProperties. Serialize only an explicitly selected DTO/snapshot. Releasing a result, decoder or registry releases its owned data; no worker, device or quota reservation is retained by serialization itself.

## Bounds, errors and runtime composition

The default `Limits` are 16 MiB input/output, 8 MiB per text/byte string, 100,000 entries per container, 200,000 total CBOR items, depth 64, and 32 MiB cumulative requested storage per validation/typed-construction phase. Strings, vectors, map entries and map sorting scratch are accounted before allocation. Declared lengths are checked against limits and the remaining input before container storage is requested; arithmetic uses division to avoid multiplication overflow. Wire nesting and nested codec calls are both bounded.

Requested-storage accounting is a guardrail, not an exact heap/RSS ceiling. Allocator metadata, standard unordered bucket policy, stack objects and unaccounted custom code are not included. Encoded vector capacity is capped at `max_bytes`; arbitrary custom code is not an allocation-free or real-time promise. Validation completes before typed construction, so invalid complete messages never publish a partially populated destination. Parsing errors report the cursor position where the problem was detected; registry errors have offset zero. No existing destination object is mutated.

Serialization is synchronous per message. Run independent batches on existing Runtime workers or `parallel_transform`; a stateful object uses its SerialQueue and a typed QueryPort. [examples/serialization.cpp](../examples/serialization.cpp) sends a sample-map query to a persistent worker and restores the result. A long codec is not preempted and must not occupy a time-sensitive callback. Transport backpressure, live timestamps and cancellation remain the corresponding runtime/I/O contracts. AtomicFile can persist the owned bytes using its existing commit/cancel semantics; the dynamic tests verify this complete path.

## Verification

[tests/serialization.cpp](../tests/serialization.cpp) checks exact wire fixtures, every binary16 bit pattern, random 64-bit integer/floating-point roundtrips, malformed random input, every truncated prefix of a record, UTF-8 failures, duplicate keys, precision/range errors and each limit. [tests/dynamic_serialization.cpp](../tests/dynamic_serialization.cpp) checks exact dynamic types, custom registration, ID/type/version rejection, exceptions, sticky failures, owned object retirement, atomic-file persistence and concurrent use. See [validation.md](validation.md) for executed configurations and cross-build scope.

## Shared values (0.10.0)

Include `Core/Serialization/SharedValue.h` for the optional `Codec<core::SharedValue<T>>`. It emits T's exact logical wire format and restores fresh storage at revision zero. Owner counts, locks, addresses and revisions remain process-local. The payload block's requested storage is counted before construction. DynamicSerializer can register this copyable wrapper under an application stable ID. The sharing and snapshot publication contracts are in [implicit-sharing.md](implicit-sharing.md).
