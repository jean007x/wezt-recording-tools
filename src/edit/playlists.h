#pragma once
// Comping: build a playlist from pieces of two playlists. The part of `alt` between `from` and `to`
// replaces whatever `live` has there; everything outside stays. Clips are cut at the boundaries.
#include <vector>

#include "session/track_source.h"

namespace wezt::edit {

std::vector<session::Clip> compRange(const std::vector<session::Clip>& live, const std::vector<session::Clip>& alt, std::int64_t from, std::int64_t to);

}  // namespace wezt::edit
