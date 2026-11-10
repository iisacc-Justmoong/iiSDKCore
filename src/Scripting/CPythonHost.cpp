#include "CPythonHost.h"

#include <Python.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace ii::scripting {
namespace {
namespace fs = std::filesystem;

class Object final {
public:
    explicit Object(PyObject* value = nullptr) : value_(value) {}
    ~Object() { Py_XDECREF(value_); }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Object& operator=(Object&& other) noexcept {
        if (this != &other) { Py_XDECREF(value_); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    PyObject* get() const noexcept { return value_; }
    PyObject* release() noexcept { return std::exchange(value_, nullptr); }
    explicit operator bool() const noexcept { return value_ != nullptr; }
private:
    PyObject* value_;
};

class Gil final {
public:
    Gil() : state_(PyGILState_Ensure()) {}
    ~Gil() { PyGILState_Release(state_); }
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;
private:
    PyGILState_STATE state_;
};

std::once_flag interpreter_once;
std::string interpreter_failure;
fs::path initialized_home;
bool initialized_by_adapter = false;

std::string python_error(std::string context) {
    if (!PyErr_Occurred()) return context;
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    Object type_owner(type), value_owner(value), traceback_owner(traceback);
    Object message(value ? PyObject_Str(value) : nullptr);
    if (message) {
        if (const char* text = PyUnicode_AsUTF8(message.get())) context += ": " + std::string(text);
    }
    PyErr_Clear();
    return context;
}

core::Result<void> initialize(const fs::path& requested_home) {
    std::call_once(interpreter_once, [&] {
        if (Py_IsInitialized()) {
            if (!requested_home.empty()) interpreter_failure = "Python home cannot be changed after interpreter initialization";
            return;
        }
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        config.install_signal_handlers = 0;
        config.parse_argv = 0;
        config.write_bytecode = 0;
        if (!requested_home.empty()) {
            const auto home = requested_home.wstring();
            const auto status = PyConfig_SetString(&config, &config.home, home.c_str());
            if (PyStatus_Exception(status)) {
                interpreter_failure = status.err_msg ? status.err_msg : "Could not set CPython home";
                PyConfig_Clear(&config);
                return;
            }
        }
        const auto status = Py_InitializeFromConfig(&config);
        if (PyStatus_Exception(status)) interpreter_failure = status.err_msg ? status.err_msg : "CPython initialization failed";
        PyConfig_Clear(&config);
        if (!interpreter_failure.empty()) return;
        initialized_home = requested_home;
        initialized_by_adapter = true;
        PyEval_SaveThread();
    });
    if (!interpreter_failure.empty()) return core::fail(core::ErrorCode::failed, interpreter_failure);
    if (!Py_IsInitialized()) return core::fail(core::ErrorCode::closed, "CPython has been finalized");
    if (!requested_home.empty() && (!initialized_by_adapter || requested_home != initialized_home))
        return core::fail(core::ErrorCode::invalid_argument, "CPython home differs from the process interpreter");
    return {};
}

Object python_path(const fs::path& path) {
#ifdef _WIN32
    const auto native = path.native();
    return Object(PyUnicode_FromWideChar(native.c_str(), static_cast<Py_ssize_t>(native.size())));
#else
    const auto native = path.native();
    return Object(PyUnicode_DecodeFSDefaultAndSize(native.data(), static_cast<Py_ssize_t>(native.size())));
#endif
}

core::Result<fs::path> filesystem_path(PyObject* text) {
#ifdef _WIN32
    Py_ssize_t size = 0;
    wchar_t* raw = PyUnicode_AsWideCharString(text, &size);
    if (!raw) return core::fail(core::ErrorCode::failed, python_error("Invalid module path"));
    fs::path result(std::wstring(raw, static_cast<std::size_t>(size)));
    PyMem_Free(raw);
    return result;
#else
    Object encoded(PyUnicode_EncodeFSDefault(text));
    if (!encoded) return core::fail(core::ErrorCode::failed, python_error("Invalid module path"));
    char* raw = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(encoded.get(), &raw, &size) < 0)
        return core::fail(core::ErrorCode::failed, python_error("Invalid module path"));
    return fs::path(std::string(raw, static_cast<std::size_t>(size)));
#endif
}

bool inside_directory(const fs::path& file, const fs::path& root) {
    std::error_code ec;
    for (auto parent = file.parent_path(); !parent.empty();) {
        if (fs::equivalent(parent, root, ec) && !ec) return true;
        ec.clear();
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return false;
}

core::Result<fs::path> check_entry_module(const std::string& name, const std::vector<fs::path>& paths) {
    Object util(PyImport_ImportModule("importlib.util"));
    if (!util) return core::fail(core::ErrorCode::failed, python_error("Cannot load CPython import utilities"));
    Object spec(PyObject_CallMethod(util.get(), "find_spec", "s", name.c_str()));
    if (!spec) return core::fail(core::ErrorCode::failed, python_error("Cannot resolve AOT module " + name));
    if (spec.get() == Py_None) return core::fail(core::ErrorCode::not_found, "AOT module not found: " + name);
    Object origin(PyObject_GetAttrString(spec.get(), "origin"));
    if (!origin || !PyUnicode_Check(origin.get()))
        return core::fail(core::ErrorCode::invalid_argument, python_error("AOT module has no native file origin"));
    Object machinery(PyImport_ImportModule("importlib.machinery"));
    Object suffixes(machinery ? PyObject_GetAttrString(machinery.get(), "EXTENSION_SUFFIXES") : nullptr);
    if (!suffixes || !PyList_Check(suffixes.get()))
        return core::fail(core::ErrorCode::failed, python_error("Cannot read CPython extension suffixes"));
    const auto is_native = [&](PyObject* candidate) {
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(suffixes.get()); ++i) {
            Object match(PyObject_CallMethod(candidate, "endswith", "O", PyList_GET_ITEM(suffixes.get(), i)));
            if (!match) return -1;
            const auto yes = PyObject_IsTrue(match.get());
            if (yes != 0) return yes;
        }
        return 0;
    };
    auto native = is_native(origin.get());
    if (native < 0) return core::fail(core::ErrorCode::failed, python_error("Cannot inspect module suffix"));
    if (!native) {
        // Some AOT compilers rewrite __spec__.origin to a source-like filename.
        // The standard extension loader still records the binary it loaded.
        Object loader(PyObject_GetAttrString(spec.get(), "loader"));
        Object extension_type(PyObject_GetAttrString(machinery.get(), "ExtensionFileLoader"));
        if (loader && extension_type && PyObject_IsInstance(loader.get(), extension_type.get()) == 1) {
            Object filename(PyObject_CallMethod(loader.get(), "get_filename", "s", name.c_str()));
            if (filename && PyUnicode_Check(filename.get())) {
                native = is_native(filename.get());
                if (native == 1) origin = std::move(filename);
            }
        }
        if (native < 0) return core::fail(core::ErrorCode::failed, python_error("Cannot inspect extension loader"));
        if (!native) PyErr_Clear();
    }
    if (!native) return core::fail(core::ErrorCode::invalid_argument, "Entry module is not a native CPython extension: " + name);
    auto file = filesystem_path(origin.get());
    if (!file) return std::unexpected(file.error());
    std::error_code ec;
    const auto resolved = fs::weakly_canonical(*file, ec);
    if (ec || !fs::is_regular_file(resolved, ec) || ec)
        return core::fail(core::ErrorCode::not_found, "AOT module file does not exist: " + name);
    for (const auto& root : paths) if (inside_directory(resolved, root)) return resolved;
    return core::fail(core::ErrorCode::invalid_argument, "AOT module is outside configured module directories: " + name);
}

core::Result<Object> verified_modules() {
    Object sys(PyImport_ImportModule("sys"));
    if (!sys) return core::fail(core::ErrorCode::failed, python_error("Cannot access CPython module registry"));
    Object registry(PyObject_GetAttrString(sys.get(), "_iisdk_aot_verified_modules"));
    if (!registry) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return core::fail(core::ErrorCode::failed, python_error("Cannot read AOT module registry"));
        PyErr_Clear();
        registry = Object(PyDict_New());
        if (!registry || PyObject_SetAttrString(sys.get(), "_iisdk_aot_verified_modules", registry.get()) < 0)
            return core::fail(core::ErrorCode::failed, python_error("Cannot create AOT module registry"));
    }
    if (!PyDict_Check(registry.get())) return core::fail(core::ErrorCode::invalid_argument, "AOT module registry is not a dictionary");
    return registry;
}

bool previously_verified(PyObject* registry, const std::string& name, PyObject* module,
                         const std::vector<fs::path>& paths) {
    auto* entry = PyDict_GetItemString(registry, name.c_str());
    if (!entry || !PyTuple_Check(entry) || PyTuple_GET_SIZE(entry) != 2) return false;
    auto* reference = PyTuple_GET_ITEM(entry, 0);
    auto* origin = PyTuple_GET_ITEM(entry, 1);
    if (!PyWeakref_CheckRef(reference) || !PyUnicode_Check(origin)) return false;
#if PY_VERSION_HEX >= 0x030D0000
    PyObject* resolved = nullptr;
    const int alive = PyWeakref_GetRef(reference, &resolved);
    Object strong_reference(resolved);
    if (alive < 0) { PyErr_Clear(); return false; }
    if (alive == 0 || strong_reference.get() != module) return false;
#else
    if (PyWeakref_GetObject(reference) != module) return false;
#endif
    auto path = filesystem_path(origin);
    if (!path) { PyErr_Clear(); return false; }
    std::error_code ec;
    if (!fs::is_regular_file(*path, ec) || ec) return false;
    for (const auto& root : paths) if (inside_directory(*path, root)) return true;
    return false;
}

core::Result<void> remember_module(PyObject* registry, const std::string& name, PyObject* module,
                                   const fs::path& origin) {
    Object reference(PyWeakref_NewRef(module, nullptr));
    auto path = python_path(origin);
    Object entry(reference && path ? PyTuple_Pack(2, reference.get(), path.get()) : nullptr);
    if (!entry || PyDict_SetItemString(registry, name.c_str(), entry.get()) < 0)
        return core::fail(core::ErrorCode::failed, python_error("Cannot record verified AOT module"));
    return {};
}

core::Result<Object> to_python(const core::Value& input, unsigned depth) {
    if (depth > 128) return core::fail(core::ErrorCode::out_of_range, "Python argument nesting exceeds 128");
    Object result;
    switch (input.kind()) {
    case core::ValueKind::null: Py_INCREF(Py_None); result = Object(Py_None); break;
    case core::ValueKind::boolean: result = Object(PyBool_FromLong(std::get<bool>(input.storage()))); break;
    case core::ValueKind::signed_integer: result = Object(PyLong_FromLongLong(std::get<std::int64_t>(input.storage()))); break;
    case core::ValueKind::unsigned_integer: result = Object(PyLong_FromUnsignedLongLong(std::get<std::uint64_t>(input.storage()))); break;
    case core::ValueKind::real: result = Object(PyFloat_FromDouble(std::get<double>(input.storage()))); break;
    case core::ValueKind::text: {
        const auto& text = std::get<std::string>(input.storage());
        result = Object(PyUnicode_DecodeUTF8(text.data(), static_cast<Py_ssize_t>(text.size()), "strict"));
        break;
    }
    case core::ValueKind::bytes: {
        const auto& bytes = std::get<core::Bytes>(input.storage());
        result = Object(PyBytes_FromStringAndSize(reinterpret_cast<const char*>(bytes.data()), static_cast<Py_ssize_t>(bytes.size())));
        break;
    }
    case core::ValueKind::array: {
        const auto& items = std::get<core::Value::Array>(input.storage());
        result = Object(PyList_New(static_cast<Py_ssize_t>(items.size())));
        if (!result) break;
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto item = to_python(items[i], depth + 1);
            if (!item) return std::unexpected(item.error());
            PyList_SET_ITEM(result.get(), static_cast<Py_ssize_t>(i), item->release());
        }
        break;
    }
    case core::ValueKind::object: {
        result = Object(PyDict_New());
        if (!result) break;
        for (const auto& [key, value] : std::get<core::Value::Object>(input.storage())) {
            Object name(PyUnicode_DecodeUTF8(key.data(), static_cast<Py_ssize_t>(key.size()), "strict"));
            auto item = to_python(value, depth + 1);
            if (!item) return std::unexpected(item.error());
            if (!name || PyDict_SetItem(result.get(), name.get(), item->get()) < 0)
                return core::fail(core::ErrorCode::failed, python_error("Cannot convert Python dictionary"));
        }
        break;
    }
    }
    if (!result) return core::fail(core::ErrorCode::failed, python_error("Cannot convert Python argument"));
    return result;
}

