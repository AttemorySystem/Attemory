#include "context/kv/segment_kv_manager.h"

#include "persistent/persistent.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace attemory::context::kv {
namespace {

void reset_active_segment(ActiveSegmentKV & active) {
    release_segment_kv_context(active.handle);
    active.handle.reset();
    active.session_id.clear();
    active.segment_id = -1;
}

SegmentKVHandle take_incremental_prefix(
    ContextKVState & kv,
    const std::string & session_id,
    int32_t segment_id) {
    const auto session_it = kv.incremental_prefixes.find(session_id);
    if (session_it == kv.incremental_prefixes.end()) {
        return nullptr;
    }

    auto & segments = session_it->second;
    const auto segment_it = segments.find(segment_id);
    if (segment_it == segments.end()) {
        return nullptr;
    }

    SegmentKVHandle handle = segment_it->second.handle;
    segments.erase(segment_it);
    if (segments.empty()) {
        kv.incremental_prefixes.erase(session_it);
    }
    return handle;
}

atmcore::RuntimeOptions exact_context_runtime(
    const atmcore::RuntimeOptions & runtime,
    int32_t required_n_ctx) {
    atmcore::RuntimeOptions compact = runtime;
    compact.n_ctx = required_n_ctx;
    compact.n_ctx_is_ceiling = false;
    return compact;
}

atmcore::RuntimeOptions compact_segment_build_runtime(
    const atmcore::RuntimeOptions & runtime) {
    atmcore::RuntimeOptions compact = runtime;
    compact.n_ctx_is_ceiling = compact.n_ctx > 0;
    return compact;
}

const char * bool_text(bool value) {
    return value ? "true" : "false";
}

int32_t live_context_length(const SegmentKVHandle & handle) {
    return segment_kv_has_live_context(handle)
        ? atmcore::active_kv_context_length(handle->active)
        : 0;
}

class SnapshotRuntimeOverride {
public:
    SnapshotRuntimeOverride(atmcore::KVSnapshot & snapshot, const atmcore::RuntimeOptions & runtime) :
        snapshot_(snapshot),
        saved_runtime_(snapshot.metadata.runtime) {
        snapshot_.metadata.runtime = runtime;
    }

    SnapshotRuntimeOverride(const SnapshotRuntimeOverride &) = delete;
    SnapshotRuntimeOverride & operator=(const SnapshotRuntimeOverride &) = delete;

    ~SnapshotRuntimeOverride() {
        snapshot_.metadata.runtime = saved_runtime_;
    }

private:
    atmcore::KVSnapshot & snapshot_;
    atmcore::RuntimeOptions saved_runtime_;
};

void clear_incremental_prefix_session(
    ContextKVState & kv,
    const std::string & session_id) {
    kv.incremental_prefixes.erase(session_id);
}

void remove_incremental_prefix(
    ContextKVState & kv,
    const std::string & session_id,
    int32_t segment_id) {
    const auto session_it = kv.incremental_prefixes.find(session_id);
    if (session_it == kv.incremental_prefixes.end()) {
        return;
    }

    auto & segments = session_it->second;
    segments.erase(segment_id);
    if (segments.empty()) {
        kv.incremental_prefixes.erase(session_it);
    }
}

bool release_incremental_prefix_live_contexts(
    ContextKVState & kv,
    std::string & error) {
    for (auto & session_item : kv.incremental_prefixes) {
        for (auto & segment_item : session_item.second) {
            SegmentKVHandle & handle = segment_item.second.handle;
            if (!segment_kv_has_live_context(handle)) {
                continue;
            }
            if (!ensure_segment_kv_snapshot(handle, error)) {
                return false;
            }
            release_segment_kv_context(handle);
        }
    }
    return true;
}

bool release_incremental_prefix_live_contexts_except(
    ContextKVState & kv,
    const std::string & protected_session_id,
    int32_t protected_segment_id,
    std::string & error) {
    for (auto & session_item : kv.incremental_prefixes) {
        for (auto & segment_item : session_item.second) {
            if (session_item.first == protected_session_id &&
                segment_item.first == protected_segment_id) {
                continue;
            }

            SegmentKVHandle & handle = segment_item.second.handle;
            if (!segment_kv_has_live_context(handle)) {
                continue;
            }
            if (!ensure_segment_kv_snapshot(handle, error)) {
                return false;
            }
            release_segment_kv_context(handle);
        }
    }
    return true;
}

} // namespace

