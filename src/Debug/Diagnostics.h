#pragma once
#include "../Runtime/Stream.h"
#include "../Runtime/EventLoop.h"
#include <source_location>
#include <string>

namespace ii::debug {
enum class Level { trace, info, warning, error };
struct Record final { Level level=Level::info; runtime::Clock::time_point time{}; std::string message; std::source_location location{}; };
// Instance-owned bounded MPMC diagnostics. Reporting never writes a global log.
class Diagnostics final {
public:
    explicit Diagnostics(std::size_t capacity=256) : records_(capacity,runtime::OverflowPolicy::drop_oldest) {}
    [[nodiscard]] bool write(Level level,std::string message,std::source_location location=std::source_location::current()) {
        return records_.try_push(Record{level,runtime::Clock::now(),std::move(message),location});
    }
    [[nodiscard]] bool try_pop(Record& record) { return records_.try_pop(record); }
    [[nodiscard]] runtime::StreamStats stats() const { return records_.stats(); }
    void close() { records_.close(); }
private:
    runtime::Stream<Record> records_;
};
} // namespace ii::debug
