"""Build and run a consumer against a relocated, temporary Core installation."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--ctest', required=True)
    parser.add_argument('--consumer', default='session-consumer')
    args = parser.parse_args()
    build = args.build.resolve()
    source = Path(__file__).resolve().parent / args.consumer
    cache = dict(re.findall(r'^([^/#][^:\n]*):[^=\n]*=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M))
    options = [f'-D{name}={cache[name]}' for name in
               ('CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS', 'CMAKE_EXE_LINKER_FLAGS',
                'CMAKE_SHARED_LINKER_FLAGS', 'CMAKE_OSX_DEPLOYMENT_TARGET',
                'CMAKE_OSX_ARCHITECTURES') if cache.get(name)]

    def run(*command):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            print(result.stdout, flush=True)
            raise RuntimeError(f'Command failed with {result.returncode}: {command}')

    with tempfile.TemporaryDirectory(prefix=f'{source.name}-', dir=build) as temporary:
        root = Path(temporary)
        stage, relocated = root / 'stage', root / 'relocated'
        run(args.cmake, '--install', str(build), '--prefix', str(stage))
        stage.rename(relocated)
        consumer = root / 'build'
        package_dir = relocated / cache.get('CMAKE_INSTALL_LIBDIR', 'lib') / 'cmake' / 'iiSDKCore'
        run(args.cmake, '-S', str(source), '-B', str(consumer), '-G', cache.get('CMAKE_GENERATOR', 'Ninja'),
            '-DCMAKE_BUILD_TYPE=Release', f'-DiiSDKCore_DIR={package_dir}', *options)
        run(args.cmake, '--build', str(consumer), '--config', 'Release', '--parallel', '2')
        run(args.ctest, '--test-dir', str(consumer), '-C', 'Release', '--output-on-failure')
    print(f'Relocated {source.name} consumer passed')


if __name__ == '__main__':
    main()
