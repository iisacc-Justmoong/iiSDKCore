# CPython AOT runtime attachment

`iiSDKCore::CPython` is an optional adapter between compiled CPython extension
modules and a Core `SerialQueue`. The base Runtime, Contracts, and Values
targets do not acquire a Python dependency. The host supplies the target's
CPython development library, module search directories, and optional Python
home. AOT generation belongs to iiSDKBuilder; Core does not depend on Builder
or on an application SDK.

`CPythonHost::attach(queue, options)` initializes CPython if needed, or joins
an already initialized CPython interpreter. Its query port invokes a named
function of an AOT extension module in that queue. The entry module must resolve
to a native CPython extension within a supplied module directory. Imported
dependencies and dynamic Python features remain CPython's responsibility.
Inputs and outputs cross the native boundary as `core::Value`: null, booleans,
64-bit signed/unsigned integers, finite doubles, UTF-8 strings, bytes, arrays
and string-keyed dictionaries. Unsupported or cyclic results and Python
exceptions are reported as errors rather than converted silently.
Python integers in the signed 64-bit range return as signed Core values;
larger nonnegative values use unsigned 64-bit values. Python does not preserve
the original signed/unsigned tag for small nonnegative integers. Conversion
limits nesting to 128 levels and rejects non-finite reals and other objects.

The interpreter is process-wide. Host instances and their `SerialQueue` must
be destroyed before the Runtime. The adapter does not finalize or reinitialize
the process interpreter, and an external owner must not finalize it while a
host is active. Python execution takes the CPython GIL, including when work is
dispatched to different Core worker threads. `sys.path` and `sys.modules`
remain interpreter-global; configured directories are added to the shared path.
The app owns runtime packaging, module dependencies, and ABI compatibility.
An already initialized foreign CPython host must release its GIL while waiting
for a Core query, then may finalize CPython only after all attached hosts close.
Verified entry modules are tracked by weak reference inside the interpreter so
that multiple hosts can reuse an AOT module even if its compiler rewrites
`__spec__.origin`. This registry is not a sandbox or a security boundary.

Enable with `IISDKCORE_BUILD_CPYTHON=ON` and provide a matching CPython
interpreter, headers and embedding library. Installed consumers use
`find_package(iiSDKCoreCPython CONFIG REQUIRED)` and link
`iiSDKCore::CPython`. This optional package is distinct from the default
no-Python Core runtime. Its `IISDK_CPYTHON_EXTENSION_SUFFIX` target property
identifies the selected interpreter's extension ABI for build-time checks.
On macOS, the imported target adds the discovered
Python framework directory to the linker's runtime search paths. An app that
relocates or bundles that framework must supply suitable deployment paths.
