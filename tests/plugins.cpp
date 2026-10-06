#include <atomic>
#include <fstream>
#include <future>
#include <iiSDKCore/Plugin/Plugin.h>
#include <iostream>
#include <latch>
#include <thread>
using namespace ii;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
namespace {
std::atomic<int> created = 0, destroyed = 0;
std::latch *entered = nullptr;
std::latch *release = nullptr;
void *create() noexcept {
  ++created;
  return &created;
}
void destroy(void *) noexcept { ++destroyed; }
std::int32_t invoke(void *, std::uint32_t operation, const std::uint8_t *input,
                    std::uint64_t size, std::uint8_t *output,
                    std::uint64_t capacity, std::uint64_t *written) noexcept {
  if (operation == 1) {
    entered->count_down();
    release->wait();
  }
  if (operation == 2) {
    *written = capacity + 1;
    return 0;
  }
  if (size > capacity)
    return 1;
  for (std::uint64_t i = 0; i < size; ++i)
    output[i] = input[i];
  *written = size;
  return 0;
}
const plugin::Descriptor api{sizeof(plugin::Descriptor),
                             plugin::abi_version,
                             "static-test",
                             11,
                             create,
                             destroy,
                             invoke};
const plugin::Descriptor *entry() noexcept { return &api; }
const plugin::Descriptor *bad_entry() noexcept {
  static auto bad = api;
  bad.version = 99;
  return &bad;
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(!plugin::Plugin::attach(bad_entry) &&
              !plugin::Plugin::attach(nullptr),
          "ABI mismatch and null entry rejected");
    auto instance = plugin::Plugin::attach(entry);
    check(instance && instance->identifier() == "static-test",
          "static plugin identifier");
    plugin::Request request{.input = {std::byte{1}, std::byte{2}}};
    check(instance->invoke(request).value() == request.input,
          "static plugin invocation");
    auto invalid = request;
    invalid.output_capacity = 16 * 1024 * 1024 + 1;
    check(!instance->invoke(invalid), "bounded plugin output");
    invalid = request;
    invalid.operation = 2;
    check(!instance->invoke(invalid), "invalid plugin response length");
    if (runtime::Thread::supported()) {
      runtime::Runtime runtime({.workers = 1, .capacity = 4});
      std::latch start(1), gate(1);
      entered = &start;
      release = &gate;
      std::promise<void> done;
      request.operation = 1;
      check(bool(instance->post(runtime, request,
                                [&](auto result) {
                                  check(bool(result), "active invocation");
                                  done.set_value();
                                })),
            "async plugin post");
      start.wait();
      auto closing =
          std::async(std::launch::async, [&] { return instance->close(); });
      while (instance->post(runtime, {}, [](auto) {}))
        std::this_thread::yield();
      check(destroyed == 0, "plugin destruction waits active operation");
      gate.count_down();
      done.get_future().wait();
      check(bool(closing.get()) && destroyed == 1 &&
                runtime.stats().pending == 0,
            "close destroys instance and returns quota");
      check(bool(runtime.join()), "plugin runtime joined");
    } else
      check(bool(instance->close()) && destroyed == 1, "static plugin close");
    check(!instance->invoke(request), "closed plugin calls rejected");
    if (argc == 3) {
      auto path = std::filesystem::absolute(argv[1]);
      check(!plugin::Plugin::load("relative-plugin"),
            "relative plugin rejected");
      check(!plugin::Plugin::load(std::filesystem::absolute(argv[2])),
            "dynamic ABI mismatch rejected");
      auto loaded = plugin::Plugin::load(path);
      check(bool(loaded), "native library loaded");
      auto marker =
          std::filesystem::current_path() / "plugin-unload-marker.dat";
      std::error_code ec;
      std::filesystem::remove(marker, ec);
      auto text = marker.string();
      plugin::Request set{.operation = 1};
      for (auto c : text)
        set.input.push_back(static_cast<std::byte>(c));
      check(bool(loaded->invoke(set)), "fixture marker");
      auto output =
          loaded->invoke({.operation = 2,
                          .input = {std::byte{1}, std::byte{2}, std::byte{3}}});
      check(output && *output ==
                          std::vector{std::byte{3}, std::byte{2}, std::byte{1}},
            "native ABI bytes");
      check(bool(loaded->close()), "native plugin clean close");
      std::ifstream stream(marker);
      std::string order;
      stream >> order;
      check(order == "DU", "instance destruction precedes module unload");
      stream.close();
      std::filesystem::remove(marker, ec);
    }
    std::cout << "plugin contracts complete\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
