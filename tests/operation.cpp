#include <iiSDKCore/Runtime/Operation.h>
#include <iostream>
#include <latch>
#include <stdexcept>
using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
int checks=0;
void check(bool value) { ++checks; if (!value) throw std::runtime_error("operation check "+std::to_string(checks)); }
auto work=[](WorkContext&) -> ii::core::Result<int> { return 42; };
}
int main() {
    EventLoop loop;
    OperationOwner owner;
    auto operation=owner.submit<int>(loop,work);
    check(operation.admission()==SubmitError::none && !operation.result());
    check(operation.wait_for(0ms)==OperationWait::timed_out && operation.status()==OperationStatus::queued);
    loop.poll();
    check(operation.result() && operation.result()->status==OperationStatus::succeeded && operation.result()->value==42);
    check(!operation.cancel() && operation.wait_for(0ms)==OperationWait::ready);
    auto cancelled=owner.submit<int>(loop,work);
    check(cancelled.cancel() && !cancelled.cancel());
    check(cancelled.result() && cancelled.result()->status==OperationStatus::cancelled && !cancelled.result()->value);
    auto deadline=owner.submit<int>(loop,work,{ii::core::Deadline::after(0ms)});
    loop.poll(); check(deadline.result()->status==OperationStatus::timed_out && deadline.result()->effects==SideEffects::none);
    {
        EventLoop bounded(1);
        check(bool(bounded.post([]{})));
        auto rejected=owner.submit<int>(bounded,work);
        check(rejected.admission()==SubmitError::full && rejected.result()->status==OperationStatus::rejected);
    }
    {
        EventLoop stopped;
        auto abandoned=owner.submit<int>(stopped,work);
        stopped.request_stop(); check(abandoned.result()->status==OperationStatus::executor_stopped);
    }
    {
        OperationOwner removed;
        const auto capture=std::make_shared<int>(1);
        auto pending=removed.submit<int>(loop,[capture](WorkContext&) -> ii::core::Result<int>{return *capture;});
        check(bool(removed.close())); check(pending.result()->status==OperationStatus::owner_removed);
        auto rejected=removed.submit<int>(loop,work);
        check(rejected.admission()==SubmitError::stopped && rejected.result()->status==OperationStatus::rejected);
    }
    auto failed=owner.submit<int>(loop,[](WorkContext&) -> ii::core::Result<int>{return ii::core::fail(ii::core::ErrorCode::not_found,"fixture");});
    auto throwing=owner.submit<int>(loop,[](WorkContext&) -> ii::core::Result<int>{throw std::runtime_error("fixture");});
    loop.poll();
    check(failed.result()->status==OperationStatus::failed && failed.result()->error->code==ii::core::ErrorCode::not_found);
    check(throwing.result()->status==OperationStatus::failed && bool(throwing.result()->error->cause));
    Runtime runtime({2,32});
    {
        std::latch started(1),release(1);
        auto running=owner.submit<int>(runtime,[&](WorkContext& context)->ii::core::Result<int> {
            context.mark_effects(SideEffects::possible); started.count_down(); release.wait();
            auto ready=context.checkpoint(); if (!ready) return std::unexpected(ready.error()); return 1;
        });
        started.wait(); check(running.cancel() && !running.result()); release.count_down();
        check(running.wait_for(5s)==OperationWait::ready);
        check(running.result()->status==OperationStatus::cancelled && running.result()->effects==SideEffects::possible);
    }
    {
        std::latch committed(1),release(1);
        auto running=owner.submit<int>(runtime,[&](WorkContext& context)->ii::core::Result<int> {
            context.mark_effects(SideEffects::committed); committed.count_down(); release.wait(); return 7;
        });
        committed.wait(); running.cancel(); release.count_down();
        check(running.wait_for(5s)==OperationWait::ready && running.result()->status==OperationStatus::succeeded);
        check(running.result()->value==7 && running.result()->effects==SideEffects::committed);
    }
    {
        OperationOwner removed;
        std::latch started(1);
        auto running=removed.submit<int>(runtime,[&](WorkContext& context)->ii::core::Result<int> {
            started.count_down();
            while (!context.owner_stop_token().stop_requested()) std::this_thread::yield();
            return std::unexpected(context.checkpoint().error());
        });
        started.wait(); check(bool(removed.close())); check(running.result()->status==OperationStatus::owner_removed);
    }
    {
        SerialQueue reply(runtime); TaskScope receiver; int callbacks=0;
        auto complete=owner.submit<int>(loop,work);
        check(complete.then(reply,receiver.sender(),[&](auto result) { check(reply.is_current() && result->value==42); ++callbacks; }));
        check(!complete.then(reply,receiver.sender(),[](auto){}));
        loop.poll(); check(bool(runtime.wait_idle()));
        check(callbacks==1 && complete.completion_submission()==SubmitError::none);
    }
    {
        auto receiver=std::make_unique<TaskScope>(); auto sender=receiver->sender(); int callbacks=0;
        auto complete=owner.submit<int>(loop,work);
        check(complete.then(loop,sender,[&](auto){++callbacks;}));
        receiver.reset(); loop.poll();
        check(callbacks==0 && complete.completion_submission()==SubmitError::stopped);
        check(sender.post(loop,[]{}).error==SubmitError::stopped);
    }
    {
        EventLoop reply; auto receiver=std::make_unique<TaskScope>(); int callbacks=0;
        auto complete=owner.submit<int>(loop,work);
        check(complete.then(reply,receiver->sender(),[&](auto){++callbacks;}));
        loop.poll(); check(complete.completion_submission()==SubmitError::none);
        receiver.reset(); reply.poll(); check(callbacks==0);
    }
    {
        EventLoop fixed; Thread thread; check(bool(thread.start(fixed)));
        auto first=owner.submit<Thread::Id>(fixed,[](WorkContext&)->ii::core::Result<Thread::Id>{return Thread::current_id();});
        auto second=owner.submit<Thread::Id>(fixed,[](WorkContext&)->ii::core::Result<Thread::Id>{return Thread::current_id();});
        check(first.wait_for(5s)==OperationWait::ready && second.wait_for(5s)==OperationWait::ready);
        check(first.result()->value==second.result()->value && first.result()->value==thread.id());
        thread.request_stop(); check(bool(thread.join()));
    }
    {
        auto never=owner.submit<int>(loop,work);
        auto waiter=owner.submit<int>(runtime,[&](WorkContext&)->ii::core::Result<int> {
            return never.wait_for(1s)==OperationWait::would_block ? 1 : 0;
        });
        check(waiter.wait_for(5s)==OperationWait::ready && waiter.result()->value==1); never.cancel();
    }
    // Completion/cancellation race: one immutable outcome and one delivery.
    for (int iteration=0;iteration<100;++iteration) {
        EventLoop reply; TaskScope receiver; int callbacks=0;
        auto concurrent=owner.submit<int>(runtime,work);
        check(concurrent.then(reply,receiver.sender(),[&](auto){++callbacks;}));
        concurrent.cancel(); check(bool(runtime.wait_idle())); reply.poll();
        check(concurrent.result() && callbacks==1 && concurrent.result()==concurrent.result());
    }
    check(bool(owner.close()) && bool(runtime.join()));
    std::cout << checks << " tracked result, cancellation, lifetime and execution context checks passed\n";
}
