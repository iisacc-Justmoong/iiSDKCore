# Native application resources (0.17)

Core supplies immutable resource bytes, indexed lookup, shared lifetime and
native read-only mappings. iiSDKBuilder is the build-host compiler. File
creation/publication belongs to iiFileProvider. No GUI, codec, storage SDK,
compression library, Qt, runtime interpreter or asset registry is imported into
Core. Image/texture/audio/shader conversion remains in its owning feature module;
the resulting native bytes can be selected and packaged for a target.

## Storage and native optimization

- Embedded packs compile to aligned const data in the target executable/library.
  Runtime access does not need the input files, compiler or FileProvider.
- External packs use MappedRegion: Windows CreateFileMapping/MapViewOfFile and
  POSIX mmap on macOS, Linux, iOS and Android. The public API accepts an existing
  Channel, never creates/opens an application file by path, and never returns a
  native handle. Obtain file Channels through iiFileProvider CoreIO.
- Payloads begin at 64-byte boundaries. A copied pack allocates aligned storage
  once; an embedded or mapped pack does not copy the payload into a heap buffer.
  Resource views share ownership, including after pack handles/channels disappear.
- The pack has a sorted UTF-8 name index; successful lookup uses binary search
  without allocating a name or payload. Native OS page caching supplies mapped
  data on demand; there is no duplicate application-wide cache or global registry.
- The in-memory builder sorts entries and deduplicates equal content, verifying
  bytes even when fingerprints collide. Aliases share one payload address.
  Source order, timestamps and host pointers do not enter the pack.

Metadata validation reads the index/names, not every payload. Explicit verify()
checks a payload fingerprint when needed. This preserves demand paging for large
assets. Fingerprints detect accidental changes; they provide no authenticity,
signature, encryption or hostile-writer protection. All byte/offset decoding is
explicit little endian; no pointer or native C++ object layout is serialized.
Consumers must decode their own payload format instead of reinterpreting arbitrary
bytes as live C++ objects.

## Runtime use

~~~cpp
#include <iiSDKCore/Resource/Pack.h>

// channel is supplied by the application/storage provider.
auto mapping=ii::io::MappedRegion::map(channel);
if (!mapping) { /* handle io::Error and its native error_code */ }
else {
    auto pack=ii::resource::Pack::open(std::move(*mapping));
    if (pack) {
        auto icon=pack->find("icons/app.svg");
        if (icon) {
            auto retained=*icon; // Owns the backing mapping, without payload copy.
            auto bytes=retained.bytes();
            const bool intact=retained.verify();
            // Keep retained alive while a decoder/upload operation uses bytes.
        }
    }
}
~~~

Pack::copy accepts arbitrary byte alignment and makes an aligned owned copy.
Pack::open_static is an explicit immutable static-lifetime contract requiring
64-byte alignment; use it only for static storage, as emitted by Builder.
Pack::open accepts an owned MappedRegion whose logical base is also 64-byte aligned. For a pack inside a container, its file offset must preserve that alignment; otherwise choose an explicit aligned copy. No hidden copying fallback occurs. Pack handles/views are copyable;
querying immutable copies concurrently is supported. Assignment/removal of the
same handle still requires caller synchronization.

Default and moved-from views expose empty bytes/names and fail verification. Keep a View while using its string_view/span. Accessors reject direct temporary
borrowing, but C++ cannot prove the lifetime of a view extracted through another
temporary wrapper. No mutable payload API is provided. Core Operation<View> or
Operation<Pack> can return an owning result to EventLoop/iiGUI through then_on
and a receiver TaskScope. Use Runtime workers for file mapping, page warming,
verification and decoding that could otherwise stall a UI thread.

## Mapping and paging hints

MappedRegion::map supports bounded subranges and handles OS page/allocation
granularity internally. slice shares the view. Closing a Channel prevents new
maps while existing maps remain alive. An empty file/range produces an empty
native view without asking the OS to map zero bytes.

The backing file must remain immutable while mapped. Another writer truncating
or modifying the same file can cause an OS fault or inconsistent bytes. Storage
publication should use a new file and provider-controlled replacement; whether
replacement is permitted while mapped remains a filesystem/OS contract.
Mapping is not a general snapshot or a security boundary.

prefetch requests paging for the selected subrange. POSIX uses MADV_WILLNEED;
Windows resolves PrefetchVirtualMemory dynamically when the compiled Windows
SDK exposes the Windows 8+ signature and the running OS supplies it. Windows
access-pattern advice is explicitly unsupported. POSIX normal/random/sequential
advice uses madvise. These are hints, not residency, asynchronous completion,
deadline or speedup guarantees; native errors are returned.

WASM reports OS-backed mapping unsupported. It can use the same embedded packs
or an explicit owned copy of downloaded/host-provided bytes. A virtual filesystem
copy is never presented as native zero-copy mapping.