core::Result<core::Value> from_python(PyObject* value, unsigned depth, std::set<PyObject*>& active) {
    if (depth > 128) return core::fail(core::ErrorCode::out_of_range, "Python result nesting exceeds 128");
    if (value == Py_None) return core::Value{};
    if (PyBool_Check(value)) return core::Value(value == Py_True);
    if (PyLong_Check(value)) {
        int overflow = 0;
        const auto number = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (!overflow && !PyErr_Occurred()) return core::Value(static_cast<std::int64_t>(number));
        PyErr_Clear();
        if (overflow > 0) {
            const auto number_unsigned = PyLong_AsUnsignedLongLong(value);
            if (!PyErr_Occurred()) return core::Value(static_cast<std::uint64_t>(number_unsigned));
            PyErr_Clear();
        }
        return core::fail(core::ErrorCode::out_of_range, "Python integer is outside Core Value range");
    }
    if (PyFloat_Check(value)) {
        const auto number = PyFloat_AsDouble(value);
        if (PyErr_Occurred() || !std::isfinite(number))
            return core::fail(core::ErrorCode::invalid_argument, python_error("Python result must be a finite float"));
        return core::Value(number);
    }
    if (PyUnicode_Check(value)) {
        Py_ssize_t size = 0;
        const char* text = PyUnicode_AsUTF8AndSize(value, &size);
        if (!text) return core::fail(core::ErrorCode::invalid_argument, python_error("Python result must be UTF-8"));
        return core::Value(std::string(text, static_cast<std::size_t>(size)));
    }
    if (PyBytes_Check(value)) {
        char* data = nullptr;
        Py_ssize_t size = 0;
        if (PyBytes_AsStringAndSize(value, &data, &size) < 0)
            return core::fail(core::ErrorCode::failed, python_error("Cannot read Python bytes"));
        core::Bytes bytes(static_cast<std::size_t>(size));
        if (size) std::memcpy(bytes.data(), data, static_cast<std::size_t>(size));
        return core::Value(std::move(bytes));
    }
    if (!PyList_Check(value) && !PyTuple_Check(value) && !PyDict_Check(value))
        return core::fail(core::ErrorCode::unsupported, "Python result type is not representable as Core Value");
    if (!active.insert(value).second) return core::fail(core::ErrorCode::invalid_argument, "Python result contains a cycle");
    struct Active final {
        std::set<PyObject*>& set;
        PyObject* value;
        ~Active() { set.erase(value); }
    } active_guard{active, value};
    if (PyList_Check(value) || PyTuple_Check(value)) {
        const auto size = PySequence_Size(value);
        if (size < 0) return core::fail(core::ErrorCode::failed, python_error("Cannot read Python sequence"));
        core::Value::Array items;
        items.reserve(static_cast<std::size_t>(size));
        for (Py_ssize_t i = 0; i < size; ++i) {
            Object item(PySequence_GetItem(value, i));
            if (!item) return core::fail(core::ErrorCode::failed, python_error("Cannot read Python sequence item"));
            auto converted = from_python(item.get(), depth + 1, active);
            if (!converted) return std::unexpected(converted.error());
            items.push_back(std::move(*converted));
        }
        return core::Value(std::move(items));
    }
    core::Value::Object items;
    Py_ssize_t position = 0;
    PyObject *key = nullptr, *item = nullptr;
    while (PyDict_Next(value, &position, &key, &item)) {
        if (!PyUnicode_Check(key)) return core::fail(core::ErrorCode::unsupported, "Python dictionary keys must be strings");
        Py_ssize_t size = 0;
        const char* text = PyUnicode_AsUTF8AndSize(key, &size);
        if (!text) return core::fail(core::ErrorCode::invalid_argument, python_error("Python dictionary key must be UTF-8"));
        auto converted = from_python(item, depth + 1, active);
        if (!converted) return std::unexpected(converted.error());
        items.emplace(std::string(text, static_cast<std::size_t>(size)), std::move(*converted));
    }
    return core::Value(std::move(items));
}

} // namespace

