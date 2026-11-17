"""Own and signal an iiSDKCore child process without transferring its PID."""
import ctypes
import math
import os
import threading
from collections import namedtuple
from iisdkcore_resources import BUSY, CLOSED, UNSUPPORTED, ResourceError

Exit = namedtuple('Exit', 'reason code signal')
_REASONS = ('exited', 'signaled', 'cancelled', 'timed_out', 'owner_removed')

class _Exit(ctypes.Structure):
    _fields_ = [('ready', ctypes.c_int), ('reason', ctypes.c_int), ('code', ctypes.c_uint32), ('signal', ctypes.c_int)]
    def value(self):
        return Exit(_REASONS[self.reason], self.code, self.signal) if self.ready else None


def _functions(dll):
    ptr = ctypes.c_void_p
    specs = {
        'start': ([ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ptr)], ctypes.c_int),
        'destroy': ([ptr], None), 'id': ([ptr, ctypes.POINTER(ctypes.c_uint64)], ctypes.c_int),
        'signal': ([ptr, ctypes.c_int, ctypes.c_int], ctypes.c_int),
        'wait': ([ptr, ctypes.c_uint64, ctypes.POINTER(_Exit)], ctypes.c_int),
        'cancel': ([ptr, ctypes.POINTER(_Exit)], ctypes.c_int),
    }
    result = {}
    for name, (args, restype) in specs.items():
        try:
            function = getattr(dll, 'iisdkcore_process_' + name)
        except AttributeError as error:
            raise ResourceError(UNSUPPORTED) from error
        function.argtypes, function.restype = args, restype
        result[name] = function
    return result


def _call(function, *args):
    code = function(*args)
    if code:
        raise ResourceError(code)

class Process:
    """Owns a child. stdin is closed; stdout and stderr are discarded."""
    def __init__(self, library, arguments):
        self._library, self._functions = library, _functions(library._dll)
        self._condition = threading.Condition()
        self._closing = False
        self._active_calls = 0
        self._handle = ctypes.c_void_p()
        if isinstance(arguments, (str, bytes, os.PathLike)):
            raise ValueError('arguments must be a sequence, not a command string')
        argv = tuple(os.fspath(value) for value in arguments)
        if not argv or any(not isinstance(value, str) or '\0' in value for value in argv):
            raise ValueError('arguments must be a nonempty sequence of NUL-free strings')
        encoded = tuple(value.encode('utf-8') for value in argv)
        values = (ctypes.c_char_p * len(encoded))(*encoded)
        _call(self._functions['start'], len(encoded), values, ctypes.byref(self._handle))
        native_id = ctypes.c_uint64()
        _call(self._functions['id'], self._handle, ctypes.byref(native_id))
        self.id = native_id.value
    def _enter(self):
        with self._condition:
            if self._closing or not self._handle:
                raise ResourceError(CLOSED)
            self._active_calls += 1
            return self._handle
    def _leave(self):
        with self._condition:
            self._active_calls -= 1
            self._condition.notify_all()
    def send_signal(self, number, *, group=False):
        if not isinstance(number, int) or not -(2**31) <= number < 2**31:
            raise ValueError('native signal must fit an int')
        handle = self._enter()
        try:
            _call(self._functions['signal'], handle, number, int(bool(group)))
        finally:
            self._leave()
    def wait(self, timeout):
        if not math.isfinite(timeout) or timeout < 0 or timeout * 1000 > 2**63 - 1:
            raise ValueError('timeout must be a finite nonnegative duration')
        handle = self._enter()
        try:
            result = _Exit()
            _call(self._functions['wait'], handle, math.ceil(timeout * 1000), ctypes.byref(result))
            return result.value()
        finally:
            self._leave()
    def cancel(self):
        handle = self._enter()
        try:
            result = _Exit()
            _call(self._functions['cancel'], handle, ctypes.byref(result))
            return result.value()
        finally:
            self._leave()
    def close(self):
        with self._condition:
            if not self._handle:
                return
            if self._closing:
                raise ResourceError(BUSY)
            self._closing = True
            handle = self._handle
        try:
            # Wake concurrent waiters before waiting for their borrowed handle.
            result = _Exit()
            _call(self._functions['cancel'], handle, ctypes.byref(result))
            with self._condition:
                while self._active_calls:
                    self._condition.wait()
            self._functions['destroy'](handle)
        except BaseException:
            with self._condition:
                self._closing = False
            raise
        with self._condition:
            self._handle = None
    def __enter__(self):
        return self
    def __exit__(self, *_):
        self.close()
    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
