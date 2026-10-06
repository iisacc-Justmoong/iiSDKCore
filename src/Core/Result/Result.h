#pragma once
#include <exception>
#include <expected>
#include <string>

namespace ii::core {
enum class ErrorCode { invalid_argument, out_of_range, not_found, already_exists, full,
    cycle, unsupported, failed, cancelled, closed, busy, wrong_executor };
struct Error final { ErrorCode code; std::string message; std::exception_ptr cause{}; };
template<class T> using Result = std::expected<T, Error>;
inline std::unexpected<Error> fail(ErrorCode code, std::string message, std::exception_ptr cause = {}) {
    return std::unexpected(Error{code,std::move(message),std::move(cause)});
}
} // namespace ii::core
