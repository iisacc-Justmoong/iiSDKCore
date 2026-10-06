#pragma once
#include "Requests.h"
#include <type_traits>

namespace ii::io::detail {
template<class Request> bool valid_transfer(const Request& request) {
    const auto size=request.buffer.bytes().size();
    return request.timeout.count()>=0 && request.offset<=size &&
        (request.length==std::dynamic_extent || request.length<=size-request.offset);
}
template<class Request> IOResult transfer_now(const Channel& channel, const Request& request) {
    if (!valid_transfer(request)) return std::unexpected(Error{ErrorCode::invalid_argument,{}});
    if (request.stop.cancelled()) return std::unexpected(Error{ErrorCode::cancelled,{}});
    const auto range=request.buffer.bytes().subspan(request.offset,request.length);
    IOResponse response{request.buffer,{},0};
    if constexpr (std::is_same_v<Request,Read>) {
        auto result=channel.read(range);
        if (!result) return std::unexpected(result.error());
        response.transfer=*result;
    } else {
        auto result=channel.write(range);
        if (!result) return std::unexpected(result.error());
        response.transfer.bytes=*result;
    }
    return response;
}
} // namespace ii::io::detail
