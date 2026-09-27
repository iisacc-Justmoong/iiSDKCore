#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <system_error>

namespace ii::io {

enum class ErrorCode { invalid_argument, not_found, permission_denied, already_exists,
    closed, unsupported, would_block, timed_out, cancelled, busy, system_error };
struct Error { ErrorCode code; std::error_code native; };
template<class T> using Result = std::expected<T, Error>;
enum class Access { read, write, read_write };
enum class Creation { existing, open_or_create, create_new, truncate };
struct FileOptions { Access access = Access::read; Creation creation = Creation::existing; };
// Raw binary 8N1, no software/hardware flow control.
struct SerialOptions { unsigned baud_rate = 115200; };
enum class ChannelKind { file, serial };
enum class Backend { posix, windows, wasm_virtual };
enum class Interest { readable, writable };
enum class SeekOrigin { begin, current, end };
struct Capabilities {
    ChannelKind kind = ChannelKind::file;
    Backend backend = Backend::posix;
    bool readable = false, writable = false, seekable = false, flushable = false;
};
struct PlatformCapabilities { Backend backend; bool files; bool serial; };
[[nodiscard]] PlatformCapabilities platform_capabilities() noexcept;
struct Transfer { std::size_t bytes = 0; bool eof = false; };

// A minimal shared cancellation flag, including toolchains without stop_token.
// No callbacks, thread ownership or allocation in token copies/checks.
class CancellationSource;
class CancellationToken final {
public:
    CancellationToken() = default;
    [[nodiscard]] bool cancelled() const noexcept { return state_ && state_->load(std::memory_order_acquire); }
private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state) : state_(std::move(state)) {}
    std::shared_ptr<std::atomic<bool>> state_;
};
class CancellationSource final {
public:
    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }
    bool cancel() const noexcept { return state_ && !state_->exchange(true, std::memory_order_acq_rel); }
private:
    std::shared_ptr<std::atomic<bool>> state_ = std::make_shared<std::atomic<bool>>(false);
};

// A copy is another owning reference to the SAME endpoint and file cursor.
// Operations are serialized internally; concurrent use returns busy. Closing
// one reference closes all aliases. No native handle escapes this boundary.
class Channel final {
public:
    Channel() = default;
    [[nodiscard]] Capabilities capabilities() const noexcept;
    [[nodiscard]] Result<Transfer> read(std::span<std::byte> destination) const;
    [[nodiscard]] Result<std::size_t> write(std::span<const std::byte> source) const;
    [[nodiscard]] Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin = SeekOrigin::begin) const;
    [[nodiscard]] Result<std::uint64_t> size() const;
    [[nodiscard]] Result<void> flush() const;
    // Zero timeout is a probe (would_block); positive timeout can time out.
    // The stop token cancels readiness waiting, not an in-progress OS transfer.
    [[nodiscard]] Result<void> wait(Interest interest, std::chrono::milliseconds timeout,
                                    CancellationToken stop = {}) const;
    [[nodiscard]] Result<void> close() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    explicit Channel(std::shared_ptr<State> state) : state_(std::move(state)) {}
    friend Result<Channel> open_file(const std::filesystem::path&, FileOptions);
    friend Result<Channel> open_serial(const std::filesystem::path&, SerialOptions);
};

[[nodiscard]] Result<Channel> open_file(const std::filesystem::path& path, FileOptions options = {});
[[nodiscard]] Result<Channel> open_serial(const std::filesystem::path& path, SerialOptions options = {});

} // namespace ii::io
