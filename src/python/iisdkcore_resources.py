"""Python access to iiSDKCore's native CPU workers and owned host memory.

Pass the explicit path to the matching shared iiSDKCore library to Library.
The library and each owner must remain open while it is being called.
"""
import ctypes
from collections import namedtuple

LIMIT_EXCEEDED = 2
CLOSED = 4
Stats = namedtuple("Stats", "limit used peak allocations")


class ResourceError(RuntimeError):
    def __init__(self, code):
        self.code = code
        super().__init__(f"iiSDKCore resource error: {code}")


def _size(value):
    if not isinstance(value, int) or value < 0 or value > ctypes.c_size_t(-1).value:
        raise ValueError("size and offset must fit size_t")
    return value


class Library:
    def __init__(self, path):
        self._dll = ctypes.CDLL(str(path))
        ptr, size = ctypes.c_void_p, ctypes.c_size_t
        ptr_out, size_out = ctypes.POINTER(ptr), ctypes.POINTER(size)
        definitions = {
            "pool_create": ([size, ptr_out], ctypes.c_int),
            "pool_destroy": ([ptr], None),
            "pool_limit": ([ptr, size], ctypes.c_int),
            "pool_stats": ([ptr, size_out, size_out, size_out, size_out], ctypes.c_int),
            "block_create": ([ptr, size, size, ptr_out], ctypes.c_int),
            "block_destroy": ([ptr], None),
            "block_size": ([ptr, size_out], ctypes.c_int),
            "block_read": ([ptr, size, ptr, size], ctypes.c_int),
            "block_write": ([ptr, size, ptr, size], ctypes.c_int),
            "block_resize": ([ptr, size], ctypes.c_int),
            "block_transfer": ([ptr, ptr], ctypes.c_int),
            "runtime_create": ([size, size, ptr_out], ctypes.c_int),
            "runtime_destroy": ([ptr], None),
            "runtime_resize": ([ptr, size], ctypes.c_int),
            "runtime_workers": ([ptr, size_out], ctypes.c_int),
            "runtime_allocated_slots": ([ptr, size_out], ctypes.c_int),
        }
        self._functions = {}
        for name, (arguments, result) in definitions.items():
            function = getattr(self._dll, "iisdkcore_resource_" + name)
            function.argtypes = arguments
            function.restype = result
            self._functions[name] = function

    def _call(self, name, *arguments):
        code = self._functions[name](*arguments)
        if code:
            raise ResourceError(code)

    def pool(self, limit):
        handle = ctypes.c_void_p()
        self._call("pool_create", _size(limit), ctypes.byref(handle))
        return Pool(self, handle)

    def runtime(self, workers, capacity=1024):
        handle = ctypes.c_void_p()
        self._call("runtime_create", _size(workers), _size(capacity), ctypes.byref(handle))
        return Runtime(self, handle)


class _Owner:
    def __init__(self, library, handle):
        self._library, self._handle = library, handle

    def _get(self):
        if not self._handle or not self._handle.value:
            raise ResourceError(CLOSED)
        return self._handle

    def close(self):
        handle = getattr(self, "_handle", None)
        if handle and handle.value:
            self._handle = None
            self._library._functions[self._destroy](handle)

    def __enter__(self):
        self._get()
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class Pool(_Owner):
    _destroy = "pool_destroy"

    @property
    def stats(self):
        values = [ctypes.c_size_t() for _ in range(4)]
        self._library._call("pool_stats", self._get(), *(ctypes.byref(value) for value in values))
        return Stats(*(value.value for value in values))

    @property
    def limit(self):
        return self.stats.limit

    @limit.setter
    def limit(self, value):
        self._library._call("pool_limit", self._get(), _size(value))

    def allocate(self, size, alignment=0):
        handle = ctypes.c_void_p()
        self._library._call("block_create", self._get(), _size(size), _size(alignment), ctypes.byref(handle))
        return Block(self._library, handle)


class Block(_Owner):
    _destroy = "block_destroy"

    @property
    def size(self):
        result = ctypes.c_size_t()
        self._library._call("block_size", self._get(), ctypes.byref(result))
        return result.value

    def read(self, size=None, offset=0):
        offset = _size(offset)
        available = self.size
        if offset > available:
            raise ValueError("offset exceeds block")
        size = available - offset if size is None else _size(size)
        if size > available - offset:
            raise ValueError("read exceeds block")
        data = ctypes.create_string_buffer(size)
        self._library._call("block_read", self._get(), offset, data, size)
        return data.raw

    def write(self, data, offset=0):
        data = bytes(data)
        offset = _size(offset)
        if offset > self.size or len(data) > self.size - offset:
            raise ValueError("write exceeds block")
        buffer = ctypes.create_string_buffer(data, len(data))
        self._library._call("block_write", self._get(), offset, buffer, len(data))

    def resize(self, size):
        self._library._call("block_resize", self._get(), _size(size))

    def transfer(self, destination):
        if not isinstance(destination, Pool) or destination._library is not self._library:
            raise ValueError("destination must belong to the same library")
        self._library._call("block_transfer", self._get(), destination._get())


class Runtime(_Owner):
    _destroy = "runtime_destroy"

    @property
    def workers(self):
        result = ctypes.c_size_t()
        self._library._call("runtime_workers", self._get(), ctypes.byref(result))
        return result.value

    @property
    def allocated_slots(self):
        result = ctypes.c_size_t()
        self._library._call("runtime_allocated_slots", self._get(), ctypes.byref(result))
        return result.value

    def resize(self, workers):
        self._library._call("runtime_resize", self._get(), _size(workers))
