#include <array>
#include <iiSDKCore/Runtime/LiveStream.h>
#include <iostream>
#include <thread>
using namespace ii::runtime;
using namespace std::chrono_literals;
struct SampleBlock final {
  std::size_t sequence;
  std::array<float, 96> samples;
};
int main() {
  EventLoop loop(16);
  LiveStream<SampleBlock> stream(8, 100ms);
  const auto first = Clock::now() + 5ms;
  for (std::size_t i = 0; i < 8; ++i) {
    SampleBlock block{i, {}};
    block.samples.fill(0.25f);
    if (!stream.push(first + i * 2ms, std::move(block)))
      return 1;
  }
  std::size_t delivered = 0;
  float energy = 0;
  bool ordered = true;
  RealtimeSchedule schedule{1ms, 1ms, 200us, 100us, Clock::now()};
  auto playback =
      stream.start(loop, schedule, [&](TimedFrame<SampleBlock> frame) {
        ordered = ordered && frame.value.sequence == delivered;
        for (float sample : frame.value.samples)
          energy += sample * sample;
        ++delivered;
      });
  if (!playback)
    return 1;
  const auto end = Clock::now() + 3s;
  while (delivered < 8 && Clock::now() < end) {
    loop.poll({64, 2ms});
    std::this_thread::sleep_for(200us);
  }
  const auto statistics = loop.stats();
  if (!stream.close() || delivered != 8 || !ordered || energy != 48 ||
      loop.stats().pending)
    return 1;
  std::cout << "timed PCM blocks=" << delivered << " samples=" << delivered * 96
            << " energy=" << energy << " slices=" << statistics.slices
            << " deadline_misses=" << statistics.deadline_misses << '\n';
}
