"""Bounded CPU/device chunk scheduling through iiSDKCore's C ABI.

Callbacks can run on native threads. A device result's wait() must return only
when the submitted operation and its resources have actually retired.
"""
import ctypes
import math
import threading
from collections import namedtuple
from iisdkcore_resources import BUSY, CLOSED, UNSUPPORTED, ResourceError, _size

_PHASES = ('queued', 'running', 'pausing', 'paused', 'cancelling', 'completed', 'cancelled', 'failed')
_TERMINAL = frozenset(('completed', 'cancelled', 'failed'))
Snapshot = namedtuple('Snapshot', 'phase revision total_items claimed_items completed_items completed_chunks device_submitted cpu_active device_active device_in_flight interrupts_active')

class _Lane(ctypes.Structure):
    _fields_ = [('group_size', ctypes.c_uint32), ('max_groups', ctypes.c_uint32)]

class _NativeSnapshot(ctypes.Structure):
    _fields_ = [('phase', ctypes.c_int)] + [(name, ctypes.c_uint64) for name in
        ('revision', 'total_items', 'claimed_items', 'completed_items', 'completed_chunks', 'device_submitted')] + [
        (name, ctypes.c_size_t) for name in ('cpu_active', 'device_active', 'device_in_flight', 'interrupts_active')]
    def value(self):
        return Snapshot(_PHASES[self.phase], *(getattr(self, name) for name, _ in self._fields_[1:]))

_Cpu = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32)
_Submit = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64,
                           ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_size_t))
_Wait = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
_Release = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_size_t)
_Interrupt = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t)

class _NativeCallbacks(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p) for name in ('context', 'cpu', 'submit', 'wait', 'collect', 'release', 'interrupt')]

class _Callbacks:
    def __init__(self, cpu, device, interrupt):
        self.errors = []
        self.pending = {}
        self.lock = threading.Lock()
        self.next_token = 0
        self.functions = []
        self.native = _NativeCallbacks()
        def wrap(function_type, function, default=1):
            def invoke(*args):
                try:
                    function(*args)
                    return 0 if function_type is not _Release else None
                except BaseException as error:
                    with self.lock:
                        self.errors.append(error)
                    return default if function_type is not _Release else None
            callback = function_type(invoke)
            self.functions.append(callback)
            return ctypes.cast(callback, ctypes.c_void_p).value
        if cpu is not None:
            self.native.cpu = wrap(_Cpu, lambda _, offset, count: cpu(offset, count))
        if device is not None:
            def submit(_, lane, offset, count, group_size, groups, out):
                operation = device(lane, offset, count, group_size, groups)
                if operation is None or not callable(getattr(operation, 'wait', None)):
                    raise TypeError('device callback must return an object with wait()')
                with self.lock:
                    self.next_token += 1
                    token = self.next_token
                    self.pending[token] = operation
                out[0] = token
            def invoke_operation(token, name):
                with self.lock:
                    operation = self.pending[token]
                method = getattr(operation, name, None)
                if method is not None:
                    method()
            def release(_, token):
                with self.lock:
                    self.pending.pop(token, None)
            self.native.submit = wrap(_Submit, submit)
            self.native.wait = wrap(_Wait, lambda _, token: invoke_operation(token, 'wait'))
            self.native.collect = wrap(_Wait, lambda _, token: invoke_operation(token, 'collect'))
            self.native.release = wrap(_Release, release)
        if interrupt is not None:
            self.native.interrupt = wrap(_Interrupt, lambda _, kind, data, size: interrupt(kind, ctypes.string_at(data, size)))


def _functions(dll):
    ptr, size, u64 = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64
    specs = {
        'queue_create': ([size, size, ctypes.POINTER(_Lane), size, ctypes.POINTER(ptr)], ctypes.c_int),
        'queue_destroy': ([ptr], ctypes.c_int),
        'submit_work': ([ptr, u64, ctypes.c_uint32, ctypes.POINTER(_NativeCallbacks), ctypes.POINTER(ptr)], ctypes.c_int),
        'task_destroy': ([ptr], None),
        'task_snapshot': ([ptr, ctypes.POINTER(_NativeSnapshot)], ctypes.c_int),
        'task_wait': ([ptr, u64, u64, ctypes.POINTER(_NativeSnapshot)], ctypes.c_int),
        'task_pause': ([ptr], ctypes.c_int), 'task_resume': ([ptr], ctypes.c_int),
        'task_cancel': ([ptr], ctypes.c_int),
        'task_interrupt': ([ptr, u64, ptr, size], ctypes.c_int),
    }
    result = {}
    for name, (args, restype) in specs.items():
        try:
            function = getattr(dll, 'iisdkcore_chunk_' + name)
        except AttributeError as error:
            raise ResourceError(UNSUPPORTED) from error
        function.argtypes, function.restype = args, restype
        result[name] = function
    return result