## Build-host compiler and CMake

iiSDKBuilder 0.2 adds resources, using Core 0.17's pure in-memory builder and
iiFileProvider for every output. The installed iiSDKBuilderResources.cmake module
can be included without importing/linking Builder into the target application.

~~~cmake
find_package(iiSDKCore 0.17 CONFIG REQUIRED COMPONENTS Runtime)
include("/host/Builder/lib/cmake/iiSDKBuilder/iiSDKBuilderResources.cmake")
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE iiSDKCore::Runtime)
iisdkbuilder_add_resources(TARGET MyApp MANIFEST resources.json
    COMPILER "/host/Builder/bin/iisdk-builder")
~~~

The helper selects the CMake target platform and emits a .cpp, .h, .iirp and
depfile under the target's build tree. Cross builds require an explicit
build-host compiler; no target executable/emulator is run to generate resources.
The same C++ data source compiles through the native Windows/macOS/Linux/Android/
iOS toolchain or Emscripten. Asset/manifest changes rebuild; unchanged bytes retain
timestamps. Source dependency paths with spaces/UTF-8/#/$ are escaped in depfiles.

The source manifest can contain a common resource and one platform override:

~~~json
{
  "schema": 1,
  "name": "app_assets",
  "resources": [
    {"name": "icons/app", "source": "assets/common.svg"},
    {"name": "icons/app", "source": "assets/apple.svg", "platform": "macos"}
  ]
}
~~~

Source paths resolve relative to the manifest. Exact target overrides common
content; other-platform-only entries are omitted before file reads. Duplicate
name/platform pairs, unknown fields/platforms and invalid logical paths fail.
Keys are case-sensitive UTF-8 without Unicode normalization: no empty/dot/parent
segments, leading/trailing slash, backslash, colon or NUL. Pack names are
nonreserved ASCII identifiers, unique among packs linked into the same binary.
Generated declarations are ii::resource::embedded::load_app_assets().

The CLI accepts --manifest, --platform, paired --cpp/--header, optional --pack and
--depfile. Binary-only --pack is suitable for large external assets. Outputs stay
under build/ and cannot replace any source/manifest or each other. All inputs are
validated/encoded before publication; each file is atomically published by the
provider, but the multi-file group is not one filesystem transaction.

The default compiler input and encoded-output budgets are each 512 MiB; a
manifest's maximumBytes sets their limit. Embedded C++ has a separate 32 MiB
limit to bound compiler cost. The Core in-memory builder's budget bounds encoded
bytes. Parser defaults cap entries at one million, each name at 4096 bytes and
metadata at 64 MiB. Small/unique resources can grow because of metadata/alignment;
deduplication is not compression. No automatic image/texture/shader conversion,
GPU-format choice, lossy processing or hidden fallback is performed.

## Version 1 binary format

All integers are unsigned 64-bit little endian. Header size is 64 bytes, entry
size 48 bytes; the pack base and every payload offset are 64-byte aligned.

| Header offset | Meaning |
|---|---|
| 0 | Eight bytes: IIRPACK followed by NUL |
| 8 | Version 1; high 32 flag bits are zero |
| 16 | Total byte extent |
| 24 | Entry count |
| 32 | Name table start, exactly 64 + count * 48 |
| 40 | Payload start / metadata extent, rounded to 64 |
| 48 | FNV-1a 64 metadata fingerprint, excluding bytes 48..55 |
| 56 | Reserved zero |

Each entry contains name offset/length, payload offset/length, content FNV-1a 64
and reserved zero. Names are contiguous and lexicographically sorted; payloads
are aligned and nonoverlapping, except exact aliases. Unindexed trailing extents,
range overflow, malformed names, budgets and invalid metadata fail before views
are exposed. Fingerprints use offset basis 14695981039346656037 and prime
1099511628211. The metadata fingerprint ends at payload start. Empty payloads
are valid; no host endian/alignment-dependent struct access is used.

## Tests and evidence

tests/resources.cpp covers byte fidelity, aliases/alignment, owned/static/mapped
lifetime, bounds, all truncated extents, header corruption, correctly
fingerprinted malformed metadata, concurrent immutable access, parser/build
budgets and Operation delivery. Its repeated-lookup fixture reports logical/
stored bytes and elapsed time; it does not assert a machine-independent latency
or compare against Qt. Installed consumers run the same contract.

Builder tests cover selection, no read of omitted files, source preservation,
output preservation on validation/encoding failures, deterministic timestamps, real generated C++ execution
without the source assets, incremental rebuilds and escaped dependency paths.
HelloResources is a common native consumer. See validation.md and Builder's
docs/Resources.md for recorded platform evidence and reproduction.

Native API references: [Microsoft MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile),
[Microsoft PrefetchVirtualMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-prefetchvirtualmemory),
[Apple mmap](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html),
[Apple madvise](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/madvise.2.html).
