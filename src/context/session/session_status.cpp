#include "context/session/session_status.h"

#include "context/kv/segment_kv_manager.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <algorithm>

namespace attemory::context {
namespace {

bool session_plan_ready(
    const persistent::ModelCacheKey & model_key,
    const Session & session) {
    return session.segment_plan.session_id == session.store.session_id &&
           session.segment_plan.config.model_cache_key == model_key.value;
}

int32_t count_valid_disk_cache_segments(
    const persistent::CacheManifest & manifest) {
    int32_t count = 0;
    for (const persistent::SegmentCacheManifest & segment : manifest.segments) {
        persistent::KVCacheEntry cache;
        cache.kv_path = segment.kv_file;
        if (persistent::kv_cache_state_exists(cache)) {
            ++count;
        }
    }
    return count;
}

int64_t total_manifest_tokens(
    const persistent::CacheManifest & manifest) {
    int64_t total = 0;
    for (const persistent::SegmentCacheManifest & segment : manifest.segments) {
        if (segment.token_count > 0) {
            total += segment.token_count;
        }
    }
    return total;
}

bool load_session_cache_manifest(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    const Session & session,
    persistent::CacheManifest & manifest) {
    const persistent::CacheLayout layout =
        cache_layout_for_session(cache_dir, model_key, session.store.session_id);
    const persistent::ResultStatus status = persistent::load_kv_cache_manifest(layout, manifest);
    return status.ok &&
           persistent::model_cache_key_equal(manifest.model, model_key) &&
           manifest.session_id == session.store.session_id;
}

void populate_planned_kv_status(
    kv::SegmentKVManager & kv_manager,
    const Session & session,
    SessionStatus & status) {
    for (const Segment & segment : session.segment_plan.segments) {
        if (segment.exact_tokens > 0) {
            status.total_tokens += segment.exact_tokens;
        } else if (segment.estimated_tokens > 0) {
            status.total_tokens += segment.estimated_tokens;
        }

        const kv::SegmentKVStatus kv_status =
            kv_manager.status(session, segment.segment_id);
        if (kv_status.has_resident_snapshot) {
            ++status.resident_segments;
        }
        if (kv_status.has_resident_snapshot || kv_status.has_disk_snapshot) {
            ++status.indexed_segments;
        }
        if (kv_status.has_disk_snapshot) {
            ++status.saved_segments;
        }
    }
}

SessionStatus build_session_status(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    kv::SegmentKVManager & kv_manager,
    Session & session) {
    SessionStatus status;
    status.session_id = session.store.session_id;
    status.memory_count = (int32_t) session.store.memories.size();
    status.facts_dirty = session.facts_dirty;
    status.kv_persist = session.store.kv_persist;
    status.plan_ready = session_plan_ready(model_key, session);

    persistent::CacheManifest manifest;
    const bool has_manifest = load_session_cache_manifest(cache_dir, model_key, session, manifest);
    const int32_t manifest_segments = has_manifest ? (int32_t) manifest.segments.size() : 0;

    status.segment_count =
        status.plan_ready ? (int32_t) session.segment_plan.segments.size() : manifest_segments;
    if (status.plan_ready) {
        populate_planned_kv_status(kv_manager, session, status);
    } else {
        status.total_tokens = has_manifest ? total_manifest_tokens(manifest) : 0;
        status.resident_segments = kv_manager.resident_segment_count(session.store.session_id);
        status.saved_segments = has_manifest ? count_valid_disk_cache_segments(manifest) : 0;
        status.indexed_segments = std::max(status.resident_segments, status.saved_segments);
    }
    status.indexed = status.segment_count > 0 && status.indexed_segments == status.segment_count;
    status.disk_cached = status.segment_count > 0 && status.saved_segments == status.segment_count;
    return status;
}

} // namespace

std::vector<SessionStatus> list_session_statuses(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    SessionMap & sessions,
    kv::SegmentKVManager & kv_manager) {
    std::vector<SessionStatus> statuses;
    statuses.reserve(sessions.size());
    for (auto & item : sessions) {
        statuses.push_back(build_session_status(cache_dir, model_key, kv_manager, item.second));
    }
    std::sort(
        statuses.begin(),
        statuses.end(),
        [](const SessionStatus & lhs, const SessionStatus & rhs) {
            return lhs.session_id < rhs.session_id;
        });
    return statuses;
}

} // namespace attemory::context
