#pragma once

#include "persistent/persistent.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace attemory::context {

struct Segment {
    attemory::persistent::SegmentId segment_id = attemory::persistent::kInvalidSegmentId;
    std::vector<attemory::persistent::MemoryIndex> memory_indices;

    int32_t estimated_tokens = 0;
    int32_t exact_tokens = 0;

    bool manual_start = false;
    bool auto_split_start = false;
};

struct SegmentPlanConfig {
    std::string model_cache_key;
    int32_t ctx_limit = 0;
    float split_ratio = 0.85f;
    int32_t max_segments = 100;
};

struct SegmentPlan {
    std::string session_id;
    SegmentPlanConfig config;
    std::vector<Segment> segments;
};

struct Session {
    attemory::persistent::SessionStore store;
    SegmentPlan segment_plan;
    std::unordered_map<attemory::persistent::SegmentId, attemory::persistent::KVCacheEntry> segment_kv_metadata;
    bool facts_dirty = false;

    int64_t last_access_ms = 0;
};

using SessionFacts = attemory::persistent::SessionStore;
using SessionMap = std::unordered_map<std::string, Session>;

inline const Segment * find_segment(
    const SegmentPlan & plan,
    attemory::persistent::SegmentId segment_id) {
    for (const Segment & segment : plan.segments) {
        if (segment.segment_id == segment_id) {
            return &segment;
        }
    }

    return nullptr;
}

inline std::vector<std::string> collect_segment_memory_texts(
    const attemory::persistent::SessionStore & store,
    const Segment & segment) {
    std::vector<std::string> memories;
    memories.reserve(segment.memory_indices.size());
    for (attemory::persistent::MemoryIndex memory_idx : segment.memory_indices) {
        const attemory::persistent::MemoryRecord * memory =
            attemory::persistent::find_memory(store, memory_idx);
        if (memory != nullptr) {
            memories.push_back(memory->text);
        }
    }
    return memories;
}

inline attemory::persistent::KVCacheEntry * find_segment_kv_metadata(
    Session & session,
    attemory::persistent::SegmentId segment_id) {
    const auto it = session.segment_kv_metadata.find(segment_id);
    return it == session.segment_kv_metadata.end() ? nullptr : &it->second;
}

inline const attemory::persistent::KVCacheEntry * find_segment_kv_metadata(
    const Session & session,
    attemory::persistent::SegmentId segment_id) {
    const auto it = session.segment_kv_metadata.find(segment_id);
    return it == session.segment_kv_metadata.end() ? nullptr : &it->second;
}

inline void clear_session_resident_kv_metadata(Session & session) {
    for (auto & item : session.segment_kv_metadata) {
        attemory::persistent::KVCacheEntry & cache = item.second;
        if (cache.state == attemory::persistent::CacheState::Resident) {
            cache.state = attemory::persistent::CacheState::Missing;
        }
    }
}

} // namespace attemory::context
