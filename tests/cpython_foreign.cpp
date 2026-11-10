#include <iiSDKCore/Scripting/CPythonHost.h>
#include <Python.h>

#include <future>
#include <iostream>
#include <stdexcept>

using namespace ii;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    Py_Initialize();
    PyThreadState* interpreter = PyEval_SaveThread();
    int exit_code = 0;
    try {
        runtime::Runtime runtime({.workers=1, .capacity=8});
        runtime::SerialQueue queue(runtime);
        auto host = scripting::CPythonHost::attach(queue, {.module_paths={argv[1]}});
        check(bool(host), "attach to an existing CPython interpreter");
        std::promise<runtime::QueryReply<scripting::PythonResult>> promise;
        auto future = promise.get_future();
        check(bool((*host)->queries().post({"iisdk_cpython_fixture", "echo", {core::Value("existing")}},
            [&](auto reply) { promise.set_value(std::move(reply)); })), "submit to Core queue");
        auto result = future.get();
        check(result && *result && (**result) == core::Value("existing"), "existing interpreter executes AOT module");
        host->reset();
        check(bool(queue.close()), "close queue before runtime");
        check(bool(runtime.join()), "join runtime before interpreter shutdown");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        exit_code = 1;
    }
    PyEval_RestoreThread(interpreter);
    if (Py_FinalizeEx() < 0) exit_code = 1;
    return exit_code;
}
