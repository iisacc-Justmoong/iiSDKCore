#include "AsyncIO.h"

namespace ii::io {
namespace {
template<class Query> IOResult transfer(const Channel& channel, const Query& query) {
    const auto bytes=query.buffer.bytes();
    if (query.timeout.count() < 0 || query.offset > bytes.size() || (query.length != std::dynamic_extent && query.length > bytes.size()-query.offset))
        return std::unexpected(Error{ErrorCode::invalid_argument, {}});
    const auto range=bytes.subspan(query.offset, query.length);
    if (query.stop.cancelled()) return std::unexpected(Error{ErrorCode::cancelled, {}});
    constexpr bool reading=std::is_same_v<Query, Read>;
    // An empty operation does not wait for data, but still validates the channel.
    auto ready=channel.wait(reading ? Interest::readable : Interest::writable,
        range.empty() ? std::chrono::milliseconds(0) : query.timeout, query.stop);
    if (!ready && !(range.empty() && ready.error().code == ErrorCode::would_block)) return std::unexpected(ready.error());
    if (query.stop.cancelled()) return std::unexpected(Error{ErrorCode::cancelled, {}});
    IOResponse response{query.buffer, {}, 0};
    if constexpr (reading) {
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
IOResult execute(const Channel& channel, const IOQuery& query) {
    return std::visit([&](const auto& request) -> IOResult {
        using T=std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, Read> || std::is_same_v<T, Write>) return transfer(channel, request);
        else if constexpr (std::is_same_v<T, Seek>) {
            auto position=channel.seek(request.offset, request.origin);
            if (!position) return std::unexpected(position.error());
            return IOResponse{{}, {}, *position};
        } else {
            auto result=std::is_same_v<T, Flush> ? channel.flush() : channel.close();
            if (!result) return std::unexpected(result.error());
            return IOResponse{};
        }
    }, query);
}
}
AsyncIO::AsyncIO(runtime::Runtime& runtime, Channel channel)
    : queue_(runtime), queries_(queue_, [channel=std::move(channel)](const IOQuery& query) {
        return execute(channel, query);
    }) {}
} // namespace ii::io
