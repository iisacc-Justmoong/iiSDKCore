#include <iiSDKCore/Runtime/Value.h>
#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ii::runtime;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void fails(const DynamicValue& value, CastErrorCode code) {
    auto result = value_cast<T>(value);
    check(!result && result.error().code == code, "cast error category");
    check(!result && result.error().source == value.type() && result.error().target == typeid(T), "cast type metadata");
}
struct Record { int value; };
PropertyResult query(ObjectProperties& object, PropertyQuery request) {
    return object.queries().send(request).value();
}
int run_tests() {
    DynamicValue value = 42;
    check(value.type() == typeid(int) && value_cast<int>(value) == 42, "exact type");
    value = std::string("43");
    check(value_cast<int>(value) == 43, "dynamic reassignment and text cast");
    auto borrowed = value_ref<std::string>(value);
    check(borrowed && borrowed->get() == "43", "reference access");
    check(!value_ref<int>(value), "reference never converts");
    DynamicValue record = Record{7};
    check(value_cast<Record>(record)->value == 7, "custom DTO exact cast");
    fails<int>(record, CastErrorCode::unsupported);
    fails<int>({}, CastErrorCode::empty);
    check(value_cast<std::nullptr_t>(DynamicValue{nullptr}).has_value(), "explicit null");
    fails<int>(DynamicValue{nullptr}, CastErrorCode::unsupported);
    check(value_cast<double>(DynamicValue{42}) == 42.0, "numeric widening");
    check(value_cast<int>(DynamicValue{42.0}) == 42, "integral floating point");
    fails<int>(DynamicValue{42.5}, CastErrorCode::precision_loss);
    fails<unsigned>(DynamicValue{-1}, CastErrorCode::out_of_range);
    fails<signed char>(DynamicValue{128}, CastErrorCode::out_of_range);
    fails<int>(DynamicValue{std::numeric_limits<double>::infinity()}, CastErrorCode::out_of_range);
    fails<int>(DynamicValue{std::numeric_limits<double>::quiet_NaN()}, CastErrorCode::out_of_range);
    fails<std::uint64_t>(DynamicValue{std::ldexp(1.0, 64)}, CastErrorCode::out_of_range);
    fails<std::int64_t>(DynamicValue{std::ldexp(1.0, 63)}, CastErrorCode::out_of_range);
    check(value_cast<std::int64_t>(DynamicValue{-std::ldexp(1.0, 63)}) == std::numeric_limits<std::int64_t>::min(), "signed lower boundary");
    fails<double>(DynamicValue{std::uint64_t(9007199254740993ULL)}, CastErrorCode::precision_loss);
    check(value_cast<double>(DynamicValue{std::uint64_t(9007199254740992ULL)}) == 9007199254740992.0, "exact integer to float");
    fails<float>(DynamicValue{0.1}, CastErrorCode::precision_loss);
    check(value_cast<float>(DynamicValue{0.5}) == 0.5f, "exact float narrowing");
    check(value_cast<long double>(DynamicValue{42}) == 42.0L, "long double numeric cast");
    fails<long double>(DynamicValue{std::string("42")}, CastErrorCode::unsupported);
    fails<std::string>(DynamicValue{42.0L}, CastErrorCode::unsupported);
    if constexpr (std::numeric_limits<long double>::digits > std::numeric_limits<double>::digits)
        fails<double>(DynamicValue{std::ldexp(1.0L, 53) + 1.0L}, CastErrorCode::precision_loss);
    fails<float>(DynamicValue{std::numeric_limits<double>::max()}, CastErrorCode::out_of_range);
    fails<float>(DynamicValue{std::numeric_limits<double>::denorm_min()}, CastErrorCode::precision_loss);
    check(value_cast<std::uint64_t>(DynamicValue{std::numeric_limits<std::uint64_t>::max()}).value() ==
          std::numeric_limits<std::uint64_t>::max(), "exact uint64 preserves all bits");
    check(value_cast<bool>(DynamicValue{1}) == true && value_cast<int>(DynamicValue{false}) == 0, "boolean numeric");
    fails<bool>(DynamicValue{2}, CastErrorCode::out_of_range);
    check(value_cast<bool>(DynamicValue{std::string("true")}) == true, "boolean text");
    fails<bool>(DynamicValue{std::string("TRUE")}, CastErrorCode::invalid_text);
    for (auto text : {"", " 42", "42 ", "42x", "+42", "1.5"})
        fails<int>(DynamicValue{std::string(text)}, CastErrorCode::invalid_text);
    fails<int>(DynamicValue{std::string("99999999999999999999")}, CastErrorCode::out_of_range);
    check(value_cast<double>(DynamicValue{std::string("1.25e2")}) == 125.0, "floating text");
    for (auto text : {"1e", " 1", "1 ", "0x1p2", "nan", "inf", "+1", "1,5", "1e2x"})
        fails<double>(DynamicValue{std::string(text)}, CastErrorCode::invalid_text);
    fails<double>(DynamicValue{std::string("1e9999")}, CastErrorCode::out_of_range);
    fails<double>(DynamicValue{std::string("1e-9999")}, CastErrorCode::out_of_range);
    check(value_cast<double>(DynamicValue{std::string("0e9999")}) == 0.0, "zero exponent boundary");
    check(value_cast<std::string>(DynamicValue{42}) == "42", "numeric formatting");
    check(value_cast<std::string>(DynamicValue{true}) == "true", "boolean formatting");
    for (double number : {0.1, -0.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()}) {
        auto formatted = value_cast<std::string>(DynamicValue{number}).value();
        auto parsed = value_cast<double>(DynamicValue{formatted}).value();
        check(parsed == number && std::signbit(parsed) == std::signbit(number), "text roundtrip");
    }

    ObjectProperties object({
        {.name="fixed", .value=1, .policy={.writable=true}},
        {.name="dynamic", .value=1, .policy={.writable=true, .typing=PropertyTyping::dynamic}},
        {.name="checked", .value=1, .policy={.writable=true, .typing=PropertyTyping::dynamic},
         .validate=[](const DynamicValue& input) { return input.type() == typeid(int); }},
        {.name="private", .value=1, .policy={.visibility=PropertyVisibility::private_member, .writable=true, .typing=PropertyTyping::dynamic}}
    });
    check(!query(object, PropertyWrite{{"fixed"}, std::string("42")}), "no implicit cast");
    check(bool(query(object, PropertyWrite{{"fixed"}, std::string("42"), true})), "explicit write cast");
    const auto revision = object.revision();
    auto failed = query(object, PropertyWrite{{"fixed"}, std::string("bad"), true});
    check(!failed && failed.error().code == PropertyErrorCode::cast_failed &&
          failed.error().cast_error->code == CastErrorCode::invalid_text && object.revision() == revision, "atomic failed cast");
    auto selected = query(object, PropertyRead{{{.name="fixed", .cast_to=typeid(std::string)}}});
    check(selected && value_cast<std::string>(selected->fields[0].value) == "42" && object.revision() == revision, "read projection casts without mutation");
    check(query(object, PropertyRead{{{.name="fixed"}}})->fields[0].value.type() == typeid(int), "stored type unchanged");
    check(bool(query(object, PropertyWrite{{"dynamic"}, std::string("live")})), "dynamic type change");
    check(object.apply(PropertyDescribe{{"dynamic"}})->descriptor->value_type == typeid(std::string), "dynamic type metadata");
    const auto before_null = object.revision();
    check(!query(object, PropertyWrite{{"dynamic"}, DynamicValue{}}) && object.revision() == before_null, "empty write is not erase");
    check(bool(query(object, PropertyWrite{{"dynamic"}, nullptr})), "dynamic explicit null");
    auto rejected = query(object, PropertyWrite{{"checked"}, std::string("bad")});
    check(!rejected && rejected.error().code == PropertyErrorCode::validation_failed, "dynamic validator preserved");
    check(!query(object, PropertyWrite{{"private"}, std::string("hidden")}), "private access preserved");
    auto omitted = query(object, PropertyRead{{{.name="fixed", .include_value=false, .cast_to=typeid(Record)}}});
    check(omitted && !omitted->fields[0].value.has_value(), "unselected values are not converted");
    auto bad_projection = query(object, PropertyRead{{{.name="fixed", .cast_to=typeid(Record)}}});
    check(!bad_projection && bad_projection.error().path == PropertyPath{"fixed"} &&
          bad_projection.error().cast_error->code == CastErrorCode::unsupported, "projection error path");

    if (Thread::supported()) {
        Runtime runtime;
        SerialQueue queue(runtime);
        QueryPort<DynamicValue, DynamicValue> cast_query(queue, [](const DynamicValue& input) {
            return cast_value(input, typeid(int)).value();
        });
        int cast_answer = 0;
        check(bool(cast_query.post(std::string("42"), [&](const QueryReply<DynamicValue>& result) {
            check(bool(result), "dynamic query conversion");
            cast_answer = value_cast<int>(*result).value();
        })), "dynamic query submission");
        ObjectProperties async(queue, {{.name="value", .value=0, .policy={.writable=true, .typing=PropertyTyping::dynamic}}});
        int completed = 0;
        for (int i=0; i<100; ++i) {
            DynamicValue next = i % 2 ? DynamicValue(std::to_string(i)) : DynamicValue(i);
            check(bool(async.queries().post(PropertyWrite{{"value"}, next}, [&](QueryReply<PropertyResult> result) {
                check(result && *result, "asynchronous dynamic write");
                ++completed;
            })), "asynchronous submission");
        }
        check(bool(runtime.wait_idle()) && cast_answer == 42 && completed == 100 && async.revision() == 100, "serialized dynamic writes");
        check(bool(runtime.join()), "join");
    }
    std::cout << "dynamic value and casting contracts passed\n";
    return 0;
}
int main() {
    try { return run_tests(); }
    catch (const std::exception& error) {
        std::cerr << "dynamic value failure: " << error.what() << '\n';
        return 1;
    }
}