SegmentKVManager::SegmentKVManager(
    atmcore::Runtime *& core,
    ContextKVState & kv) :
    core_(core),
    kv_(kv) {
}

SegmentKVStatus SegmentKVManager::status(
    const attemory::context::Session & session,
    attemory::persistent::SegmentId segment_id,
    const ActiveSegmentKV * active) const {
    SegmentKVStatus status;
    status.segment_id = segment_id;

    const attemory::persistent::KVCacheEntry * cache =
        attemory::context::find_segment_kv_metadata(session, segment_id);
    if (cache != nullptr) {
        status.has_disk_snapshot =
            !cache->kv_path.empty() &&
            (cache->state == attemory::persistent::CacheState::DiskOnly ||
             cache->state == attemory::persistent::CacheState::Resident) &&
            attemory::persistent::kv_cache_state_exists(*cache);
        status.token_count = cache->token_count;
        status.bytes_on_disk = cache->bytes_on_disk;
    }

    SegmentKVHandle resident = kv_.resident.peek(session.store.session_id, segment_id);
    if (resident != nullptr) {
        status.has_resident_snapshot = true;
        status.has_resident_state_blob = segment_kv_has_snapshot_blob(resident);
        status.has_live_context = segment_kv_has_live_context(resident);
        status.token_count = (int32_t) segment_kv_base_token_count(resident);
    }

    status.is_active =
        (active == nullptr ? &kv_.active_segment : active)->handle != nullptr &&
        (active == nullptr ? &kv_.active_segment : active)->session_id == session.store.session_id &&
        (active == nullptr ? &kv_.active_segment : active)->segment_id == segment_id;
    return status;
}

bool SegmentKVManager::prepare_resident_segment(
    attemory::context::Session & session,
    const atmcore::RuntimeOptions & runtime,
    attemory::persistent::SegmentId segment_id,
    SegmentKVPrepareMode mode,
    SegmentKVHandle & resident,
    std::string & error) {
    error.clear();

    const attemory::context::Segment * segment =
        attemory::context::find_segment(session.segment_plan, segment_id);
    if (segment == nullptr) {
        error = "segment not found";
        return false;
    }

    return prepare_resident_segment(session, runtime, *segment, mode, resident, error);
}

void SegmentKVManager::clear_active() {
    reset_active_segment(kv_.active_segment);
}

void SegmentKVManager::evict_active_for_command(AttemoryCommand command, const std::string & session_id) {
    if (command == AttemoryCommand::CREATE_SESSION ||
        kv_.active_segment.handle == nullptr ||
        kv_.active_segment.session_id.empty() ||
        kv_.active_segment.session_id == session_id) {
        return;
    }

    clear_active();
}

void SegmentKVManager::evict_session(const std::string & session_id) {
    if (kv_.active_segment.session_id == session_id) {
        clear_active();
    }
}

void SegmentKVManager::clear_resident_session(const std::string & session_id) {
    kv_.resident.clear_session(session_id);
    clear_incremental_prefix_session(kv_, session_id);
}

void SegmentKVManager::remove_resident_segment(const std::string & session_id, int32_t segment_id) {
    kv_.resident.remove(session_id, segment_id);
    remove_incremental_prefix(kv_, session_id, segment_id);
}

void SegmentKVManager::stage_incremental_prefix(const std::string & session_id, int32_t segment_id) {
    SegmentKVHandle resident = kv_.resident.find(session_id, segment_id);
    if (resident == nullptr) {
        return;
    }

    ResidentKVEntry entry;
    entry.handle = resident;
    kv_.incremental_prefixes[session_id][segment_id] = entry;
    kv_.resident.remove(session_id, segment_id);
    if (kv_.active_segment.session_id == session_id &&
        kv_.active_segment.segment_id == segment_id) {
        reset_active_segment(kv_.active_segment);
    }
}

int32_t SegmentKVManager::resident_segment_count(const std::string & session_id) const {
    return kv_.resident.count_session(session_id);
}

bool SegmentKVManager::release_all_live_contexts(std::string & error) {
    clear_active();
    return release_incremental_prefix_live_contexts(kv_, error) &&
        kv_.resident.release_all_live_contexts(error);
}

bool SegmentKVManager::keep_only_session_live(const std::string & session_id, std::string & error) {
    return kv_.resident.keep_only_session_live(session_id, error);
}

