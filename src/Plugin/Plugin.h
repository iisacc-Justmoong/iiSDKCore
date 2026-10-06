#pragma once
#include "../Core/Result/Result.h"
#include "../Runtime/Runtime.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ii::plugin {
inline constexpr std::uint32_t abi_version = 1;
// Fixed C layout/function calling convention. No std object, exception or
// allocator ownership crosses this boundary. Byte spans are borrowed per call.
struct Descriptor final {
  std::uint32_t size, version;
  const char *identifier;
  std::uint32_t identifier_size;
  void *(*create)() noexcept;
  void (*destroy)(void *) noexcept;
  std::int32_t (*invoke)(void *, std::uint32_t, const std::uint8_t *,
                         std::uint64_t, std::uint8_t *, std::uint64_t,
                         std::uint64_t *) noexcept;
};
using Entry =
    const Descriptor *(*)() noexcept; // exported C name: ii_sdk_plugin_v1
struct Request final {
  std::uint32_t operation = 0;
  std::vector<std::byte> input;
  std::size_t output_capacity = 4096;
};
class Plugin final {
public:
  // Full paths avoid process-global library search-path mutation.
  [[nodiscard]] static core::Result<Plugin>
  load(const std::filesystem::path &path);
  // Static registration is the portable iOS/WASM path. Entry code must outlive
  // the instance; the host does not acquire/unload a library for this form.
  [[nodiscard]] static core::Result<Plugin> attach(Entry entry);
  ~Plugin();
  Plugin(Plugin &&) noexcept;
  Plugin &operator=(Plugin &&) noexcept;
  Plugin(const Plugin &) = delete;
  Plugin &operator=(const Plugin &) = delete;
  [[nodiscard]] std::string identifier() const;
  [[nodiscard]] core::Result<std::vector<std::byte>>
  invoke(const Request &request) const;
  [[nodiscard]] runtime::Submission
  post(runtime::Runtime &runtime, Request request,
       std::function<void(core::Result<std::vector<std::byte>>)> completion)
      const;
  // Blocks new calls, cancels queued calls, waits for active native calls,
  // destroys the plugin instance, then releases the loaded library.
  [[nodiscard]] runtime::ThreadResult close() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Plugin(std::unique_ptr<Impl>);
};
} // namespace ii::plugin