struct CPythonHost::State final {
    std::vector<fs::path> module_paths;
    PythonResult call(const PythonCall& request) const {
        if (!Py_IsInitialized()) return core::fail(core::ErrorCode::closed, "CPython has been finalized");
        if (request.module.empty() || request.function.empty() || request.module.find('\0') != std::string::npos ||
            request.function.find('\0') != std::string::npos)
            return core::fail(core::ErrorCode::invalid_argument, "Python module and function names must be nonempty without NUL");
        Gil gil;
        auto registry = verified_modules();
        if (!registry) return std::unexpected(registry.error());
        auto* existing = PyDict_GetItemString(PyImport_GetModuleDict(), request.module.c_str());
        Object module;
        if (existing && previously_verified(registry->get(), request.module, existing, module_paths)) {
            Py_INCREF(existing);
            module = Object(existing);
        } else {
            auto valid = check_entry_module(request.module, module_paths);
            if (!valid) return std::unexpected(valid.error());
            module = Object(PyImport_ImportModule(request.module.c_str()));
            if (!module) return core::fail(core::ErrorCode::failed, python_error("Cannot import AOT module " + request.module));
            if (auto remembered = remember_module(registry->get(), request.module, module.get(), *valid); !remembered)
                return std::unexpected(remembered.error());
        }
        Object function(PyObject_GetAttrString(module.get(), request.function.c_str()));
        if (!function) return core::fail(core::ErrorCode::not_found, python_error("Python function not found: " + request.function));
        if (!PyCallable_Check(function.get())) return core::fail(core::ErrorCode::invalid_argument, "Python attribute is not callable");
        Object arguments(PyTuple_New(static_cast<Py_ssize_t>(request.arguments.size())));
        if (!arguments) return core::fail(core::ErrorCode::failed, python_error("Cannot allocate Python arguments"));
        for (std::size_t i = 0; i < request.arguments.size(); ++i) {
            auto converted = to_python(request.arguments[i], 0);
            if (!converted) return std::unexpected(converted.error());
            PyTuple_SET_ITEM(arguments.get(), static_cast<Py_ssize_t>(i), converted->release());
        }
        Object result(PyObject_CallObject(function.get(), arguments.get()));
        if (!result) return core::fail(core::ErrorCode::failed, python_error("Python call failed"));
        std::set<PyObject*> active;
        return from_python(result.get(), 0, active);
    }
};

