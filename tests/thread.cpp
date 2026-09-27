#include <iiSDKCore/Runtime/Thread.h>
#include <iiSDKCore/Runtime/EventLoop.h>

#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        Thread worker;
        check(worker.state() == ThreadState::idle && !worker.joinable(), "initial state");
        check(bool(worker.join()) && !worker.request_stop(), "idle join and stop are harmless");
        if (!Thread::supported()) {
            bool called = false;
            auto result = worker.start([&](ThreadContext&) { called = true; });
            check(!result && result.error().code == ThreadErrorCode::unsupported && !called &&
                  worker.state() == ThreadState::idle, "unsupported hosts never run inline");
            std::cout << "thread unavailable contract passed on " << platform_name() << '\n';
            return 0;
        }
        auto invalid = worker.start(Thread::Work{});
        check(!invalid && invalid.error().code == ThreadErrorCode::invalid_work, "empty work rejected");
        int value = 0;
        Thread::Id executed;
        check(bool(worker.start([&](ThreadContext&) {
            executed = Thread::current_id();
            value = 42;
        })), "start native worker");
        check(bool(worker.join()) && value == 42 && executed != Thread::current_id() &&
              executed == worker.id() && worker.state() == ThreadState::completed, "join publishes results");
        check(bool(worker.join()), "join is idempotent");

        std::atomic<bool> entered = false;
        bool stopped = false;
        check(bool(worker.start([&](ThreadContext& context) {
            entered.store(true);
            stopped = context.wait_for(30s) && context.stop_requested();
        })), "restart joined instance");
        auto duplicate = worker.start([](ThreadContext&) {});
        check(!duplicate && duplicate.error().code == ThreadErrorCode::busy, "unjoined run cannot be overwritten");
        const auto before = Clock::now();
        check(worker.request_stop() && !worker.request_stop(), "stop is idempotent");
        check(bool(worker.join()) && stopped && Clock::now() - before < 5s, "stop wakes cooperative wait");
        check(entered.load(), "a stop requested before entry reaches the worker context");

        check(bool(worker.start([](ThreadContext&) { throw std::logic_error("worker failure"); })), "throwing work starts");
        auto failed = worker.join();
        check(!failed && failed.error().code == ThreadErrorCode::work_failed && failed.error().cause &&
              worker.state() == ThreadState::failed, "worker exception is retained");
        bool original = false;
        try { std::rethrow_exception(failed.error().cause); }
        catch (const std::logic_error& error) { original = std::string(error.what()) == "worker failure"; }
        check(original && !worker.join(), "join can inspect the same failure again");
        check(bool(worker.start([](ThreadContext& context) {
            if (context.stop_requested() || context.wait_for(1ms)) throw std::runtime_error("stale stop state");
        })) && bool(worker.join()), "new run has fresh stop and failure state");

        bool self_join_rejected = false;
        check(bool(worker.start([&](ThreadContext&) {
            auto result = worker.join();
            self_join_rejected = !result && result.error().code == ThreadErrorCode::self_join;
        })) && bool(worker.join()) && self_join_rejected, "self join never deadlocks");

        bool destruction_stopped = false;
        {
            Thread owned;
            check(bool(owned.start([&](ThreadContext& context) {
                destruction_stopped = context.wait_for(30s);
            })), "RAII worker starts");
        }
        check(destruction_stopped, "destruction requests stop and joins");

        EventLoop loop(8);
        Thread::Id dispatcher;
        check(bool(loop.post([&] { dispatcher = Thread::current_id(); loop.request_stop(); })), "post loop work");
        check(bool(worker.start(loop)) && bool(worker.join()) && dispatcher == worker.id(), "managed loop dispatch");
        EventLoop idle;
        check(bool(worker.start(idle)), "idle loop starts");
        check(worker.request_stop() && bool(worker.join()) && idle.state() == LoopState::stopped,
              "thread stop wakes an idle event loop");
        EventLoop suspended;
        suspended.suspend();
        check(bool(worker.start(suspended)), "suspended loop starts");
        worker.request_stop();
        check(bool(worker.join()), "thread stop wakes a suspended event loop");

        EventLoop broken;
        check(bool(broken.post([] { throw std::runtime_error("loop failure"); })), "post throwing callback");
        check(bool(worker.start(broken)), "throwing loop starts");
        check(!worker.join() && worker.state() == ThreadState::failed, "loop failure reaches thread owner");

        // All API queries are safe while another thread is joining the worker.
        std::atomic<bool> joining = false;
        check(bool(worker.start([](ThreadContext& context) { (void)context.wait_for(30s); })), "concurrent test worker");
        Thread joiner;
        bool joined = false;
        check(bool(joiner.start([&](ThreadContext&) { joining = true; joined = bool(worker.join()); })), "joiner starts");
        while (!joining.load()) std::this_thread::yield();
        check(worker.joinable(), "joinable during join");
        (void)worker.state();
        (void)worker.id();
        worker.request_stop();
        check(bool(joiner.join()) && joined, "concurrent stop and join complete");
        std::cout << "thread contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
