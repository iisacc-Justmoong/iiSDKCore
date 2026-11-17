"""Check the installed Web application helper's optional C++ module scanning policy."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core', required=True, type=Path)
parser.add_argument('--toolchain', required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='.wasm-application-', dir=root / 'resources') as source_dir, \
     tempfile.TemporaryDirectory(prefix='wasm-application-', dir=root / 'build') as output_dir:
    source = Path(source_dir)
    (source / 'main.cpp').write_text('int application_probe() { return 0; }\n')
    (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.31)
project(WasmApplicationScan LANGUAGES CXX)
find_package(iiSDKCore CONFIG REQUIRED COMPONENTS Runtime)
iisdkcore_add_application(App SOURCES main.cpp)
get_target_property(scan App CXX_SCAN_FOR_MODULES)
if(NOT scan STREQUAL EXPECT_SCAN)
    message(FATAL_ERROR "Expected CXX module scanning ${EXPECT_SCAN}, got ${scan}")
endif()
''')
    for name, flag, expected in [('default', None, 'OFF'), ('explicit-on', 'ON', 'ON'), ('explicit-off', 'OFF', 'OFF')]:
        command = ['cmake', '-S', str(source), '-B', str(Path(output_dir) / name), '-G', 'Ninja',
                   f'-DCMAKE_TOOLCHAIN_FILE={args.toolchain.resolve()}',
                   f'-DiiSDKCore_DIR={args.core.resolve() / "lib/cmake/iiSDKCore"}', f'-DEXPECT_SCAN={expected}']
        if flag is not None:
            command.append(f'-DCMAKE_CXX_SCAN_FOR_MODULES={flag}')
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f'{name}: {result.stdout}\n{result.stderr}')
print('Web application module scanning defaults off and respects explicit overrides')
