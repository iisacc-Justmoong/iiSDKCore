#include "Plugin.h"
#include "../Runtime/TaskScope.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <type_traits>
#if defined(IISDKCORE_STATIC_PLUGINS)
#include "StaticCatalog.h"
#endif
#if defined(_WIN32) && !defined(IISDKCORE_STATIC_PLUGINS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif !defined(__EMSCRIPTEN__) && !defined(IISDKCORE_STATIC_PLUGINS)
#include <dlfcn.h>
#endif

namespace ii::plugin {
static_assert(std::is_standard_layout_v<Descriptor> &&
              std::is_trivially_copyable_v<Descriptor>);
namespace {
struct Module final {
#if defined(_WIN32) && !defined(IISDKCORE_STATIC_PLUGINS)
  HMODULE handle = nullptr;
  ~Module() {
    if (handle)
      FreeLibrary(handle);
  }
#elif !defined(__EMSCRIPTEN__) && !defined(IISDKCORE_STATIC_PLUGINS)
  void *handle = nullptr;
  ~Module() {
    if (handle)
      dlclose(handle);
  }
#endif
};
struct State final {
  std::shared_ptr<Module> module;
  Descriptor api{};
  void *instance = nullptr;
  std::string identifier;
  std::mutex execution;
  bool created = false;
  ~State() { retire(); }
  void retire() noexcept {
    if (created) {
      created = false;
      api.destroy(instance);
      instance = nullptr;
    }
    module.reset(); // destruction of plugin objects precedes library unload
  }
  core::Result<std::vector<std::byte>> invoke(const Request &request) try {
    if (request.input.size() > 16 * 1024 * 1024 ||
        request.output_capacity > 16 * 1024 * 1024)
      return core::fail(core::ErrorCode::out_of_range,
                        "Plugin transfer exceeds 16 MiB");
    std::lock_guard lock(execution);
    std::vector<std::byte> output(request.output_capacity);
    std::uint64_t written = 0;
    const auto status = api.invoke(
        instance, request.operation,
        reinterpret_cast<const std::uint8_t *>(request.input.data()),
        request.input.size(), reinterpret_cast<std::uint8_t *>(output.data()),
        output.size(), &written);
    if (status || written > output.size())
      return core::fail(core::ErrorCode::failed,
                        "Plugin operation failed: " + std::to_string(status));
    output.resize(static_cast<std::size_t>(written));
    return output;
  } catch (...) {
    return core::fail(core::ErrorCode::failed, "Plugin host transfer failed",
                      std::current_exception());
  }
};
core::Result<std::shared_ptr<State>>
make_state(Entry entry, std::shared_ptr<Module> module = {}) {
  if (!entry)
    return core::fail(core::ErrorCode::invalid_argument,
                      "Missing plugin entry");
  const auto *descriptor = entry();
  if (!descriptor || descriptor->size < sizeof(Descriptor) ||
      descriptor->version != abi_version || !descriptor->identifier ||
      !descriptor->identifier_size || descriptor->identifier_size > 256 ||
      !descriptor->create || !descriptor->destroy || !descriptor->invoke)
    return core::fail(core::ErrorCode::unsupported,
                      "Incompatible plugin ABI descriptor");
  if (std::memchr(descriptor->identifier, 0, descriptor->identifier_size))
    return core::fail(core::ErrorCode::invalid_argument,
                      "Plugin identifier contains NUL");
  auto state = std::make_shared<State>();
  state->module = std::move(module);
  state->api = *descriptor;
  state->identifier.assign(descriptor->identifier, descriptor->identifier_size);
  state->instance = state->api.create();
  if (!state->instance)
    return core::fail(core::ErrorCode::failed, "Plugin creation failed");
  state->created = true;
  return state;
}
} // namespace
struct Plugin::Impl final {
  std::shared_ptr<State> state;
  runtime::TaskScope lifetime;
  explicit Impl(std::shared_ptr<State> value)
      : state(std::move(value)), lifetime([s = state] { s->retire(); }) {}
};
Plugin::Plugin(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}
Plugin::~Plugin() = default;
Plugin::Plugin(Plugin &&) noexcept = default;
Plugin &Plugin::operator=(Plugin &&) noexcept = default;
core::Result<Plugin> Plugin::attach(Entry entry) {
  auto state = make_state(entry);
  if (!state)
    return std::unexpected(state.error());
  return Plugin(std::make_unique<Impl>(std::move(*state)));
}
core::Result<Plugin> Plugin::open_static(std::string_view identifier) {
  if (identifier.empty() || identifier.size() > 256 ||
      identifier.find('\0') != std::string_view::npos)
    return core::fail(core::ErrorCode::invalid_argument, "Invalid static plugin identifier");
#if defined(IISDKCORE_STATIC_PLUGINS)
  Entry found = nullptr;
  for (const auto entry : detail::embedded_entries()) {
    if (!entry) continue;
    const auto *descriptor = entry();
    if (!descriptor || descriptor->size < sizeof(Descriptor) ||
        descriptor->version != abi_version || !descriptor->identifier ||
        !descriptor->identifier_size || descriptor->identifier_size > 256)
      continue;
    if (std::string_view(descriptor->identifier, descriptor->identifier_size) != identifier)
      continue;
    if (found)
      return core::fail(core::ErrorCode::already_exists, "Duplicate static plugin identifier");
    found = entry;
  }
  if (!found) return core::fail(core::ErrorCode::not_found, "Static plugin is not registered");
  return attach(found);
#else
  return core::fail(core::ErrorCode::unsupported, "Static plugin catalog is not enabled");
#endif
}
core::Result<Plugin> Plugin::load(const std::filesystem::path &path) {
  if (!path.is_absolute() ||
      path.native().find(typename std::filesystem::path::value_type{}) !=
          std::filesystem::path::string_type::npos)
    return core::fail(core::ErrorCode::invalid_argument,
                      "Plugin requires an absolute path without NUL");
#if defined(__EMSCRIPTEN__) || defined(IISDKCORE_STATIC_PLUGINS)
#if defined(IISDKCORE_STATIC_PLUGINS)
  const auto filename_utf8 = path.filename().u8string();
  const std::string filename(reinterpret_cast<const char *>(filename_utf8.data()), filename_utf8.size());
  const auto version = filename.rfind(".so.");
  std::string name;
  if (version != std::string::npos && version + 4 < filename.size() &&
      std::ranges::all_of(std::string_view(filename).substr(version + 4),
                          [](unsigned char c) { return std::isdigit(c) || c == '.'; }))
    name = filename.substr(0, version);
  else {
    const auto stem_utf8 = path.stem().u8string();
    name.assign(reinterpret_cast<const char *>(stem_utf8.data()), stem_utf8.size());
  }
  auto result = open_static(name);
  if (result || result.error().code != core::ErrorCode::not_found ||
      !name.starts_with("lib") || name.size() <= 3)
    return result;
  return open_static(std::string_view(name).substr(3));
#else
  return core::fail(core::ErrorCode::unsupported, "Use a statically registered plugin on this platform");
#endif
#else
  auto module = std::make_shared<Module>();
  Entry entry = nullptr;
#if defined(_WIN32)
  auto native_path = path;
  native_path.make_preferred();
  module->handle = LoadLibraryExW(native_path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module->handle)
    return core::fail(core::ErrorCode::not_found,
                      "Plugin LoadLibraryEx failed: " +
                          std::to_string(GetLastError()));
  auto symbol = GetProcAddress(module->handle, "ii_sdk_plugin_v1");
  static_assert(sizeof(symbol) == sizeof(entry));
  std::memcpy(&entry, &symbol, sizeof(entry));
#else
  module->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module->handle)
    return core::fail(core::ErrorCode::not_found,
                      std::string("Plugin dlopen failed: ") + dlerror());
  auto symbol = dlsym(module->handle, "ii_sdk_plugin_v1");
  static_assert(sizeof(symbol) == sizeof(entry));
  std::memcpy(&entry, &symbol, sizeof(entry));
#endif
  auto state = make_state(entry, std::move(module));
  if (!state)
    return std::unexpected(state.error());
  return Plugin(std::make_unique<Impl>(std::move(*state)));
#endif
}
std::string Plugin::identifier() const {
  return impl_ ? impl_->state->identifier : std::string{};
}
core::Result<std::vector<std::byte>>
Plugin::invoke(const Request &request) const {
  if (!impl_)
    return core::fail(core::ErrorCode::closed, "Plugin moved or closed");
  if (impl_->lifetime.is_current())
    return core::fail(core::ErrorCode::busy, "Recursive plugin invocation");
  core::Result<std::vector<std::byte>> result;
  const auto admission =
      impl_->lifetime.run([&] { result = impl_->state->invoke(request); });
  if (admission != runtime::SubmitError::none)
    return core::fail(core::ErrorCode::closed, "Plugin is closed");
  return result;
}
runtime::Submission
Plugin::post(runtime::Runtime &runtime, Request request,
             std::function<void(core::Result<std::vector<std::byte>>)>
                 completion) const {
  if (!impl_)
    return {{}, runtime::SubmitError::stopped};
  if (!completion || request.input.size() > 16 * 1024 * 1024 ||
      request.output_capacity > 16 * 1024 * 1024)
    return {{}, runtime::SubmitError::invalid};
  return impl_->lifetime.post(runtime, [state = impl_->state,
                                        request = std::move(request),
                                        completion = std::move(completion)] {
    completion(state->invoke(request));
  });
}
runtime::ThreadResult Plugin::close() const {
  return impl_ ? impl_->lifetime.close() : runtime::ThreadResult{};
}
} // namespace ii::plugin
