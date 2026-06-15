#include "context/kv/segment_kv_metadata.h"

#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace attemory::context {
namespace {

void mark_segment_kv_missing_with_layout(
    Session & session,
    const persistent::ModelCacheKey & model_key,
    const persistent::CacheLayout & cache_layout,
    const Segment & segment) {
    persistent::KVCacheEntry & cache = session.segment_kv_metadata[segment.segment_id];
    initialize_segment_kv_metadata(session, model_key, cache_layout, segment, cache);
    cache.state = persistent::CacheState::Missing;
    cache.token_count = 0;
    cache.bytes_on_disk = 0;
    cache.bytes_in_memory = 0;
    cache.created_at_ms = 0;
    cache.last_access_ms = 0;
}

void refresh_segment_kv_metadata_with_layout(
    Session & session,
    const persistent::ModelCacheKey & model_key,
    const persistent::CacheLayout & cache_layout) {
    persistent::CacheManifest manifest;
    const persistent::ResultStatus manifest_status = persistent::load_kv_cache_manifest(cache_layout, manifest);

    std::unordered_map<int32_t, persistent::SegmentCacheManifest> manifest_by_segment;
    if (manifest_status.ok &&
        persistent::model_cache_key_equal(manifest.model, model_key) &&
        manifest.session_id == session.store.session_id) {
        for (const persistent::SegmentCacheManifest & segment : manifest.segments) {
            manifest_by_segment[segment.segment_id] = segment;
        }
    }

    for (const Segment & segment : session.segment_plan.segments) {
        persistent::KVCacheEntry & cache = session.segment_kv_metadata[segment.segment_id];
        initialize_segment_kv_metadata(session, model_key, cache_layout, segment, cache);

        const auto it = manifest_by_segment.find(segment.segment_id);
        persistent::KVCacheEntry disk_cache = cache;
        if (it != manifest_by_segment.end() && !it->second.kv_file.empty()) {
            disk_cache.kv_path = it->second.kv_file;
        }
        if (it == manifest_by_segment.end() ||
            it->second.segment_content_hash != cache.key.segment_content_hash ||
            !persistent::kv_cache_state_exists(disk_cache)) {
            if (cache.state != persistent::CacheState::Resident) {
                cache.state = persistent::CacheState::Missing;
            }
            continue;
        }

        cache.kv_path = it->second.kv_file.empty() ? cache.kv_path : it->second.kv_file;
        cache.search_cache_path =
            it->second.search_cache_file.empty() ? cache.search_cache_path : it->second.search_cache_file;
        cache.token_count = it->second.token_count;
        cache.bytes_on_disk = it->second.bytes;
        cache.created_at_ms = it->second.created_at_ms;
        cache.last_access_ms = it->second.last_access_ms;
        if (cache.state != persistent::CacheState::Resident) {
            cache.state = persistent::CacheState::DiskOnly;
        }
    }
}

} // namespace

std::string segment_kv_content_hash(const SessionFacts & state, const Segment & segment) {
    uint64_t hash = 1469598103934665603ull;
    auto update = [&](const void * data, size_t size) {
        const unsigned char * bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    auto update_string = [&](const std::string & value) {
        const uint64_t size = (uint64_t) value.size();
        update(&size, sizeof(size));
        update(value.data(), value.size());
    };

    update_string(state.system_text);
    for (persistent::MemoryIndex memory_idx : segment.memory_indices) {
        update(&memory_idx, sizeof(memory_idx));
        const persistent::MemoryRecord * memory = persistent::find_memory(state, memory_idx);
        if (memory != nullptr) {
            update_string(memory->text);
        }
    }

    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) hash);
    return buf;
}

void initialize_segment_kv_metadata(
    Session & session,
    const persistent::ModelCacheKey & model_key,
    const persistent::CacheLayout & cache_layout,
    const Segment & segment,
    persistent::KVCacheEntry & cache) {
    cache.key.model = model_key;
    cache.key.session_id = session.store.session_id;
    cache.key.segment_id = segment.segment_id;
    cache.key.segment_content_hash = segment_kv_content_hash(session.store, segment);
    cache.kv_path = persistent::cache_segment_kv_path(cache_layout, segment.segment_id).string();
    cache.search_cache_path =
        persistent::cache_segment_search_cache_path(cache_layout, segment.segment_id).string();
}

void mark_segment_kv_missing(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    const Segment & segment) {
    const persistent::CacheLayout layout =
        cache_layout_for_session(cache_dir, model_key, session.store.session_id);
    mark_segment_kv_missing_with_layout(session, model_key, layout, segment);
}

void refresh_session_kv_metadata(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session) {
    const persistent::CacheLayout layout =
        cache_layout_for_session(cache_dir, model_key, session.store.session_id);
    refresh_segment_kv_metadata_with_layout(session, model_key, layout);
}

void delete_session_kv_cache(
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    const std::string & session_id) {
    persistent::remove_kv_cache(cache_layout_for_session(cache_dir, model_key, session_id));
}

} // namespace attemory::context
