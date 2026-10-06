#pragma once
#include "Channel.h"
#include <utility>

namespace ii::io {
// Exclusive temporary in the target directory, then an atomic name replacement.
// No direct-write fallback. Commit flushes data; it does not promise directory
// durability after sudden power loss on every filesystem.
class AtomicFile final {
public:
  [[nodiscard]] static Result<AtomicFile>
  create(const std::filesystem::path &target);
  ~AtomicFile();
  AtomicFile(AtomicFile &&) noexcept;
  AtomicFile &operator=(AtomicFile &&) noexcept;
  AtomicFile(const AtomicFile &) = delete;
  AtomicFile &operator=(const AtomicFile &) = delete;
  [[nodiscard]] Result<std::size_t> write(std::span<const std::byte> data);
  [[nodiscard]] Result<void> commit();
  [[nodiscard]] Result<void> cancel();
  [[nodiscard]] std::filesystem::path temporary_path() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AtomicFile(std::unique_ptr<Impl>);
};
} // namespace ii::io
