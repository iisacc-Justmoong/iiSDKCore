#pragma once
#include "Plugin.h"
#include <span>

namespace ii::plugin::detail {
// Defined in the application-specific generated Core archive.
std::span<const Entry> embedded_entries() noexcept;
}
