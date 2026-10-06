#include <iiSDKCore/Runtime/EventLoop.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace ii::io;
    using namespace ii::runtime;
    try {
        struct Files final {
            std::string stem="loop-io-example-"+std::to_string(Clock::now().time_since_epoch().count());
            std::filesystem::path input=stem+"-input.bin", output=stem+"-output.bin";
            ~Files() { std::error_code ignored; std::filesystem::remove(input,ignored); std::filesystem::remove(output,ignored); }
        } files;
        auto source=open_file(files.input,{.access=Access::read_write,.creation=Creation::create_new}).value();
        auto destination=open_file(files.output,{.access=Access::write,.creation=Creation::create_new}).value();
        const std::array<std::byte,5> bytes{std::byte{0},std::byte{42},std::byte{128},std::byte{255},std::byte{7}};
        if (source.write(bytes)!=bytes.size() || source.seek(0)!=0) throw std::runtime_error("prepare input");
        EventLoop loop(8);
        Buffer buffer(bytes.size());
        std::size_t input=0,output=0;
        auto reading=loop.read(source,Read{buffer},[&](IOResult reply) {
            if (!reply) throw std::runtime_error("loop input failed");
            input=reply->transfer.bytes;
            auto writing=loop.write(destination,Write{reply->buffer,0,input},[&](IOResult sent) {
                if (!sent) throw std::runtime_error("loop output failed");
                output=sent->transfer.bytes;
                loop.request_stop();
            });
            if (!writing) throw std::runtime_error("output admission failed");
        });
        if (!reading) throw std::runtime_error("input admission failed");
        // A native worker can use run(); host-driven GUI/WASM code uses poll().
        while (loop.state()!=LoopState::stopped) loop.poll();
        if (!source.close() || !destination.close()) throw std::runtime_error("channel close failed");
        auto result=open_file(files.output).value();
        std::array<std::byte,5> stored{};
        auto transfer=result.read(stored);
        if (!transfer || transfer->bytes!=bytes.size() || stored!=bytes) throw std::runtime_error("output verification failed");
        std::cout << "loop I/O complete: input=" << input << " output=" << output << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
