#include <iiSDKCore/Core/Value/Value.h>
#include <iiSDKCore/Runtime/Execution.h>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace ii::core;
namespace {
int checks = 0;
void check(bool valid) { ++checks; if (!valid) throw std::runtime_error("foundation contract " + std::to_string(checks)); }
template<class F> void invalid(F operation) {
    try { operation(); } catch (const std::invalid_argument &) { ++checks; return; }
    throw std::runtime_error("expected invalid structured data");
}
struct Executor final {
    ii::runtime::Submission post(ii::runtime::Task work, ii::runtime::Priority) { work(); return {}; }
};
struct InvalidExecutor final { bool post(ii::runtime::Task, ii::runtime::Priority); };
static_assert(ii::runtime::TaskExecutor<Executor>);
static_assert(!ii::runtime::TaskExecutor<InvalidExecutor>);
static_assert(!std::is_convertible_v<int, Value> && !std::is_convertible_v<const char *, Value>);
static_assert(!std::is_constructible_v<Value, void *> && !std::is_constructible_v<Value, long double>);
template<class T> concept TemporaryView = requires(T value) { std::move(value).template get<std::int64_t>(); };
static_assert(!TemporaryView<Value>);
}
int main() {
    try {
        for (std::string_view text : {"", "ASCII", "한글", "\xf0\x9f\x8c\x8d"}) check(valid_utf8(text));
        for (std::string_view text : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) check(!valid_utf8(text));
        check(valid_utf8(std::string_view("a\0b", 3)));
        Value none;
        check(none.kind() == ValueKind::null && bool(none.get<std::monostate>()));
        Value object(Value::Object{{"name", Value("한글")}, {"optional", Value()},
            {"count", Value(std::numeric_limits<std::uint64_t>::max())}, {"enabled", Value(false)},
            {"values", Value(Value::Array{Value(-1), Value(1.5)})}});
        check(object.find("name")->get().get<std::string>()->get() == "한글");
        check(object.find("optional")->get().kind() == ValueKind::null);
        check(!object.find("missing") && object.find("missing").error().code == ErrorCode::not_found);
        check(object.find("count")->get().get<std::uint64_t>()->get() == std::numeric_limits<std::uint64_t>::max());
        check(!object.find("count")->get().get<double>());
        const Value boolean(false), integer(1);
        check(!boolean.get<std::int64_t>() && !integer.get<bool>());
        const auto &array = object.find("values")->get();
        check(array.at(0)->get().get<std::int64_t>()->get() == -1);
        check(array.at(1)->get().get<double>()->get() == 1.5);
        check(!array.at(2) && array.at(2).error().code == ErrorCode::out_of_range);
        check(!none.find("x") && !none.at(0));
        auto copy = object; object = Value(); check(copy.kind() == ValueKind::object && object.kind() == ValueKind::null);
        Value bytes(Bytes{std::byte{0xff}, std::byte{0}});
        check(bytes.kind() == ValueKind::bytes && bytes.get<Bytes>()->get().size() == 2 && !bytes.get<std::string>());
        check(byte_view("abc").size() == 3);
        check(Value(1) != Value(1U) && Value(1) != Value(1.0));
        invalid([] { (void)Value(std::numeric_limits<double>::infinity()); });
        invalid([] { (void)Value(std::numeric_limits<double>::quiet_NaN()); });
        invalid([] { (void)Value("\xc0\xaf"); });
        invalid([] { (void)Value(static_cast<const char *>(nullptr)); });
        invalid([] { (void)Value(Value::Object{{"\xff", Value()}}); });
        int called = 0; Executor executor;
        check(bool(executor.post([&] { ++called; }, ii::runtime::Priority::normal)) && called == 1);
        const ii::runtime::Submission empty;
        check(bool(empty) && !empty.token.pending() && !empty.token.cancel());
        std::cout << checks << " independent foundation checks passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