CPythonHost::CPythonHost(runtime::SerialQueue& queue, std::shared_ptr<State> state)
    : state_(std::move(state)), queries_(queue, [state=state_](const PythonCall& call) { return state->call(call); }) {}
CPythonHost::~CPythonHost() = default;

core::Result<std::unique_ptr<CPythonHost>> CPythonHost::attach(runtime::SerialQueue& queue, CPythonOptions options) {
    try {
        if (options.module_paths.empty()) return core::fail(core::ErrorCode::invalid_argument, "At least one AOT module directory is required");
        for (auto& path : options.module_paths) {
            std::error_code ec;
            path = fs::weakly_canonical(path, ec);
            if (ec || !fs::is_directory(path, ec) || ec)
                return core::fail(core::ErrorCode::invalid_argument, "AOT module directory does not exist");
        }
        if (auto ready = initialize(options.python_home); !ready) return std::unexpected(ready.error());
        Gil gil;
        int major = 0, minor = 0;
        if (std::sscanf(Py_GetVersion(), "%d.%d", &major, &minor) != 2 ||
            major != PY_MAJOR_VERSION || minor != PY_MINOR_VERSION)
            return core::fail(core::ErrorCode::unsupported, "Loaded CPython ABI does not match the Core adapter");
        Object sys(PyImport_ImportModule("sys"));
        Object implementation(sys ? PyObject_GetAttrString(sys.get(), "implementation") : nullptr);
        Object name(implementation ? PyObject_GetAttrString(implementation.get(), "name") : nullptr);
        if (!name || !PyUnicode_Check(name.get()) || PyUnicode_CompareWithASCIIString(name.get(), "cpython") != 0)
            return core::fail(core::ErrorCode::unsupported, python_error("Only CPython is supported"));
        Object search_path(PyObject_GetAttrString(sys.get(), "path"));
        if (!search_path || !PyList_Check(search_path.get()))
            return core::fail(core::ErrorCode::failed, python_error("CPython sys.path is not a list"));
        for (auto it = options.module_paths.rbegin(); it != options.module_paths.rend(); ++it) {
            auto path = python_path(*it);
            if (!path) return core::fail(core::ErrorCode::failed, python_error("Cannot encode AOT module path"));
            const auto exists = PySequence_Contains(search_path.get(), path.get());
            if (exists < 0 || (!exists && PyList_Insert(search_path.get(), 0, path.get()) < 0))
                return core::fail(core::ErrorCode::failed, python_error("Cannot register AOT module path"));
        }
        auto state = std::make_shared<State>();
        state->module_paths = std::move(options.module_paths);
        return std::unique_ptr<CPythonHost>(new CPythonHost(queue, std::move(state)));
    } catch (const std::exception& error) {
        return core::fail(core::ErrorCode::failed, error.what(), std::current_exception());
    }
}

} // namespace ii::scripting
