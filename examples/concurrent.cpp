#include <iiSDKCore/Runtime/Concurrent.h>
#include <iiSDKCore/Runtime/FutureWatcher.h>
#include <iostream>

int main() {
    using namespace ii::runtime;
    if (!Thread::supported()) return 0;
    Runtime workers({.workers = 4, .capacity = 32});
    Concurrent compute(workers);
    EventLoop host;
    auto squares = compute.mapped(std::vector{1, 2, 3, 4}, [](int value) { return value * value; }, {.grain = 1});
    bool observed = false;
    FutureWatcher<int> watcher(squares, host, [&](const FutureSnapshot& snapshot) {
        if (snapshot.status == FutureStatus::succeeded) {
            observed = true;
            std::cout << "completed " << snapshot.result_count << " ordered results\n";
        }
    });
    auto result = squares.get_all(); // Console synchronization, outside worker/host callbacks.
    if (!result || *result != std::vector<int>({1, 4, 9, 16})) return 1;
    for (int value : *result) std::cout << value << ' ';
    std::cout << '\n';
    (void)host.poll();
    if (!observed || !watcher.close()) return 2;
    auto sum = compute.mapped_reduced(std::vector{1, 2, 3, 4},
        [](int value) { return value * value; }, [](int& total, int value) { total += value; }, 0);
    auto value = sum.get();
    if (!value || *value != 30) return 3;
    auto configured = task([](int a, int b) { return a + b; })
        .with_arguments(20, 22).with_priority(Priority::high).spawn(compute);
    auto answer = configured.get();
    if (!answer || *answer != 42 || !compute.close() || !workers.join()) return 4;
    std::cout << "ordered reduction = " << *value << "; configured task = " << *answer << '\n';
}
