#include "../examples/objects/Device.h"

#include <concepts>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace ii::runtime;
using namespace ii::examples;
using namespace std::chrono_literals;

template<class T> concept PublicValue = requires(const T& object) {
    { object.audio.output.level } -> std::same_as<const int&>;
    { object.audio.output.limits.maximum } -> std::same_as<const int&>;
};
template<class T> concept PublicMethod = requires(T& object) {
    object.audio.output.reset();
    object.audio.output.set_level.send(SetLevel{1});
};
template<class T> concept PrivateRootValue = requires(T& object) { object.internal_.identity.name; };
template<class T> concept PrivateRootMethod = requires(T& object) { object.internal_.identity.read_name(); };
template<class T> concept PrivateChildValue = requires(T& object) { object.audio.output.state_.level; };
template<class T> concept PrivateChildMethod = requires(T& object) { object.audio.output.state_.set_level(1); };
template<class T> concept PrivateGrandchildMethod = requires(T& object) { object.audio.output.state_.history.record(); };
template<class T> concept PrivateMethod = requires(T& object) { object.audio.output.apply(SetLevel{1}); };
template<class T> concept AssignReadOnlyValue = requires(T& object) { object.audio.output.level = 99; };
template<class T> concept FlattenedMember = requires(T& object) { object.level; object.set_level; };

static_assert(PublicValue<Device> && PublicMethod<Device>);
static_assert(!PrivateRootValue<Device> && !PrivateRootMethod<Device>);
static_assert(!PrivateChildValue<Device> && !PrivateChildMethod<Device>);
static_assert(!PrivateGrandchildMethod<Device> && !AssignReadOnlyValue<Device>);
static_assert(!PrivateMethod<Device> && !PrivateRootValue<const Device>);
static_assert(!FlattenedMember<Device>);
static_assert(!std::is_copy_constructible_v<Device> && !std::is_move_constructible_v<Device>);

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        Device first, second;
        check(first.audio.output.limits.maximum == 100, "public nested value");
        check(first.audio.output.limits.contains(42), "public nested ordinary method");
        check(first.audio.output.set_level.send({42}).has_value(), "nested query method");
        check(first.audio.output.level == 42 && first.audio.output.revision == 1,
              "private tree updates public reference views");
        check(second.audio.output.level == 0 && second.audio.output.revision == 0,
              "member trees belong to instances");

        auto reply = first.read.send({.audio = {.output = {.level = true}}});
        check(reply && reply->audio && reply->audio->output && reply->audio->output->level == 42,
              "query response preserves the member tree");
        check(!reply->audio->output->revision && !reply->name && !reply->object,
              "unselected siblings and the whole object remain absent");
        auto empty = first.read.send({});
        check(empty && !empty->audio && !empty->name && !empty->object,
              "empty selection does not materialize subtrees");
        auto name = first.read.send({.name = true});
        check(name && name->name == "device A" && !name->audio,
              "public query explicitly projects private tree values");

        EventLoop loop;
        bool completed = false;
        (void)first.audio.output.set_level.post(loop, {8}, [&](QueryReply<void> result) {
            completed = bool(result);
        });
        check(first.audio.output.level == 42, "nested queued method remains asynchronous");
        loop.poll({8, 1s});
        check(completed && first.audio.output.level == 8 && first.audio.output.revision == 2,
              "queued method reaches the correct nested receiver");
        first.audio.output.reset();
        check(first.audio.output.level == 0 && first.audio.output.revision == 3,
              "ordinary method uses private nested methods");
        check(!first.audio.output.set_level.send({101}) && first.audio.output.revision == 3,
              "failed nested method preserves state");

        bool expired = false;
        {
            Device temporary;
            (void)temporary.audio.output.set_level.post(loop, {1}, [&](QueryReply<void> result) {
                expired = !result && result.error().code == QueryErrorCode::receiver_expired;
            });
        }
        loop.poll({8, 1s});
        check(expired, "destroying a tree expires its descendant query ports");
        std::cout << "member tree contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
