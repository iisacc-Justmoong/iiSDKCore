#include <cstdio>
#include <iiSDKCore/Embedded/Embedded.h>
using namespace ii::embedded;
struct State final {
    EventLoop<4> *loop;
    unsigned count = 0;
    Ticket recurring{};
};
static void increment(void *p) noexcept { ++static_cast<State *>(p)->count; }
static void cancel_self(void *p) noexcept {
    auto &s = *static_cast<State *>(p);
    ++s.count;
    s.loop->cancel(s.recurring);
}
int main() {
    EventLoop<4> loop;
    State s{&loop};
    auto first = loop.post({increment, &s});
    if (!first || loop.poll(0) != 1 || s.count != 1)
        return 1;
    if (loop.cancel(first))
        return 2;
    auto timed = loop.schedule(0xfffffff0u, 32, {increment, &s});
    if (!timed || loop.poll(0x0fu) || loop.poll(0x10u) != 1)
        return 3;
    s.recurring = loop.every(100, 10, {cancel_self, &s});
    if (!s.recurring || loop.poll(110) != 1 || loop.poll(120))
        return 4;
    for (unsigned i = 0; i < 4; ++i)
        if (!loop.post({increment, &s}))
            return 5;
    if (loop.post({increment, &s}) || loop.poll(130, 2) != 2 || loop.poll(130, 2) != 2)
        return 6;
    if (loop.schedule(0, 0x80000000u, {increment, &s}) || loop.every(0, 0, {increment, &s}))
        return 7;
    if (contract_version() != 1 || s.count != 7)
        return 8;
    const auto stale = loop.post({increment, &s});
    loop.clear();
    const auto replacement = loop.post({increment, &s});
    EventLoop<4> foreign;
    if (!replacement || loop.cancel(stale) || foreign.cancel(replacement) || !loop.cancel(replacement))
        return 9;
    const auto periodic = loop.every(1000, 5, {increment, &s});
    if (!periodic || loop.poll(1040) != 1 || loop.poll(1044) || loop.poll(1045) != 1 || s.count != 9)
        return 10;
    loop.clear();
    struct Nested final {
        EventLoop<4> *loop;
        bool blocked = false;
    };
    Nested nested{&loop};
    loop.post({[](void *value) noexcept {
                   auto &n = *static_cast<Nested *>(value);
                   n.blocked = n.loop->poll(0) == 0;
               },
               &nested});
    if (loop.poll(0) != 1 || !nested.blocked || loop.pending())
        return 11;
    std::puts("Embedded bounded callbacks, deadlines, wraparound, cancellation "
              "and budget passed");
}
