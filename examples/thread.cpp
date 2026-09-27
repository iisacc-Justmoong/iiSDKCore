#include <iiSDKCore/Runtime/Thread.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iostream>

using namespace ii::runtime;

int main() {
    if (!Thread::supported()) {
        std::cout << "thread example: this build has no worker support\n";
        return 0;
    }
    EventLoop loop(8);
    QueryPort<int, int> calculate([](const int& value) { return value * 2; });
    Thread worker;
    int answer = 0;
    bool background = false;
    const auto caller = Thread::current_id();
    if (!calculate.post(loop, 21, [&](QueryReply<int> reply) {
        if (reply) answer = *reply;
        background = Thread::current_id() != caller;
        loop.request_stop();
    })) return 1;
    if (!worker.start(loop)) return 2;
    if (!worker.join() || answer != 42 || !background) return 3;
    std::cout << "managed thread complete: answer=" << answer << " background=1\n";
}
