#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
#include <iiSDKCore/Runtime/BrowserLoop.h>

#include <iostream>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;

// The same composition represents sensor samples, media frames or game updates.
struct LiveSession final {
    EventLoop loop{32};
    Stream<int> samples{8};
    int produced = 0;
    int consumed = 0;
    TaskToken producer;
    void start() {
        auto source = loop.every(1ms, [this] {
            if (!samples.try_push(produced)) throw std::runtime_error("sample overflow");
            ++produced;
            if (produced == 120) {
                loop.cancel(producer);
                samples.close();
            }
        });
        if (!source) throw std::runtime_error("producer admission failed");
        producer = source.token;
        if (!loop.every(1ms, [this] {
            int sample;
            while (samples.try_pop(sample)) {
                if (sample != consumed) throw std::runtime_error("stream ordering failed");
                ++consumed;
            }
            if (samples.drained()) {
                std::cout << "live runtime complete: produced=" << produced
                          << " consumed=" << consumed
                          << " capacity=" << loop.stats().capacity
                          << " high_watermark=" << loop.stats().high_watermark << '\n';
                loop.request_stop();
            }
        })) throw std::runtime_error("consumer admission failed");
    }
};

int main() {
#if defined(__EMSCRIPTEN__)
    // Browser callbacks outlive main(); the driver and its referenced loop must too.
    static LiveSession session;
    static BrowserLoop browser(session.loop);
    session.start();
    browser.start();
#else
    LiveSession session;
    session.start();
    session.loop.run();
#endif
}
