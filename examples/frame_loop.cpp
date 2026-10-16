#include <iiSDKCore/Runtime/FrameLoop.h>
#if defined(__EMSCRIPTEN__)
#include <iiSDKCore/Runtime/BrowserLoop.h>
#endif
#include <iostream>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;

// App UI uses real time; simulation uses fixed scaled time. Host/renderer
// integration is supplied through callbacks rather than an engine hierarchy.
struct AppAndGame final {
  EventLoop events;
  FrameLoop frames{events, {.fixed_step = 5ms, .frame_interval = 4ms}};
#if defined(__EMSCRIPTEN__)
  BrowserLoop browser{frames};
#endif
  double previous = 0, position = 0, displayed = 0;
  unsigned ui_frames = 0;

  AppAndGame() {
    auto before = frames.add([this](const FrameContext &) { previous = position; },
        {.phase = FramePhase::before_fixed});
    auto simulate = frames.add([this](const FrameContext &time) {
      position += std::chrono::duration<double>(time.delta).count() * 2.;
    }, {.phase = FramePhase::fixed_update});
    auto ui = frames.add([this](const FrameContext &) { ++ui_frames; },
        {.phase = FramePhase::update, .clock = TickClock::unscaled});
    auto present = frames.add([this](const FrameContext &time) {
      displayed = previous + (position - previous) * time.interpolation;
    }, {.phase = FramePhase::present, .clock = TickClock::unscaled});
    auto end = frames.add([this](const FrameContext &time) {
      if (time.frame == 8) {
        std::cout << "app/game frame loop complete: frames=" << time.frame
                  << " UI=" << ui_frames << " fixed=" << time.fixed_tick
                  << " interpolated_position=" << displayed << '\n';
        events.request_stop();
      }
    }, {.phase = FramePhase::end, .clock = TickClock::unscaled});
    if (!before || !simulate || !ui || !present || !end)
      throw std::runtime_error("Cannot register app/game frame callbacks");
  }
};

int main() {
  static AppAndGame app;
#if defined(__EMSCRIPTEN__)
  app.browser.start();
#else
  app.frames.run();
  if (app.ui_frames != 8 || app.frames.stats().fixed_steps == 0) return 1;
#endif
}
