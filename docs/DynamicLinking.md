# Dynamic linking

The Runtime, Observation and optional CPython embedding host are shared
libraries by default. New iOS and WebAssembly configurations select the
framework and side-module variants when shared linking is enabled. CPython
requires the matching native interpreter and embedding library; its shared
host supports both older weak-reference APIs and Python 3.13 and later.

`IISDKCORE_STATIC_PLUGINS=ON` with `BUILD_SHARED_LIBS=OFF` remains an explicit
application-specific static mode. The `Embedded` target is shared in normal
OS builds; the freestanding-only profile remains an archive for boards without
a dynamic loader.
Applications must bundle compatible runtime libraries and dependencies.

The Emscripten package configuration enables CMake shared-library imports for side modules. The builder or application must supply the main-module compile and link options.
The imported Runtime also supplies the installed Observation directory to the
Wasm linker so the transitive side module resolves after relocation.
