#include <iiSDKCore/library.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <iiSDKCore/Runtime/Thread.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/IO/Channel.h>
#include <array>

int main()
{
    hello();
    ii::runtime::DynamicValue dynamic = std::string("42");
    if (ii::runtime::value_cast<int>(dynamic) != 42) return 9;
    ii::runtime::EventLoop loop(4);
    ii::runtime::Stream<int> stream(2);
    int received = 0;
    if (!loop.post([&] { (void)stream.try_push(42); })) return 1;
    if (loop.poll() != 1 || !stream.try_pop(received) || received != 42) return 2;
    ii::runtime::QueryPort<int, int> receiver([](const int& value) { return value * 2; });
    if (receiver.send(21).value() != 42) return 3;
    ii::runtime::ObjectProperties properties({{.name = "answer", .value = 42}});
    auto answer = properties.queries().send(ii::runtime::PropertyRead{{{.name = "answer"}}});
    if (!answer || !*answer || std::any_cast<int>((**answer).fields[0].value) != 42) return 4;
    ii::runtime::Thread worker;
    if (worker.joinable() || worker.state() != ii::runtime::ThreadState::idle || !worker.join()) return 5;
#if !defined(__EMSCRIPTEN__)
    auto source_file = ii::io::open_file(__FILE__);
    std::array<std::byte, 8> io_bytes{};
    if (!source_file) return 10;
    auto io_read = source_file->read(io_bytes);
    if (!io_read || io_read->bytes != io_bytes.size() || !source_file->close()) return 10;
    int threaded_value = 0;
    if (!worker.start([&](ii::runtime::ThreadContext&) { threaded_value = 7; }) ||
        !worker.join() || threaded_value != 7) return 6;
    ii::runtime::Runtime runtime;
    ii::runtime::SerialQueue queue(runtime);
    ii::runtime::QueryPort<int, int> async(queue, [](const int& value) { return value + 1; });
    int answer_async = 0;
    if (!async.post(41, [&](ii::runtime::QueryReply<int> result) { answer_async = result.value(); }) ||
        !runtime.wait_idle() || answer_async != 42 || !runtime.join()) return 8;
#endif
    return loop.stats().executed == 1 ? 0 : 7;
}
