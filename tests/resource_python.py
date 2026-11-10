"""End-to-end Python access to the same native resource owners as C++."""
import importlib.util
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
module_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "src/python/iisdkcore_resources.py"
spec = importlib.util.spec_from_file_location("iisdkcore_resources", module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

sdk = module.Library(sys.argv[1])
with sdk.runtime(workers=0, capacity=4) as dormant:
    assert dormant.workers == 0 and dormant.allocated_slots == 0
    dormant.resize(1)
    assert dormant.workers == 1 and dormant.allocated_slots == 4
    dormant.resize(0)
    assert dormant.workers == 0 and dormant.allocated_slots == 0
with sdk.runtime(workers=1, capacity=4) as runtime:
    assert runtime.workers == 1
    runtime.resize(3)
    assert runtime.workers == 3
    runtime.resize(0)
    assert runtime.workers == 0
    runtime.resize(1)

with sdk.pool(128) as source, sdk.pool(128) as target:
    with source.allocate(16) as block:
        assert block.read() == bytes(16)
        block.write(b"abcdef", 3)
        assert block.read(6, 3) == b"abcdef"
        block.transfer(target)
        assert source.stats.used == 0 and target.stats.used == 16
        block.resize(24)
        assert block.read(6, 3) == b"abcdef" and block.read(8, 16) == bytes(8)
        try:
            target.limit = 23
            raise AssertionError("live memory allowed an undersized limit")
        except module.ResourceError as error:
            assert error.code == module.LIMIT_EXCEEDED
    assert target.stats.used == 0
    target.limit = 0
    try:
        target.allocate(1)
        raise AssertionError("zero budget allowed allocation")
    except module.ResourceError as error:
        assert error.code == module.LIMIT_EXCEEDED

pool = sdk.pool(8)
block = pool.allocate(8)
block.write(b"lifetime")
pool.close()
assert block.read() == b"lifetime"
block.close()
block.close()
print("RESOURCE_PYTHON_OK")
