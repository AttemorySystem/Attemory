#pragma once

#include "context/context_types.h"
#include "context/kv/segment_kv_manager.h"
#include "context/session/session_state.h"

#include <filesystem>
#include <vector>

namespace attemory::context {

std::vector<SessionStatus> list_session_statuses(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    SessionMap & sessions,
    kv::SegmentKVManager & kv_manager);

} // namespace attemory::context
