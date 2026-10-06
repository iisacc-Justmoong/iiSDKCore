#include <algorithm>
#include <fstream>
#include <iiSDKCore/Plugin/Plugin.h>
#include <string>
using namespace ii::plugin;
namespace {
std::string marker;
struct Unload final {
  ~Unload() {
    if (!marker.empty())
      std::ofstream(marker, std::ios::app) << 'U';
  }
} unload;
struct Instance final {
  int calls = 0;
};
void *create() noexcept {
  try {
    return new Instance;
  } catch (...) {
    return nullptr;
  }
}
void destroy(void *p) noexcept {
  delete static_cast<Instance *>(p);
  if (!marker.empty())
    std::ofstream(marker, std::ios::app) << 'D';
}
std::int32_t invoke(void *p, std::uint32_t op, const std::uint8_t *in,
                    std::uint64_t size, std::uint8_t *out,
                    std::uint64_t capacity, std::uint64_t *written) noexcept {
  try {
    ++static_cast<Instance *>(p)->calls;
    if (op == 1) {
      marker.assign(reinterpret_cast<const char *>(in), size);
      *written = 0;
      return 0;
    }
    if (op != 2 || size > capacity)
      return 1;
    for (std::uint64_t i = 0; i < size; ++i)
      out[i] = in[size - 1 - i];
    *written = size;
    return 0;
  } catch (...) {
    return 2;
  }
}
#ifdef BAD_PLUGIN_ABI
constexpr unsigned version = 99;
#else
constexpr unsigned version = abi_version;
#endif
const Descriptor descriptor{
    sizeof(Descriptor), version, "fixture", 7, create, destroy, invoke};
} // namespace
#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FIXTURE_EXPORT const Descriptor *ii_sdk_plugin_v1() noexcept {
  return &descriptor;
}
