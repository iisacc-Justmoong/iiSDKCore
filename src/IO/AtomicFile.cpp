#include "AtomicFile.h"
#include <atomic>
#include <mutex>
#include <random>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace ii::io {
struct AtomicFile::Impl final {
  mutable std::mutex mutex;
  std::filesystem::path target, temporary;
  Channel channel;
  bool finished = false;
  Result<void> retire() {
    auto closed = channel.close();
    if (!closed)
      return closed;
    std::error_code error;
    std::filesystem::remove(temporary, error);
    if (error)
      return std::unexpected(Error{ErrorCode::system_error, error});
    finished = true;
    return {};
  }
  ~Impl() {
    if (!finished)
      (void)retire();
  }
};
AtomicFile::AtomicFile(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}
AtomicFile::~AtomicFile() = default;
AtomicFile::AtomicFile(AtomicFile &&) noexcept = default;
AtomicFile &AtomicFile::operator=(AtomicFile &&) noexcept = default;
Result<AtomicFile> AtomicFile::create(const std::filesystem::path &path) {
  if (path.empty() || path.filename().empty() ||
      path.native().find(typename std::filesystem::path::value_type{}) !=
          std::filesystem::path::string_type::npos)
    return std::unexpected(Error{ErrorCode::invalid_argument, {}});
  std::error_code ec;
  auto target = std::filesystem::absolute(path, ec);
  if (ec)
    return std::unexpected(Error{ErrorCode::system_error, ec});
  static std::atomic<std::uint64_t> sequence{0};
  std::random_device random;
  const auto nonce = (static_cast<std::uint64_t>(random()) << 32) ^ random();
  for (unsigned retry = 0; retry < 64; ++retry) {
    auto temporary =
        target.parent_path() /
        (".ii-save-" + std::to_string(nonce) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    auto state = std::make_unique<Impl>();
    state->finished = true;
    state->target = target;
    state->temporary = std::move(temporary);
    auto channel =
        open_file(state->temporary, {Access::write, Creation::create_new});
    if (!channel) {
      if (channel.error().code == ErrorCode::already_exists)
        continue;
      return std::unexpected(channel.error());
    }
    state->channel = std::move(*channel);
    state->finished = false;
    return AtomicFile(std::move(state));
  }
  return std::unexpected(Error{ErrorCode::already_exists, {}});
}
Result<std::size_t> AtomicFile::write(std::span<const std::byte> data) {
  if (!impl_)
    return std::unexpected(Error{ErrorCode::closed, {}});
  std::lock_guard lock(impl_->mutex);
  if (impl_->finished)
    return std::unexpected(Error{ErrorCode::closed, {}});
  std::size_t offset = 0;
  while (offset < data.size()) {
    auto written = impl_->channel.write(data.subspan(offset));
    if (!written)
      return std::unexpected(written.error());
    if (!*written)
      return std::unexpected(Error{ErrorCode::system_error, {}});
    offset += *written;
  }
  return offset;
}
Result<void> AtomicFile::commit() {
  if (!impl_)
    return std::unexpected(Error{ErrorCode::closed, {}});
  std::lock_guard lock(impl_->mutex);
  if (impl_->finished)
    return std::unexpected(Error{ErrorCode::closed, {}});
  auto flushed = impl_->channel.flush();
  if (!flushed)
    return flushed;
  auto closed = impl_->channel.close();
  if (!closed)
    return closed;
  std::error_code error;
#if defined(_WIN32)
  if (!MoveFileExW(impl_->temporary.c_str(), impl_->target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
#else
  std::filesystem::rename(impl_->temporary, impl_->target, error);
#endif
  if (error) {
    (void)impl_->retire();
    return std::unexpected(Error{ErrorCode::system_error, error});
  }
  impl_->finished = true;
  return {};
}
Result<void> AtomicFile::cancel() {
  if (!impl_)
    return {};
  std::lock_guard lock(impl_->mutex);
  if (impl_->finished)
    return {};
  return impl_->retire();
}
std::filesystem::path AtomicFile::temporary_path() const {
  if (!impl_)
    return {};
  std::lock_guard lock(impl_->mutex);
  return impl_->temporary;
}
} // namespace ii::io