def _call(function, *args):
    code = function(*args)
    if code:
        raise ResourceError(code)

def _u32(value):
    if not isinstance(value, int) or value < 0 or value > 0xffffffff:
        raise ValueError('value must fit uint32')
    return value

def _u64(value):
    if not isinstance(value, int) or value < 0 or value > 0xffffffffffffffff:
        raise ValueError('value must fit uint64')
    return value

class ChunkTicket:
    def __init__(self, library, functions, handle, callbacks):
        self._library, self._functions, self._handle, self._callbacks = library, functions, handle, callbacks
    @property
    def errors(self):
        with self._callbacks.lock:
            return tuple(self._callbacks.errors)
    def snapshot(self):
        out = _NativeSnapshot()
        _call(self._functions['task_snapshot'], self._handle, ctypes.byref(out))
        return out.value()
    def wait_for_change(self, revision, timeout):
        if not math.isfinite(timeout) or timeout < 0:
            raise ValueError('timeout must be finite and nonnegative')
        out = _NativeSnapshot()
        _call(self._functions['task_wait'], self._handle, _u64(revision), _u64(math.ceil(timeout * 1000)), ctypes.byref(out))
        return out.value()
    def pause(self):
        _call(self._functions['task_pause'], self._handle)
    def resume(self):
        _call(self._functions['task_resume'], self._handle)
    def cancel(self):
        _call(self._functions['task_cancel'], self._handle)
    def interrupt(self, kind, data=b''):
        value = bytes(data)
        storage = ctypes.create_string_buffer(value) if value else None
        _call(self._functions['task_interrupt'], self._handle, _u64(kind), storage, len(value))
    def __del__(self):
        handle = getattr(self, '_handle', None)
        if handle and handle.value:
            self._handle = None
            self._functions['task_destroy'](handle)

class ChunkQueue:
    def __init__(self, library, *, cpu_workers=1, capacity=64, devices=()):
        self._library, self._functions = library, _functions(library._dll)
        self._condition = threading.Condition()
        self._active_calls = 0
        self._closing = False
        self._tickets = []
        lanes = tuple((_u32(group), _u32(groups)) for group, groups in devices)
        array = (_Lane * len(lanes))(*(_Lane(*lane) for lane in lanes))
        self._handle = ctypes.c_void_p()
        _call(self._functions['queue_create'], _size(cpu_workers), _size(capacity), array, len(lanes), ctypes.byref(self._handle))
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
    def reap(self):
        with self._condition:
            self._tickets[:] = [ticket for ticket in self._tickets if ticket.snapshot().phase not in _TERMINAL]
    def submit(self, *, items, chunk_items, cpu=None, device=None, interrupt=None):
        if any(value is not None and not callable(value) for value in (cpu, device, interrupt)):
            raise TypeError('chunk callbacks must be callable')
        handle = self._enter()
        try:
            self.reap()
            callbacks = _Callbacks(cpu, device, interrupt)
            out = ctypes.c_void_p()
            _call(self._functions['submit_work'], handle, _u64(items), _u32(chunk_items), ctypes.byref(callbacks.native), ctypes.byref(out))
            ticket = ChunkTicket(self._library, self._functions, out, callbacks)
            with self._condition:
                self._tickets.append(ticket)
            return ticket
        finally:
            self._leave()
    def close(self):
        with self._condition:
            if not self._handle:
                return
            if self._closing:
                raise ResourceError(BUSY)
            self._closing = True
            while self._active_calls:
                self._condition.wait()
            handle = self._handle
        try:
            _call(self._functions['queue_destroy'], handle)
        except BaseException:
            with self._condition:
                self._closing = False
            raise
        with self._condition:
            self._handle = None
            self._tickets.clear()
    def __enter__(self):
        return self
    def __exit__(self, *_):
        self.close()
    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
