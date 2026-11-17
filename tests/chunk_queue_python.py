import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'python'))
from iisdkcore_resources import Library, ResourceError, UNSUPPORTED


def expect(value, message):
    if not value:
        raise AssertionError(message)


def phase(task, desired):
    deadline = time.monotonic() + 5
    state = task.snapshot()
    while state.phase != desired and time.monotonic() < deadline:
        state = task.wait_for_change(state.revision, 0.2)
    expect(state.phase == desired, f'{desired} not reached: {state.phase}')
    return state


def main():
    lib = Library(sys.argv[1])
    seen = []
    lock = threading.Lock()
    def cpu(offset, count):
        with lock:
            seen.extend(range(offset, offset + count))
    with lib.chunk_queue(cpu_workers=3, capacity=8) as queue:
        try:
            queue.submit(items=1, chunk_items=1, cpu=42)
            raise AssertionError('noncallable CPU callback was accepted')
        except TypeError:
            pass
        try:
            queue.submit(items=1, chunk_items=1, device=lambda *_: None)
            raise AssertionError('device work without a device lane was accepted')
        except ResourceError as error:
            expect(error.code == UNSUPPORTED, 'unsupported was reported as a temporary block')
        tasks = [queue.submit(items=23, chunk_items=4, cpu=cpu) for _ in range(4)]
        for task in tasks:
            expect(phase(task, 'completed').completed_items == 23, 'CPU chunks lost work')
        expect(len(seen) == 92 and all(seen.count(i) == 4 for i in range(23)), 'CPU coverage incorrect')
        def fail_cpu(offset, count):
            raise ValueError('expected callback failure')
        failed = queue.submit(items=1, chunk_items=1, cpu=fail_cpu)
        phase(failed, 'failed')
        expect(len(failed.errors) == 1 and isinstance(failed.errors[0], ValueError), 'Python failure was lost')

    entered, release = threading.Event(), threading.Event()
    geometry, collected, events = [], [], []
    class DeviceResult:
        def wait(self):
            entered.set()
            if not release.wait(5):
                raise RuntimeError('completion gate timed out')
        def collect(self):
            collected.append(True)
    def device(index, offset, count, group_size, groups):
        geometry.append((index, offset, count, group_size, groups))
        return DeviceResult()
    with lib.chunk_queue(cpu_workers=0, capacity=2, devices=[(4, 2)]) as queue:
        task = queue.submit(items=14, chunk_items=10, device=device,
                            interrupt=lambda kind, data: events.append((kind, data)))
        try:
            expect(entered.wait(5), 'device wait was not entered')
            expect(task.snapshot().device_submitted == 1, 'submission not visible')
            task.interrupt(17, b'abc')
            expect(events == [(17, b'abc')], 'interrupt payload not delivered')
            task.pause()
            expect(task.snapshot().phase == 'pausing', 'pause reported early')
        finally:
            release.set()
        phase(task, 'paused')
        task.resume()
        expect(phase(task, 'completed').completed_items == 14, 'device completion lost work')
        expect(geometry == [(0, 0, 8, 4, 2), (0, 8, 6, 4, 2)] and len(collected) == 2,
               'device dispatch geometry incorrect')
    entered, release = threading.Event(), threading.Event()
    collected.clear()
    with lib.chunk_queue(cpu_workers=0, devices=[(4, 2)]) as queue:
        task = queue.submit(items=16, chunk_items=8, device=device)
        try:
            expect(entered.wait(5), 'cancel test did not submit')
            task.cancel()
            expect(task.snapshot().phase == 'cancelling', 'device cancel reported early')
        finally:
            release.set()
        expect(phase(task, 'cancelled').completed_items == 0 and not collected,
               'cancelled submission collected or completed early')
    old = object.__new__(Library)
    old._dll = object()
    try:
        old.chunk_queue()
        raise AssertionError('old library was accepted')
    except ResourceError as error:
        expect(error.code == UNSUPPORTED, 'old library did not report unsupported')
    print('Python chunk queue tests passed')


if __name__ == '__main__':
    main()
