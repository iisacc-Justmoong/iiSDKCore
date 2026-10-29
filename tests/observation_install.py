"""Install/relocate the observation component and execute hidden-module consumers without Runtime."""
from pathlib import Path
import argparse,json,re,shutil,subprocess,tempfile
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build',type=Path,required=True);parser.add_argument('--cmake',required=True);args=parser.parse_args()
    build=args.build.resolve();source=Path(__file__).resolve().parents[1]
    cache=dict(re.findall(r'^([^/#][^:\n]*):[^=\n]*=(.*)$',(build/'CMakeCache.txt').read_text(),re.M))
    options=[f'-D{name}={cache[name]}' for name in ('CMAKE_CXX_COMPILER','CMAKE_CXX_FLAGS','CMAKE_EXE_LINKER_FLAGS','CMAKE_SHARED_LINKER_FLAGS','CMAKE_OSX_DEPLOYMENT_TARGET') if cache.get(name)]
    def run(*command):
        result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        print(result.stdout,end='',flush=True)
        if result.returncode:raise RuntimeError(f'Command failed with {result.returncode}: {command}')
    with tempfile.TemporaryDirectory(prefix='observation-package-',dir=build) as temporary:
        root=Path(temporary);stage=root/'stage';relocated=root/'relocated'
        run(args.cmake,'--install',str(build),'--prefix',str(stage));stage.rename(relocated)
        project=root/'project';shutil.copytree(source/'tests/observation-consumer',project)
        shutil.copy2(source/'tests/observable_module.cpp',project/'main.cpp')
        shutil.copytree(source/'tests/fixtures/observable_module',project/'fixtures/observable_module')
        target=project/'build'
        run(args.cmake,'-S',str(project),'-B',str(target),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',f'-DiiSDKCore_DIR={relocated}/lib/cmake/iiSDKCore',*options)
        run(args.cmake,'--build',str(target),'-j','2');run('ctest','--test-dir',str(target),'--output-on-failure','-j','1')
        contracts=root/'contracts';shutil.copytree(source/'tests/contracts-consumer',contracts)
        shutil.copy2(source/'tests/contracts.cpp',root/'contracts.cpp');shutil.copy2(source/'tests/text.cpp',root/'text.cpp')
        run(args.cmake,'-S',str(contracts),'-B',str(contracts/'build'),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',f'-DiiSDKCore_DIR={relocated}/lib/cmake/iiSDKCore',*options)
        run(args.cmake,'--build',str(contracts/'build'),'-j','2');run('ctest','--test-dir',str(contracts/'build'),'--output-on-failure','-j','1')
        result={'installed':True,'relocated':True,'observation_only_component_without_runtime_or_threads':True,
                'hidden_module_cases_passed':6,'contracts_only_cases_passed':2,'contracts_imports_observation':False,
                'app_window_or_GPU_created':False}
        (build/'observation-installed-consumer.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2))
if __name__=='__main__':main()
