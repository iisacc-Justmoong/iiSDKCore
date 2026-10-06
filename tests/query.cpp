#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Runtime/Thread.h>

#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <thread>
#endif

using namespace ii::runtime;
using namespace std::chrono_literals;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct SetValue { int value; };
struct ReadValue {};

struct Counter final {
    int value = 0;
    QueryPort<SetValue> set{[this](const SetValue& query) { value = query.value; }};
    QueryPort<ReadValue, int> read{[this](const ReadValue&) { return value; }};
};

void selected_fields_contract() {
    struct Object final {
        int level = 0;
        bool enabled = true;
        std::string label = "sensor";
        int label_reads = 0;
        Object() = default;
        Object(const Object&) = delete;
    } object;
    struct ReadFields { bool level = false, label = false, object = false; };
    struct Fields {
        std::optional<int> level;
        std::optional<std::string> label;
        std::optional<std::reference_wrapper<const Object>> object;
    };
    QueryPort<ReadFields, Fields> read([&](const ReadFields& query) {
        Fields response;
        if (query.level) response.level = object.level;
        if (query.label) { ++object.label_reads; response.label = object.label; }
        if (query.object) response.object = std::cref(object);
        return response;
    });
    auto empty = read.send({});
    check(empty && !empty->level && !empty->label && !empty->object,
          "an empty field selection does not implicitly expand to the object");
    auto scalar = read.send({.level = true});
    check(scalar && scalar->level == 0 && !scalar->label && !scalar->object,
          "selected zero value is present while unselected fields remain absent");
    check(object.label_reads == 0, "unselected fields are not evaluated or copied");

    EventLoop loop;
    ReadFields selection{.label = true};
    bool selected_reply = false;
    auto queued = read.post(loop, selection, [&](QueryReply<Fields> reply) {
        selected_reply = reply && reply->label == "sensor" && !reply->level && !reply->object;
    });
    selection = {.level = true, .object = true};
    check(bool(queued), "field selection accepted");
    loop.poll({8, 1s});
    check(selected_reply && object.label_reads == 1, "queued query preserves its explicit selection");
    auto whole = read.send({.object = true});
    check(whole && whole->object && &whole->object->get() == &object && !whole->level && !whole->label,
          "whole noncopyable object can be explicitly requested by reference");

    struct Patch { std::optional<int> level; std::optional<bool> enabled; };
    QueryPort<Patch> update([&](const Patch& query) {
        if (query.level.has_value()) object.level = *query.level;
        if (query.enabled.has_value()) object.enabled = *query.enabled;
    });
    check(update.send({.level = 7}).has_value() && object.level == 7 && object.enabled,
          "partial update preserves omitted fields");
    check(update.send({.level = 0, .enabled = false}).has_value() && object.level == 0 && !object.enabled,
          "explicit zero and false updates are not treated as omitted");
}

int main() {
    try {
        selected_fields_contract();
        Counter receiver;
        check(receiver.set.send({7}).has_value(), "void query completion");
        check(receiver.read.send({}).value() == 7, "typed synchronous reply");

        EventLoop loop(4);
        bool delivered = false;
        SetValue query{42};
        auto submission = receiver.set.post(loop, query, [&](QueryReply<void> reply) {
            check(reply.has_value(), "queued void reply");
            delivered = true;
        });
        query.value = 99;
        check(bool(submission) && !delivered && receiver.value == 7, "delivery is queued");
        loop.poll({8, 1s});
        check(delivered && receiver.value == 42, "queued request owns its payload");

        int reply_value = 0;
        (void)receiver.read.post(loop, {}, [&](QueryReply<int> reply) { reply_value = reply.value(); });
        loop.poll({8, 1s});
        check(reply_value == 42, "queued typed response");

        bool expired = false;
        {
            QueryPort<ReadValue, int> temporary([](const ReadValue&) { return 10; });
            (void)temporary.post(loop, {}, [&](QueryReply<int> reply) {
                expired = true;
                (void)reply;
            });
        }
        loop.poll({8, 1s});
        check(!expired && loop.stats().pending==0, "removed receiver releases queued queries without late completions");

        QueryPort<ReadValue, int> failing([](const ReadValue&) -> int {
            throw std::runtime_error("receiver error");
        });
        auto failure = failing.send({});
        check(!failure && failure.error().code == QueryErrorCode::handler_failed,
              "receiver exception becomes query failure");
        bool original_error = false;
        try { std::rethrow_exception(failure.error().cause); }
        catch (const std::runtime_error& error) { original_error = std::string(error.what()) == "receiver error"; }
        check(original_error, "original receiver exception preserved");
        bool queued_failure = false;
        (void)failing.post(loop, {}, [&](QueryReply<int> reply) { queued_failure = !reply; });
        loop.poll({8, 1s});
        check(queued_failure, "queued handler failure delivered to completion");

        bool cancelled_reply = false;
        auto cancelled = receiver.set.post(loop, {100}, [&](QueryReply<void>) { cancelled_reply = true; });
        check(loop.cancel(cancelled.token), "queued query supports cancellation");
        loop.poll({8, 1s});
        check(!cancelled_reply && receiver.value == 42, "cancel suppresses delivery and completion");

        EventLoop full(1);
        (void)full.post([] {});
        check(receiver.read.post(full, {}, [](QueryReply<int>) {}).error == SubmitError::full,
              "query admission exposes backpressure");
        full.request_stop();
        check(receiver.read.post(full, {}, [](QueryReply<int>) {}).error == SubmitError::stopped,
              "query admission exposes stopped loop");
        check(receiver.read.post(loop, {}, {}).error == SubmitError::invalid,
              "missing completion is rejected");

        bool empty_rejected = false;
        try { QueryPort<ReadValue, int> empty({}); }
        catch (const std::invalid_argument&) { empty_rejected = true; }
        check(empty_rejected, "empty handler is rejected");

        (void)receiver.read.post(loop, {}, [](QueryReply<int>) { throw std::runtime_error("completion error"); });
        bool completion_error = false;
        try { loop.poll({8, 1s}); } catch (const std::runtime_error&) { completion_error = true; }
        check(completion_error && loop.stats().pending == 0,
              "completion exception follows EventLoop cleanup contract");

        QueryPort<ReadValue, std::unique_ptr<int>> owning_reply(
            [](const ReadValue&) { return std::make_unique<int>(5); });
        check(*owning_reply.send({}).value() == 5, "move-only response ownership");
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        EventLoop threaded;
        QueryPort<ReadValue, std::thread::id> identity(
            [](const ReadValue&) { return std::this_thread::get_id(); });
        const auto sender_thread = std::this_thread::get_id();
        bool correct_thread = false;
        (void)identity.post(threaded, {}, [&](QueryReply<std::thread::id> reply) {
            correct_thread = reply && *reply == std::this_thread::get_id() && *reply != sender_thread;
            threaded.request_stop();
        });
        Thread runner;
        check(bool(runner.start(threaded)) && bool(runner.join()), "managed query dispatch");
        check(correct_thread, "receiver and completion run on the dispatch thread");
#endif
        std::cout << "query contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
