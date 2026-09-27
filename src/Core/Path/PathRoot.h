#pragma once
#include "../Result/Result.h"
#include <filesystem>
#include <stdexcept>

namespace ii::core {
// Lexical resource root, NOT a symlink/security sandbox.
class PathRoot final {
public:
    explicit PathRoot(std::filesystem::path root) : root_(std::move(root).lexically_normal()) {
        if (!root_.is_absolute() ||
            root_.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            throw std::invalid_argument("PathRoot requires an absolute root without NUL");
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }
    [[nodiscard]] Result<std::filesystem::path> resolve(const std::filesystem::path& relative) const {
        if (relative.empty() || relative.has_root_path() ||
            relative.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            return fail(ErrorCode::invalid_argument,"Expected a nonempty relative path");
        auto normalized=relative.lexically_normal();
        if (*normalized.begin()=="..") return fail(ErrorCode::out_of_range,"Path escapes its lexical root");
        return (root_/normalized).lexically_normal();
    }
private:
    std::filesystem::path root_;
};
} // namespace ii::core
