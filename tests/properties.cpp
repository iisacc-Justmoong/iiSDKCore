#include <iiSDKCore/Runtime/ObjectProperties.h>

#include <iostream>
#include <stdexcept>

using namespace ii::runtime;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

PropertyPolicy editable() { return {.queryable = true, .designable = true, .writable = true}; }
PropertyDefinition level(int value = 1) {
    return {.name = "level", .value = value, .policy = editable(),
            .validate = [](const std::any& input) {
                const auto v = std::any_cast<int>(input);
                return v >= 0 && v <= 100;
            }};
}
std::vector<PropertyDefinition> schema() {
    return {{.name = "audio", .policy = editable(), .children = {
        {.name = "output", .policy = editable(), .children = {level()}}}}};
}
PropertyRead read_level() {
    return {{{.name = "audio", .children = {{.name = "output", .children = {{.name = "level"}}}}}}};
}
PropertyResult external(ObjectProperties& object, PropertyQuery query) {
    auto reply = object.queries().send(query);
    check(bool(reply), "query transport succeeded");
    return std::move(*reply);
}
int value(ObjectProperties& object) {
    auto result = external(object, read_level());
    check(bool(result), "read selected tree");
    return std::any_cast<int>(result->fields.at(0).children.at(0).children.at(0).value);
}
void fails(const PropertyResult& result, PropertyErrorCode code, const char* message) {
    check(!result && result.error().code == code, message);
}
struct CountedValue {
    int& copies;
    explicit CountedValue(int& count) : copies(count) {}
    CountedValue(const CountedValue& other) : copies(other.copies) { ++copies; }
};