void SegmentKVManager::sync_active_search_cache_to_resident() {
    const ActiveSegmentKV & active = kv_.active_segment;
    if (active.handle == nullptr ||
        active.session_id.empty() ||
        active.segment_id < 0 ||
        active.handle->active.metadata.base_search_cache_blob.empty()) {
        return;
    }

    SegmentKVHandle resident = kv_.resident.peek(active.session_id, active.segment_id);
    if (resident == nullptr) {
        return;
    }

    resident->active.metadata.base_search_cache_blob = active.handle->active.metadata.base_search_cache_blob;
    if (resident->has_snapshot) {
        resident->snapshot.metadata.base_search_cache_blob = active.handle->active.metadata.base_search_cache_blob;
    }
}

void SegmentKVManager::clear_all() {
    clear_active();
    kv_.resident.clear();
    kv_.incremental_prefixes.clear();
}

void SegmentKVManager::set_resident_budget_bytes(uint64_t bytes) {
    kv_.resident.set_budget_bytes(bytes);
}

uint64_t SegmentKVManager::resident_budget_bytes() const {
    return kv_.resident.budget_bytes();
}

uint64_t SegmentKVManager::resident_bytes() const {
    return kv_.resident.resident_bytes();
}

bool SegmentKVManager::activate_for_search(
    attemory::context::Session & session,
    const atmcore::RuntimeOptions & runtime,
    attemory::persistent::SegmentId segment_id,
    size_t query_stage_token_count,
    bool run_log,
    std::string & error) {
    error.clear();

    const attemory::context::Segment * segment =
        attemory::context::find_segment(session.segment_plan, segment_id);
    if (segment == nullptr) {
        error = "segment not found";
        return false;
    }

    SegmentKVHandle resident;
    if (!prepare_resident_segment(
            session,
            runtime,
            *segment,
            SegmentKVPrepareMode::RequireExisting,
            resident,
            error)) {
        return false;
    }
    if (resident == nullptr) {
        error = "resident segment snapshot is unavailable";
        return false;
    }

    const atmcore::KVMetadata * resident_metadata = segment_kv_metadata(resident);
    if (resident_metadata == nullptr) {
        error = "resident segment KV metadata is unavailable";
        return false;
    }

    const size_t total_tokens_required = resident_metadata->base_tokens.size() + query_stage_token_count;
    const int32_t required_n_ctx =
        atmcore::required_context_for_token_count(total_tokens_required);

    ActiveSegmentKV & active = kv_.active_segment;
    if (run_log) {
        std::fprintf(
            stderr,
            "search-activate begin session=%s segment=%d base_tokens=%zu query_stage_tokens=%zu "
            "total_tokens_required=%zu required_n_ctx=%d active_session=%s active_segment=%d "
            "active_live=%s active_ctx_len=%d resident_live=%s resident_ctx_len=%d "
            "resident_snapshot=%s resident_blob=%s\n",
            session.store.session_id.c_str(),
            segment_id,
            resident_metadata->base_tokens.size(),
            query_stage_token_count,
            total_tokens_required,
            required_n_ctx,
            active.session_id.c_str(),
            active.segment_id,
            bool_text(segment_kv_has_live_context(active.handle)),
            live_context_length(active.handle),
            bool_text(segment_kv_has_live_context(resident)),
            live_context_length(resident),
            bool_text(resident->has_snapshot),
            bool_text(segment_kv_has_snapshot_blob(resident)));
        std::fflush(stderr);
    }
    if (active.handle != nullptr &&
        active.session_id == session.store.session_id &&
        active.segment_id == segment_id &&
        atmcore::active_kv_context_length(active.handle->active) >= required_n_ctx) {
        if (run_log) {
            std::fprintf(
                stderr,
                "search-activate reuse-active-current session=%s segment=%d active_ctx_len=%d\n",
                session.store.session_id.c_str(),
                segment_id,
                live_context_length(active.handle));
            std::fflush(stderr);
        }
        return true;
    }

    if (segment_kv_has_live_context(resident) &&
        atmcore::active_kv_context_length(resident->active) >= required_n_ctx) {
        if (run_log) {
            std::fprintf(
                stderr,
                "search-activate reuse-resident-live session=%s segment=%d resident_ctx_len=%d\n",
                session.store.session_id.c_str(),
                segment_id,
                live_context_length(resident));
            std::fflush(stderr);
        }
        active.handle = resident;
        active.session_id = session.store.session_id;
        active.segment_id = segment_id;
        return true;
    }

    if (run_log && active.handle != nullptr) {
        std::fprintf(
            stderr,
            "search-activate reset-active session=%s segment=%d previous_session=%s "
            "previous_segment=%d previous_live=%s previous_ctx_len=%d\n",
            session.store.session_id.c_str(),
            segment_id,
            active.session_id.c_str(),
            active.segment_id,
            bool_text(segment_kv_has_live_context(active.handle)),
            live_context_length(active.handle));
        std::fflush(stderr);
    }
    reset_active_segment(active);

    if (!segment_kv_has_snapshot_blob(resident)) {
        if (!runtime.use_native_engine) {
            error = "resident KV snapshot is missing serialized state";
            return false;
        }

        if (run_log) {
            std::fprintf(
                stderr,
                "search-activate rebuild-ephemeral begin session=%s segment=%d required_n_ctx=%d\n",
                session.store.session_id.c_str(),
                segment_id,
                required_n_ctx);
            std::fflush(stderr);
        }
        const std::vector<std::string> memories =
            attemory::context::collect_segment_memory_texts(session.store, *segment);
        atmcore::RuntimeOptions expanded_runtime = runtime;
        expanded_runtime.n_ctx = required_n_ctx;
        expanded_runtime.n_ctx_is_ceiling = false;
        SegmentKVHandle rebuilt = make_segment_kv_handle();
        if (!atmcore::build_active_kv_ephemeral(
                core_,
                expanded_runtime,
                session.store.system_text,
                memories,
                rebuilt->active,
                error)) {
            return false;
        }
        if (run_log) {
            std::fprintf(
                stderr,
                "search-activate rebuild-ephemeral done session=%s segment=%d active_ctx_len=%d\n",
                session.store.session_id.c_str(),
                segment_id,
                live_context_length(rebuilt));
            std::fflush(stderr);
        }
        if (!kv_.resident.store(session.store.session_id, segment_id, rebuilt, error)) {
            return false;
        }
        if (!keep_only_segment_live(runtime, session.store.session_id, segment_id, error)) {
            return false;
        }
        active.handle = rebuilt;
        active.session_id = session.store.session_id;
        active.segment_id = segment_id;
        return true;
    }

    SegmentKVHandle hydrated = make_segment_kv_handle();
    const atmcore::RuntimeOptions hydrate_runtime =
        exact_context_runtime(runtime, required_n_ctx);
    if (run_log) {
        std::fprintf(
            stderr,
            "search-activate hydrate-snapshot begin session=%s segment=%d required_n_ctx=%d "
            "hydrate_n_ctx=%d snapshot_tokens=%zu snapshot_blob_bytes=%zu\n",
            session.store.session_id.c_str(),
            segment_id,
            required_n_ctx,
            hydrate_runtime.n_ctx,
            resident->snapshot.metadata.base_tokens.size(),
            resident->snapshot.base_seq_state_blob.size());
        std::fflush(stderr);
    }
    SnapshotRuntimeOverride snapshot_runtime(resident->snapshot, hydrate_runtime);
    if (!atmcore::load_active_kv_from_snapshot(
            core_,
            hydrate_runtime,
            resident->snapshot,
            total_tokens_required,
            hydrated->active,
            error)) {
        if (run_log) {
            std::fprintf(
                stderr,
                "search-activate hydrate-snapshot failed session=%s segment=%d error=\"%s\"\n",
                session.store.session_id.c_str(),
                segment_id,
                error.c_str());
            std::fflush(stderr);
        }
        return false;
    }
    if (run_log) {
        std::fprintf(
            stderr,
            "search-activate hydrate-snapshot done session=%s segment=%d active_ctx_len=%d\n",
            session.store.session_id.c_str(),
            segment_id,
            live_context_length(hydrated));
        std::fflush(stderr);
    }

    active.handle = hydrated;
    active.session_id = session.store.session_id;
    active.segment_id = segment_id;
    return true;
}

