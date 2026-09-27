#pragma once
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ii::core {
struct Violation { std::string_view message; std::source_location location; };
// Per-instance reporting. An empty reporter throws; never disabled by NDEBUG.
class Contract final {
public:
    using Reporter=std::function<void(const Violation&)>;
    explicit Contract(Reporter reporter={}) : reporter_(std::move(reporter)) {}
    bool check(bool condition, std::string_view message,
               std::source_location location=std::source_location::current()) const {
        if (condition) return true;
        if (reporter_) reporter_(Violation{message,location});
        else throw std::logic_error(std::string(message));
        return false;
    }
private:
    Reporter reporter_;
};
} // namespace ii::core