int main() {
    try {
        std::vector<PropertyChange> changes;
        ObjectProperties first(schema(), editable(), [&](const PropertyChange& change) {
            changes.push_back(change);
        });
        ObjectProperties second(schema(), editable());
        check(value(first) == 1 && first.revision() == 0, "creation-time properties");
        check(external(first, PropertyRead{})->fields.empty(), "empty selection stays empty");
        check(bool(external(first, PropertyWrite{{"audio", "output", "level"}, 42})), "write value");
        check(value(first) == 42 && value(second) == 1, "instance values are independent");
        check(changes.size() == 1 && changes[0].revision == 1 &&
              changes[0].path == PropertyPath({"audio", "output", "level"}), "change query after commit");
        fails(external(first, PropertyWrite{{"audio", "output", "level"}, 101}),
              PropertyErrorCode::validation_failed, "validation rejects invalid value");
        fails(external(first, PropertyWrite{{"audio", "output", "level"}, std::string("42")}),
              PropertyErrorCode::type_mismatch, "no implicit conversion");
        check(value(first) == 42 && changes.size() == 1, "failed writes have no effects");

        auto descriptor = external(first, PropertyDescribe{{"audio", "output", "level"}});
        check(descriptor && descriptor->descriptor->value_type == typeid(int) &&
              descriptor->descriptor->policy.designable, "design metadata");
        check(bool(external(first, PropertyDefine{{"audio", "output"},
              {.name = "gain", .value = 0.5, .policy = editable()}})), "extend a live instance");
        fails(external(second, PropertyDescribe{{"audio", "output", "gain"}}),
              PropertyErrorCode::not_found, "extension does not change other instances");
        check(bool(external(first, PropertyDefine{{"audio", "output"},
              {.name = "level", .value = std::string("automatic"), .policy = editable()}, true})),
              "replace schema and value type");
        auto old_snapshot = external(first, read_level());
        check(bool(external(first, PropertyErase{{"audio", "output", "level"}})), "remove live property");
        check(std::any_cast<std::string>(old_snapshot->fields[0].children[0].children[0].value) == "automatic",
              "snapshots survive removal");
        check(bool(first.apply(PropertyDefine{{"audio", "output"}, level(7)})), "owner can re-add property");

        // A node can hold a value, behavior and children at once.
        PropertyDefinition action{.name = "adjust", .value = std::string("v1"), .policy = editable(),
            .children = {{.name = "unit", .value = std::string("percent"), .policy = editable()}},
            .method = [&](const std::any& input) -> std::any {
                auto result = first.apply(PropertyWrite{{"audio", "output", "level"}, input});
                if (!result) throw std::runtime_error("method write failed");
                return value(first);
            }};
        check(bool(first.apply(PropertyDefine{{}, action})), "install instance method");
        auto invoked = external(first, PropertyInvoke{{"adjust"}, 9});
        check(invoked && std::any_cast<int>(invoked->returned) == 9 && value(first) == 9,
              "method may update its instance through owner queries");
        action.method = [](const std::any&) -> std::any { return 123; };
        check(bool(first.apply(PropertyDefine{{}, action, true})), "replace method on an existing instance");
        check(std::any_cast<int>(external(first, PropertyInvoke{{"adjust"}, {}})->returned) == 123,
              "new behavior executes");

        check(bool(first.apply(PropertyDefine{{}, {.name = "counter", .method =
            [count = 0](const std::any&) mutable -> std::any { return ++count; }}})), "stateful method");
        check(std::any_cast<int>(external(first, PropertyInvoke{{"counter"}, {}})->returned) == 1 &&
              std::any_cast<int>(external(first, PropertyInvoke{{"counter"}, {}})->returned) == 2,
              "method instance state persists between calls");
        check(bool(first.apply(PropertyDefine{{}, {.name = "once", .method =
            [&](const std::any&) -> std::any {
                check(bool(first.apply(PropertyErase{{"once"}})), "self removal");
                return 17;
            }}})), "self removing method");
        check(std::any_cast<int>(external(first, PropertyInvoke{{"once"}, {}})->returned) == 17,
              "in-flight method survives its own removal");
        fails(external(first, PropertyInvoke{{"once"}, {}}), PropertyErrorCode::not_found,
              "removed method cannot execute again");

        PropertyDefinition secret{.name = "private", .policy = {.visibility = PropertyVisibility::private_member,
            .queryable = true, .designable = true}, .children = {level(5)}};
        check(bool(first.apply(PropertyDefine{{"audio"}, secret})), "owner extends private tree");
        fails(external(first, PropertyRead{{{.name = "audio", .children = {
              {.name = "private", .children = {{.name = "level"}}}}}}}),
              PropertyErrorCode::access_denied, "private ancestor protects descendants");
        fails(external(first, PropertyErase{{"audio"}}), PropertyErrorCode::access_denied,
              "cannot erase private descendants via public ancestor");
        fails(external(first, PropertyDefine{{}, schema()[0], true}), PropertyErrorCode::access_denied,
              "cannot replace protected descendants via public ancestor");
        auto audio_descriptor = external(first, PropertyDescribe{{"audio"}});
        check(audio_descriptor && audio_descriptor->descriptor->children == std::vector<std::string>{"output"},
              "private child names are not disclosed");
        check(bool(first.apply(PropertyWrite{{"audio", "private", "level"}, 6})), "owner can change private value");
        auto owner_read = first.apply(PropertyRead{{{.name = "audio", .children = {
              {.name = "private", .children = {{.name = "level"}}}}}}});
        check(owner_read && std::any_cast<int>(owner_read->fields[0].children[0].children[0].value) == 6,
              "owner queries preserve private trees");

        ObjectProperties locked({{.name = "readOnly", .value = 1},
            {.name = "designOnly", .value = 2, .policy = {.queryable = false, .designable = true}},
            {.name = "branch", .policy = editable(), .children = {{.name = "locked", .value = 3}}}}, editable());
        fails(external(locked, PropertyWrite{{"readOnly"}, 2}), PropertyErrorCode::not_writable, "read-only value");
        fails(external(locked, PropertyDescribe{{"readOnly"}}), PropertyErrorCode::access_denied, "not designable");
        check(bool(external(locked, PropertyDescribe{{"designOnly"}})), "designability is independent of queryability");
        fails(external(locked, PropertyRead{{{.name = "designOnly"}}}), PropertyErrorCode::access_denied,
              "designable does not grant read permission");
        fails(external(locked, PropertyErase{{"branch"}}), PropertyErrorCode::access_denied,
              "locked descendants survive removal of an editable parent");
        fails(external(locked, PropertyDefine{{}, {.name = "readOnly", .value = 9}, true}),
              PropertyErrorCode::access_denied, "replacement cannot bypass designability");

        const auto revision = first.revision();
        fails(first.apply(PropertyDefine{{}, {.name = "bad", .children = {level(), level()}}}),
              PropertyErrorCode::invalid_definition, "duplicate sibling names rejected atomically");
        fails(first.apply(PropertyDefine{{}, level(101)}), PropertyErrorCode::validation_failed,
              "initial values are validated");
        check(first.revision() == revision, "failed definitions preserve revision");
        fails(first.apply(PropertyDefine{{}, {.name = ""}}), PropertyErrorCode::invalid_definition,
              "empty member names rejected");
        fails(first.apply(PropertyDefine{{}, action}), PropertyErrorCode::already_exists,
              "replacement requires explicit opt-in");
        fails(first.apply(PropertyErase{{}}), PropertyErrorCode::invalid_definition, "root cannot be removed");
        fails(first.apply(PropertyInvoke{{"audio"}, {}}), PropertyErrorCode::not_callable, "group is not a method");

        bool rejected_reentry = false;
        auto validated = level();
        validated.name = "validated";
        validated.validate = [&](const std::any&) {
            auto nested = first.apply(PropertyErase{{"audio"}});
            rejected_reentry = !nested && nested.error().code == PropertyErrorCode::reentrant;
            return true;
        };
        check(bool(first.apply(PropertyDefine{{}, validated})) && rejected_reentry,
              "validator cannot invalidate an in-progress edit");
        ObjectProperties observer_failure({}, editable(), [](const PropertyChange&) { throw std::runtime_error("observer"); });
        auto committed = observer_failure.apply(PropertyDefine{{}, level()});
        check(committed && committed->notification_error && observer_failure.revision() == 1,
              "notification failure does not roll back an applied edit");

        auto throwing = level();
        throwing.name = "throwing";
        throwing.validate = [](const std::any&) -> bool { throw std::logic_error("validator failure"); };
        auto validation_exception = first.apply(PropertyDefine{{}, throwing});
        check(!validation_exception && validation_exception.error().cause &&
              validation_exception.error().code == PropertyErrorCode::handler_failed, "validator exception preserved");
        check(bool(first.apply(PropertyDefine{{}, {.name = "fail", .method =
            [](const std::any&) -> std::any { throw std::logic_error("method failure"); }}})), "throwing method");
        auto invocation_exception = external(first, PropertyInvoke{{"fail"}, {}});
        check(!invocation_exception && invocation_exception.error().cause &&
              invocation_exception.error().code == PropertyErrorCode::handler_failed, "method exception preserved");
        check(bool(first.apply(PropertyDefine{{}, {.name = "explicitObject", .value = std::cref(second)}})),
              "whole object reference is opt-in");
        auto reference = external(first, PropertyRead{{{.name = "explicitObject"}}});
        check(&std::any_cast<std::reference_wrapper<const ObjectProperties>>(reference->fields[0].value).get() == &second,
              "noncopyable object can be explicitly referenced");
        auto omitted_value = external(first, PropertyRead{{{.name = "explicitObject", .include_value = false}}});
        check(!omitted_value->fields[0].value.has_value(), "node selection need not select its value");

        bool rejected_initial = false;
        try { ObjectProperties invalid({level(), level()}); }
        catch (const std::invalid_argument&) { rejected_initial = true; }
        check(rejected_initial, "invalid creation-time schema rejected");

        int copies = 0;
        ObjectProperties counted({{.name = "heavy", .value = CountedValue{copies}, .policy = editable()}}, editable());
        copies = 0;
        check(bool(external(counted, PropertyRead{})) &&
              bool(external(counted, PropertyDescribe{{"heavy"}})) &&
              bool(external(counted, PropertyRead{{{.name = "heavy", .include_value = false}}})) && copies == 0,
              "unselected values are not copied even for metadata inspection");
        check(bool(external(counted, PropertyRead{{{.name = "heavy"}}})) && copies > 0,
              "selected value produces an owned snapshot");

        bool observed_after_commit = false;
        ObjectProperties notifying(schema(), editable(), [&](const PropertyChange& change) {
            observed_after_commit = value(notifying) == 25 && notifying.revision() == change.revision;
        });
        check(bool(notifying.apply(PropertyWrite{{"audio", "output", "level"}, 25})) && observed_after_commit,
              "notification queries can read committed instance state");

        EventLoop loop;
        bool delivered = false;
        auto pending = first.queries().post(loop, PropertyWrite{{"audio", "output", "level"}, 11},
            [&](QueryReply<PropertyResult> reply) { delivered = reply && *reply; });
        check(bool(pending) && value(first) == 9, "queued edit is deferred");
        loop.poll();
        check(delivered && value(first) == 11, "queued edit uses existing runtime");
        bool stale_write_rejected = false;
        (void)first.queries().post(loop, PropertyWrite{{"audio", "output", "level"}, 12},
            [&](QueryReply<PropertyResult> reply) {
                stale_write_rejected = reply && !*reply && reply->error().code == PropertyErrorCode::type_mismatch;
            });
        check(bool(first.apply(PropertyDefine{{"audio", "output"},
              {.name = "level", .value = std::string("new schema"), .policy = editable()}, true})), "change before delivery");
        loop.poll();
        check(stale_write_rejected, "queued queries validate against the schema at delivery");
        bool expired = false;
        {
            ObjectProperties temporary(schema());
            (void)temporary.queries().post(loop, read_level(), [&](QueryReply<PropertyResult> reply) {
                expired = !reply && reply.error().code == QueryErrorCode::receiver_expired;
            });
        }
        loop.poll();
        check(expired, "destroyed instance expires queued property queries");
        std::cout << "dynamic property contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