bool SegmentKVManager::try_prepare_incremental_resident_segment(
    attemory::context::Session & session,
    const atmcore::RuntimeOptions & runtime,
    const attemory::context::Segment & segment,
    const std::vector<std::string> & memories,
    SegmentKVHandle & resident,
    std::string & error) {
    resident = nullptr;
    error.clear();
    if (runtime.use_native_engine) {
        return false;
    }

    SegmentKVHandle prefix = take_incremental_prefix(kv_, session.store.session_id, segment.segment_id);
    if (prefix == nullptr) {
        return false;
    }

    const atmcore::KVMetadata * prefix_metadata = segment_kv_metadata(prefix);
    if (prefix_metadata == nullptr ||
        prefix_metadata->system != session.store.system_text ||
        prefix_metadata->memories.empty() ||
        prefix_metadata->memories.size() >= memories.size()) {
        return false;
    }

    const size_t prefix_memory_count = prefix_metadata->memories.size();
    for (size_t i = 0; i < prefix_memory_count; ++i) {
        if (prefix_metadata->memories[i] != memories[i]) {
            return false;
        }
    }

    int32_t token_count = 0;
    std::string local_error;
    if (!atmcore::estimate_memory_kv_token_count(
            core_,
            runtime,
            session.store.system_text,
            memories,
            token_count,
            local_error)) {
        return false;
    }
    if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, local_error)) {
        return false;
    }

    const int32_t required_n_ctx =
        atmcore::required_context_for_token_count(static_cast<size_t>(token_count));
    SegmentKVHandle extended;
    if (segment_kv_has_live_context(prefix) &&
        atmcore::active_kv_context_length(prefix->active) >= required_n_ctx) {
        extended = prefix;
    } else {
        if (!ensure_segment_kv_snapshot(prefix, local_error)) {
            return false;
        }
        release_segment_kv_context(prefix);

        extended = make_segment_kv_handle();
        extended->snapshot = prefix->snapshot;
        extended->has_snapshot = true;
        const atmcore::RuntimeOptions hydrate_runtime =
            exact_context_runtime(runtime, required_n_ctx);
        SnapshotRuntimeOverride snapshot_runtime(extended->snapshot, hydrate_runtime);
        if (!atmcore::load_active_kv_from_snapshot(
                core_,
                hydrate_runtime,
                extended->snapshot,
                static_cast<size_t>(token_count),
                extended->active,
                local_error)) {
            return false;
        }
    }

    const std::vector<std::string> appended_memories(
        memories.begin() + static_cast<std::ptrdiff_t>(prefix_memory_count),
        memories.end());
    if (!atmcore::extend_active_kv(
            core_,
            runtime,
            extended->active,
            appended_memories,
            local_error)) {
        release_segment_kv_context(extended);
        return false;
    }

    const int32_t updated_token_count = (int32_t) extended->active.metadata.base_tokens.size();
    if (!ensure_segment_kv_snapshot(extended, local_error)) {
        release_segment_kv_context(extended);
        return false;
    }

    if (!kv_.resident.store(session.store.session_id, segment.segment_id, extended, local_error)) {
        return false;
    }
    if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, local_error)) {
        return false;
    }

    attemory::persistent::KVCacheEntry & updated_cache =
        session.segment_kv_metadata[segment.segment_id];
    updated_cache.state = attemory::persistent::CacheState::Resident;
    updated_cache.token_count = updated_token_count;
    resident = extended;
    kv_.active_segment.handle = extended;
    kv_.active_segment.session_id = session.store.session_id;
    kv_.active_segment.segment_id = segment.segment_id;
    return true;
}

