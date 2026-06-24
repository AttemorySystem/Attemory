#pragma once

#include "attemory-core/attemory-core.h"
#include "context/context_types.h"
#include "context/session/session_state.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace attemory::context {

inline constexpr int32_t kMaxSegmentsPerSession = 100;
inline constexpr int32_t kSegmentSoftLimitPercent = 90;

int32_t segment_soft_limit(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime);

bool initialize_session_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool run_log,
    std::string & error);

bool prepare_session_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool run_log,
    std::string & error);

bool rebuild_segment_plan_fast_for_oneshot(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    std::string & error);

} // namespace attemory::context
