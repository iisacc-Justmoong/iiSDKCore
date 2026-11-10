"""Load the installed Python bridge and Core without a library search override."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build, cmake, library_dir, library_name, configuration = sys.argv[1:]
source = Path(__file__).resolve().parents[1]
state = source / "resources" / ".state"
state.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="resource-python-", dir=state) as temporary:
    stage = Path(temporary)
    subprocess.run([cmake, "--install", build, "--prefix", str(stage), "--config", configuration], check=True,
                   stdout=subprocess.DEVNULL)
    environment = os.environ.copy()
    for name in ("DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LD_LIBRARY_PATH", "PYTHONPATH"):
        environment.pop(name, None)
    subprocess.run([sys.executable, "-B", str(source / "tests/resource_python.py"),
                    str(stage / library_dir / library_name),
                    str(stage / "share/iiSDKCore/python/iisdkcore_resources.py")],
                   env=environment, check=True)