bool SegmentKVManager::prepare_resident_segment(
    attemory::context::Session & session,
    const atmcore::RuntimeOptions & runtime,
    const attemory::context::Segment & segment,
    SegmentKVPrepareMode mode,
    SegmentKVHandle & resident,
    std::string & error) {
    error.clear();

    resident = kv_.resident.find(session.store.session_id, segment.segment_id);
    if (resident != nullptr) {
        if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, error)) {
            return false;
        }
        if (segment_kv_has_live_context(resident)) {
            kv_.active_segment.handle = resident;
            kv_.active_segment.session_id = session.store.session_id;
            kv_.active_segment.segment_id = segment.segment_id;
        }
        return true;
    }

    const std::vector<std::string> memories =
        attemory::context::collect_segment_memory_texts(session.store, segment);

    if (try_prepare_incremental_resident_segment(
            session,
            runtime,
            segment,
            memories,
            resident,
            error)) {
        return true;
    }
    error.clear();

    attemory::persistent::KVCacheEntry * cache =
        attemory::context::find_segment_kv_metadata(session, segment.segment_id);
    if (cache != nullptr &&
        !cache->kv_path.empty() &&
        (mode == SegmentKVPrepareMode::AllowBuild ||
         cache->state == attemory::persistent::CacheState::DiskOnly ||
         cache->state == attemory::persistent::CacheState::Resident) &&
        attemory::persistent::kv_cache_state_exists(*cache)) {
        SegmentKVHandle loaded = make_segment_kv_handle();
        attemory::persistent::KVCacheStateLoadRequest request;
        request.cache = *cache;
        request.system_text = session.store.system_text;
        request.memories = memories;
        const atmcore::RuntimeOptions cache_runtime =
            compact_segment_build_runtime(runtime);
        const attemory::persistent::ResultStatus status =
            attemory::persistent::load_kv_cache_state_snapshot(
                core_,
                cache_runtime,
                request,
                loaded->snapshot);
        if (status.ok) {
            loaded->has_snapshot = true;
            if (!kv_.resident.store(session.store.session_id, segment.segment_id, loaded, error)) {
                return false;
            }
            if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, error)) {
                return false;
            }
            cache->state = attemory::persistent::CacheState::Resident;
            cache->token_count = (int32_t) segment_kv_base_token_count(loaded);
            resident = loaded;
            return true;
        }
        cache->state = attemory::persistent::CacheState::Missing;
        if (mode == SegmentKVPrepareMode::RequireExisting) {
            error =
                "failed to load segment KV disk cache: session=" +
                session.store.session_id +
                " segment=" +
                std::to_string(segment.segment_id) +
                " reason=" +
                status.error;
            return false;
        }
        error.clear();
    }

    if (mode == SegmentKVPrepareMode::RequireExisting) {
        error =
            "segment KV is not indexed: session=" +
            session.store.session_id +
            " segment=" +
            std::to_string(segment.segment_id) +
            "; run index or save-session before search";
        return false;
    }

    SegmentKVHandle rebuilt = make_segment_kv_handle();
    if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, error)) {
        return false;
    }
    const atmcore::RuntimeOptions build_runtime = compact_segment_build_runtime(runtime);
    const bool keep_ephemeral = runtime.use_native_engine;
    const bool built = keep_ephemeral
        ? atmcore::build_active_kv_ephemeral(
              core_,
              build_runtime,
              session.store.system_text,
              memories,
              rebuilt->active,
              error)
        : atmcore::build_active_kv(
              core_,
              build_runtime,
              session.store.system_text,
              memories,
              rebuilt->active,
              error);
    if (!built) {
        return false;
    }

    const int32_t token_count = (int32_t) rebuilt->active.metadata.base_tokens.size();
    if (!runtime.use_native_engine) {
        if (!ensure_segment_kv_snapshot(rebuilt, error)) {
            return false;
        }
    }
    if (!kv_.resident.store(session.store.session_id, segment.segment_id, rebuilt, error)) {
        return false;
    }
    if (!keep_only_segment_live(runtime, session.store.session_id, segment.segment_id, error)) {
        return false;
    }
    attemory::persistent::KVCacheEntry & updated_cache = session.segment_kv_metadata[segment.segment_id];
    updated_cache.state = attemory::persistent::CacheState::Resident;
    updated_cache.token_count = token_count;
    resident = rebuilt;
    if (segment_kv_has_live_context(rebuilt)) {
        kv_.active_segment.handle = rebuilt;
        kv_.active_segment.session_id = session.store.session_id;
        kv_.active_segment.segment_id = segment.segment_id;
    }
    return true;
}

bool SegmentKVManager::keep_only_segment_live(
    const atmcore::RuntimeOptions & runtime,
    const std::string & session_id,
    int32_t segment_id,
    std::string & error) {
    (void) runtime;
    error.clear();
    if (!kv_.resident.keep_only_segment_live(session_id, segment_id, error)) {
        return false;
    }
    if (!release_incremental_prefix_live_contexts_except(kv_, session_id, segment_id, error)) {
        return false;
    }
    if (kv_.active_segment.handle != nullptr &&
        (kv_.active_segment.session_id != session_id ||
         kv_.active_segment.segment_id != segment_id)) {
        reset_active_segment(kv_.active_segment);
    }
    return true;
}

} // namespace attemory::context::kv
