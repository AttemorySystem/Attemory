#pragma once

#include "context/session/session_state.h"

#include <filesystem>
#include <string>

namespace attemory::context {

std::string segment_kv_content_hash(const SessionFacts & facts, const Segment & segment);

void initialize_segment_kv_metadata(
    Session & session,
    const persistent::ModelCacheKey & model_key,
    const persistent::CacheLayout & cache_layout,
    const Segment & segment,
    persistent::KVCacheEntry & cache);

void mark_segment_kv_missing(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    const Segment & segment);
void refresh_session_kv_metadata(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session);
void delete_session_kv_cache(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    const std::string & session_id);

} // namespace attemory::context
