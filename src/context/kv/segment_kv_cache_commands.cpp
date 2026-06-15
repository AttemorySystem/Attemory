#include "attemory-core/attemory-core.h"
#include "context/command/command_result.h"
#include "context/command/command_scope.h"
#include "context/kv/segment_kv_metadata.h"
#include "context/kv/segment_kv_manager.h"
#include "context/session/session_summary.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace attemory::context {

namespace {

using steady_clock = std::chrono::steady_clock;

int64_t elapsed_ms(const steady_clock::time_point & start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now() - start).count();
}

int64_t unix_time_ms() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()).count();
}

persistent::SegmentCacheManifest segment_kv_manifest_entry(
    const Segment & segment,
    const persistent::KVCacheEntry & cache) {
    persistent::SegmentCacheManifest manifest_segment;
    manifest_segment.segment_id = segment.segment_id;
    if (!segment.memory_indices.empty()) {
        manifest_segment.first_memory_idx = segment.memory_indices.front();
        manifest_segment.last_memory_idx_exclusive = segment.memory_indices.back() + 1;
    }
    manifest_segment.segment_content_hash = cache.key.segment_content_hash;
    manifest_segment.kv_file = cache.kv_path;
    manifest_segment.search_cache_file = cache.search_cache_path;
    manifest_segment.token_count = cache.token_count;
    manifest_segment.bytes = cache.bytes_on_disk;
    manifest_segment.created_at_ms = cache.created_at_ms;
    manifest_segment.last_access_ms = cache.last_access_ms;
    return manifest_segment;
}

} // namespace

static RuntimeOptions compact_cache_build_runtime(const RuntimeOptions & runtime) {
    RuntimeOptions compact = runtime;
    compact.n_ctx_is_ceiling = compact.n_ctx > 0;
    return compact;
}

static void warmup_native_search_if_needed(
    KVCacheCommandContext & context,
    kv::SegmentKVHandle & resident) {
    if (context.native_search_warmup_done ||
        resident == nullptr ||
        !context.command.runtime.use_native_engine ||
        context.command.runtime.backend == atmcore::BackendKind::CPU) {
        return;
    }

    const steady_clock::time_point start = steady_clock::now();
    std::string warmup_error;
    if (!atmcore_internal::warmup_native_search_attention(
            context.command.core,
            resident->active,
            warmup_error)) {
        if (context.command.run_log) {
            std::fprintf(stderr, "native-search-warmup skipped error=\"%s\"\n", warmup_error.c_str());
        }
        return;
    }

    context.native_search_warmup_done = true;
    if (context.command.run_log) {
        std::fprintf(stderr, "native-search-warmup done elapsed_ms=%lld\n", (long long) elapsed_ms(start));
    }
}

CommandResult kv::SegmentKVManager::index_session_kv_cache(
    KVCacheCommandContext context,
    const std::string & session_id) {
    SessionScope scope(context.command, *this, AttemoryCommand::INDEX, session_id);
    if (!scope.require_planned_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    if (!scope.persist_if_dirty()) {
        return scope.failure_result();
    }

    CommandResult result;
    if (session.segment_plan.segments.empty()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "session has no memory segments");
    }

    evict_session(session.store.session_id);
    const RuntimeOptions compact_runtime = compact_cache_build_runtime(context.command.runtime);

    for (Segment & segment : session.segment_plan.segments) {
        if (status(session, segment.segment_id).has_resident_snapshot) {
            continue;
        }

        kv::SegmentKVHandle resident;
        if (!prepare_resident_segment(
                session,
                compact_runtime,
                segment.segment_id,
                resident,
                result.error)) {
            return result;
        }
        warmup_native_search_if_needed(context, resident);

        segment.exact_tokens = (int32_t) kv::segment_kv_base_token_count(resident);
        segment.estimated_tokens = segment.exact_tokens;
    }

    result.ok = true;
    return result;
}

CommandResult kv::SegmentKVManager::save_session_kv_cache(
    KVCacheCommandContext context,
    const std::string & session_id) {
    SessionScope scope(context.command, *this, AttemoryCommand::SAVE_SESSION, session_id);
    if (!scope.require_planned_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    if (!scope.persist_if_dirty()) {
        return scope.failure_result();
    }

    CommandResult result;
    if (session.segment_plan.segments.empty()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "session has no memory segments");
    }

    const persistent::CacheLayout layout =
        cache_layout_for_session(context.command.cache_dir, context.command.model_key, session.store.session_id);
    std::vector<kv::SegmentKVHandle> residents;
    residents.reserve(session.segment_plan.segments.size());

    for (const Segment & segment : session.segment_plan.segments) {
        kv::SegmentKVHandle resident = kv_.resident.peek(session.store.session_id, segment.segment_id);
        if (resident == nullptr) {
            return make_error(
                ErrorCode::InvalidRequest,
                "segment KV is not resident: session=" +
                    session.store.session_id +
                    " segment=" +
                    std::to_string(segment.segment_id) +
                    "; run index before save",
                {
                    {"session_id", session.store.session_id},
                    {"segment_id", std::to_string(segment.segment_id)},
                });
        }
        residents.push_back(resident);
    }

    persistent::ResultStatus status = persistent::ensure_directory(persistent::cache_session_dir_path(layout));
    if (!result_to_bool(status, result.error)) {
        result.error_info = make_error_info(ErrorCode::InternalError, result.error);
        return result;
    }

    persistent::CacheManifest manifest;
    manifest.model = context.command.model_key;
    manifest.session_id = session.store.session_id;

    for (size_t i = 0; i < session.segment_plan.segments.size(); ++i) {
        const Segment & segment = session.segment_plan.segments[i];
        kv::SegmentKVHandle resident = residents[i];
        persistent::KVCacheEntry & cache = session.segment_kv_metadata[segment.segment_id];
        initialize_segment_kv_metadata(session, context.command.model_key, layout, segment, cache);
        cache.state = persistent::CacheState::Resident;
        const std::string kv_path = cache.kv_path;
        if (!kv::ensure_segment_kv_snapshot(resident, result.error)) {
            persistent::remove_file_if_exists(kv_path);
            result.error_info = make_error_info(ErrorCode::InternalError, result.error);
            return result;
        }
        if (!result_to_bool(persistent::save_kv_cache_state(cache, resident->snapshot), result.error)) {
            persistent::remove_file_if_exists(kv_path);
            result.error_info = make_error_info(ErrorCode::InternalError, result.error);
            return result;
        }

        cache.token_count = (int32_t) kv::segment_kv_base_token_count(resident);
        cache.created_at_ms = unix_time_ms();
        cache.last_access_ms = cache.created_at_ms;
        if (!persistent::get_kv_cache_state_size(cache, cache.bytes_on_disk).ok) {
            cache.bytes_on_disk = 0;
        }

        manifest.segments.push_back(segment_kv_manifest_entry(segment, cache));
    }

    result.ok = result_to_bool(persistent::save_kv_cache_manifest(layout, manifest), result.error);
    if (!result.ok) {
        result.error_info = make_error_info(ErrorCode::InternalError, result.error);
    }
    if (result.ok) {
        result.payload = ResultPayload::SaveSummary;
        result.summary = summarize_session(*this, &session);
    }
    return result;
}

} // namespace attemory::context
