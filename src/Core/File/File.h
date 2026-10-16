#pragma once
#include "../Path/PathRoot.h"
#include "../../IO/Channel.h"

namespace ii::core {
// Named resource + owning channel reference; copying shares the channel cursor.
class File final {
public:
    template<class Open>
    [[nodiscard]] static io::Result<File> open(const PathRoot& root, const std::filesystem::path& relative,
                                              Open&& opener, io::FileOptions options={}) {
        auto path=root.resolve(relative);
        if (!path) return std::unexpected(io::Error{io::ErrorCode::invalid_argument,{}});
        auto channel=std::forward<Open>(opener)(*path,options);
        if (!channel) return std::unexpected(channel.error());
        return File(std::move(*path),std::move(*channel));
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const io::Channel& channel() const noexcept { return channel_; }
private:
    File(std::filesystem::path path, io::Channel channel) : path_(std::move(path)),channel_(std::move(channel)) {}
    std::filesystem::path path_;
    io::Channel channel_;
};
} // namespace ii::core
