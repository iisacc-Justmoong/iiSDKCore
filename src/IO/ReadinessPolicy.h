#pragma once
#include "Channel.h"
#include <optional>

namespace ii::io::detail {
// The currently adopted Windows pipe and serial handles are non-overlapped.
inline bool needs_explicit_polling(Capabilities caps) noexcept {
    return caps.backend==Backend::windows && caps.kind!=ChannelKind::file;
}
inline bool valid_polling_interval(std::optional<std::chrono::milliseconds> interval) noexcept {
    return !interval || *interval>std::chrono::milliseconds::zero();
}
}
