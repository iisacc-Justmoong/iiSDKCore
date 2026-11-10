#pragma once

#include "../Core/Result/Result.h"
#include "../Core/Value/Value.h"
#include "../Runtime/Query.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ii::scripting {

struct CPythonOptions final {
    std::vector<std::filesystem::path> module_paths;
    std::filesystem::path python_home;
};

struct PythonCall final {
    std::string module;
    std::string function;
    std::vector<core::Value> arguments;
};
using PythonResult = core::Result<core::Value>;

// The host is attached to a Core serial execution domain. Entry modules must
// be native CPython extensions inside an explicitly supplied module directory.
class CPythonHost final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<CPythonHost>> attach(
        runtime::SerialQueue& queue, CPythonOptions options);
    ~CPythonHost();
    CPythonHost(const CPythonHost&) = delete;
    CPythonHost& operator=(const CPythonHost&) = delete;
    [[nodiscard]] const runtime::QueryPort<PythonCall, PythonResult>& queries() const noexcept { return queries_; }

private:
    struct State;
    CPythonHost(runtime::SerialQueue& queue, std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    runtime::QueryPort<PythonCall, PythonResult> queries_;
};

} // namespace ii::scripting
