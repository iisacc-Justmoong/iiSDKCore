import os
import signal
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'python'))
from iisdkcore_resources import CLOSED, UNSUPPORTED, Library, ResourceError


def main():
    library = Library(sys.argv[1])
    with library.start_process([sys.argv[2], 'sleep']) as child:
        assert child.id > 0
        exits = []
        waiter = threading.Thread(target=lambda: exits.append(child.wait(5)))
        waiter.start()
        if os.name == 'posix':
            child.send_signal(signal.SIGSTOP)
            child.send_signal(signal.SIGCONT)
            child.send_signal(signal.SIGTERM, group=True)
            waiter.join(5)
            assert not waiter.is_alive()
            assert exits[0] is not None and exits[0].reason == 'signaled' and exits[0].signal == signal.SIGTERM
            try:
                child.send_signal(signal.SIGTERM)
                raise AssertionError('completed child accepted signal')
            except ResourceError as error:
                assert error.code == CLOSED
        else:
            try:
                child.send_signal(1)
                raise AssertionError('POSIX signal accepted on Windows')
            except ResourceError as error:
                assert error.code == UNSUPPORTED
            child.cancel()
            waiter.join(5)
            assert not waiter.is_alive() and exits[0].reason == 'cancelled'
    print('Python owned process tests passed')


if __name__ == '__main__':
    main()
