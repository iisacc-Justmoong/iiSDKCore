"""Check iOS and WebAssembly Core target kinds without executing an app."""
from pathlib import Path
import subprocess
import sys
import tempfile

source, cmake, emscripten = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='mobile-shared-', dir=source/'build') as tmp:
    root=Path(tmp)
    hook=root/'check.cmake'
    hook.write_text('''function(_check_core_shared)
get_target_property(core_type iiSDKCore TYPE)
get_target_property(obs_type iiSDKCoreObservation TYPE)
get_target_property(embedded_type iiSDKCoreEmbedded TYPE)
if(NOT core_type STREQUAL "SHARED_LIBRARY" OR NOT obs_type STREQUAL "SHARED_LIBRARY" OR NOT embedded_type STREQUAL "SHARED_LIBRARY")
 message(FATAL_ERROR "Mobile dynamic Core, Observation and Embedded must be shared")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
 get_target_property(core_framework iiSDKCore FRAMEWORK)
 get_target_property(obs_framework iiSDKCoreObservation FRAMEWORK)
 get_target_property(embedded_framework iiSDKCoreEmbedded FRAMEWORK)
 if(NOT core_framework OR NOT obs_framework OR NOT embedded_framework)
  message(FATAL_ERROR "iOS dynamic Core, Observation and Embedded must be frameworks")
 endif()
endif()
endfunction()
cmake_language(DEFER CALL _check_core_shared)
''')
    cases=[('ios',['-DCMAKE_SYSTEM_NAME=iOS','-DCMAKE_OSX_SYSROOT=iphonesimulator','-DCMAKE_OSX_ARCHITECTURES=arm64']),
           ('wasm',['-DCMAKE_TOOLCHAIN_FILE='+str(emscripten)])]
    for name,args in cases:
        result=subprocess.run([str(cmake),'-S',str(source),'-B',str(root/name/'build'),'-G','Ninja',
            '-DBUILD_TESTING=OFF','-DBUILD_SHARED_LIBS=ON',
            '-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES='+str(hook),*args],capture_output=True,text=True,timeout=180)
        assert result.returncode==0, name+'\n'+result.stdout+'\n'+result.stderr
print('iOS framework and WebAssembly shared Core configuration passed')
