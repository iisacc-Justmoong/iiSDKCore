#include <array>
#include <iiSDKCore/Animation/Timeline.h>
#include <iiSDKCore/Core/Event/Event.h>
#include <iiSDKCore/Core/IO/BinaryCursor.h>
#include <iiSDKCore/Core/Memory/SharedValue.h>
#include <iostream>
#include <vector>
int main() {
  ii::core::SharedValue<std::vector<int>> value(std::vector{1});
  auto snapshot = value.snapshot();
  if (!value.update([](auto &x) { x.push_back(2); }) ||
      snapshot.get().size() != 1)
    return 1;
  auto failed = value.update([](auto &x) {
    x.clear();
    throw std::runtime_error("edit failure");
  });
  if (failed || value.snapshot().get().size() != 2)
    return 5;
  ii::animation::Keyframes<double> curve(
      {{std::chrono::milliseconds(0), 0}, {std::chrono::milliseconds(10), 1}});
  if (curve.sample(std::chrono::milliseconds(5)) != 0.5)
    return 2;
  ii::core::EventSequence events;
  if (events.make(42).payload != 42)
    return 3;
  std::array<std::byte, 4> bytes{};
  ii::core::ByteWriter writer(bytes);
  if (!writer.write<std::uint32_t>(42) ||
      ii::core::ByteReader(bytes).read<std::uint32_t>() != 42)
    return 4;
  std::cout << "independent value modules complete\n";
}
