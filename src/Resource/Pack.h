#pragma once
#include "../Core/Bytes/Bytes.h"
#include "../Core/Result/Result.h"
#include "../IO/MemoryMap.h"
#include <string_view>

namespace ii::resource {
inline constexpr std::size_t PackAlignment = 64;
enum class StorageKind { unavailable, embedded, owned, mapped };
struct Limits final {
    std::size_t maximum_entries = 1'000'000;
    std::size_t maximum_name_bytes = 4096;
    std::size_t maximum_metadata_bytes = 64 * 1024 * 1024;
};
struct Source final { std::string_view name; core::ByteView bytes; };
struct BuildOptions final { std::size_t maximum_bytes = 512 * 1024 * 1024; Limits limits; };
struct PackStats final {
    std::size_t entries = 0, unique_payloads = 0, stored_bytes = 0, metadata_bytes = 0;
    std::uint64_t logical_bytes = 0;
    StorageKind storage = StorageKind::unavailable;
};
namespace detail { struct PackData; }

// Retains its pack/mapping. Copying a view never copies the payload.
class View final {
public:
    [[nodiscard]] std::string_view name() const & noexcept { return owner_ ? name_ : std::string_view{}; }
    std::string_view name() const && = delete;
    [[nodiscard]] core::ByteView bytes() const & noexcept { return owner_ ? bytes_ : core::ByteView{}; }
    core::ByteView bytes() const && = delete;
    [[nodiscard]] bool verify() const noexcept;
    [[nodiscard]] io::Result<void> prefetch() const;
private:
    friend class Pack;
    std::shared_ptr<const detail::PackData> owner_;
    std::string_view name_;
    core::ByteView bytes_;
    std::uint64_t fingerprint_ = 0;
};
class Pack final {
public:
    // Explicit static-lifetime immutable storage; must be PackAlignment-aligned.
    [[nodiscard]] static core::Result<Pack> open_static(core::ByteView, Limits = {});
    // Copies once into aligned owned storage; later views/lookups do not copy.
    [[nodiscard]] static core::Result<Pack> copy(core::ByteView, Limits = {});
    [[nodiscard]] static core::Result<Pack> open(io::MappedRegion, Limits = {});
    [[nodiscard]] core::Result<View> find(std::string_view name) const;
    [[nodiscard]] core::Result<View> at(std::size_t index) const;
    [[nodiscard]] PackStats stats() const noexcept;
private:
    std::shared_ptr<const detail::PackData> state_;
};
// Pure in-memory build: canonical sorted names, aligned content deduplication.
// Files, codecs and target-specific compilation belong to consuming modules.
[[nodiscard]] core::Result<core::Bytes> build(std::span<const Source>, BuildOptions = {});
[[nodiscard]] bool valid_name(std::string_view) noexcept;
} // namespace ii::resource
