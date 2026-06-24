#include "context/session/session_manager.h"

#include "context/kv/segment_kv_metadata.h"
#include "context/command/command_result.h"
#include "context/command/command_scope.h"
#include "context/context_state.h"
#include "context/kv/segment_kv_manager.h"
#include "context/search/search_service.h"
#include "context/session/segment_planner.h"
#include "context/session/session_lookup.h"
#include "context/session/session_summary.h"
#include "context/session/session_status.h"
#include "context/storage/model_cache_key.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

namespace attemory::context {

struct SessionManager::Impl {
    ContextState state;
    kv::SegmentKVManager kv_manager;

    Impl() :
        kv_manager(state.core, state.kv) {
    }

    SessionCommandContext command_context() {
        return {
            state.sessions,
            state.kv,
            state.core,
            state.options.runtime,
            state.data_dir,
            state.cache_dir,
            state.model_cache_key,
            state.options.run_log,
        };
    }

    SearchRuntimeContext search_context() {
        return {
            command_context(),
        };
    }
};

namespace {

inline constexpr int32_t kAppendTokenEstimateSafetyMargin = 4;

void reset_session_derived_state(kv::SegmentKVManager & kv_manager, Session & session) {
    kv_manager.evict_session(session.store.session_id);
    kv_manager.clear_resident_session(session.store.session_id);
    session.segment_plan = {};
    session.segment_kv_metadata.clear();
}

void evict_session_runtime_state(kv::SegmentKVManager & kv_manager, Session & session) {
    kv_manager.evict_session(session.store.session_id);
    kv_manager.clear_resident_session(session.store.session_id);
}

void mark_resident_segment_kv_metadata_cleared(Session & session) {
    for (auto & item : session.segment_kv_metadata) {
        if (item.second.state != persistent::CacheState::Resident) {
            continue;
        }

        const bool has_disk_cache =
            persistent::kv_cache_state_exists(item.second);
        item.second.state = has_disk_cache ?
            persistent::CacheState::DiskOnly :
            persistent::CacheState::Missing;
    }
}

bool estimate_segment_token_count_with_extra_memory(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const SessionFacts & facts,
    const Segment & segment,
    const std::string & extra_memory,
    int32_t & token_count,
    std::string & error) {
    std::vector<std::string> memories = collect_segment_memory_texts(facts, segment);
    memories.push_back(extra_memory);
    return atmcore::estimate_memory_kv_token_count(core, runtime, facts.system_text, memories, token_count, error);
}

bool estimate_new_segment_token_count(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const SessionFacts & facts,
    const std::string & memory,
    int32_t & token_count,
    std::string & error) {
    const std::vector<std::string> memories = { memory };
    return atmcore::estimate_memory_kv_token_count(core, runtime, facts.system_text, memories, token_count, error);
}

bool estimate_empty_segment_token_count(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const SessionFacts & facts,
    int32_t & token_count,
    std::string & error) {
    const std::vector<std::string> memories;
    return atmcore::estimate_memory_kv_token_count(core, runtime, facts.system_text, memories, token_count, error);
}

bool estimate_memory_text_tokens(
    atmcore::Runtime * core,
    const std::string & text,
    int32_t & token_count,
    std::string & error) {
    return atmcore::estimate_text_token_count_fast(core, text, false, token_count, error);
}

int32_t append_token_estimate(
    int32_t current_segment_tokens,
    int32_t empty_segment_tokens,
    int32_t single_memory_segment_tokens) {
    const int64_t memory_delta =
        std::max<int32_t>(1, single_memory_segment_tokens - empty_segment_tokens) +
        kAppendTokenEstimateSafetyMargin;
    const int64_t total =
        static_cast<int64_t>(current_segment_tokens) + memory_delta;
    return total > std::numeric_limits<int32_t>::max()
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(total);
}

bool append_memory_and_update_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    const MemoryInput & memory,
    int32_t & token_count,
    int32_t & segment_id,
    bool & persist_session_facts,
    std::string & error) {
    token_count = -1;
    segment_id = kNoSegmentId;
    persist_session_facts = false;
    error.clear();

    const int32_t soft_limit = segment_soft_limit(core, runtime);
    if (soft_limit <= 0) {
        error = "failed to resolve effective context length";
        return false;
    }

    Segment * last_segment =
        session.segment_plan.segments.empty() ? nullptr : &session.segment_plan.segments.back();

    bool need_new_segment = false;
    bool manual_start = false;
    bool auto_start = false;
    int32_t candidate_existing_tokens = -1;

    const bool starts_manual_segment =
        std::find(
            session.store.manual_segment_boundaries.begin(),
            session.store.manual_segment_boundaries.end(),
            session.store.next_memory_idx) != session.store.manual_segment_boundaries.end();

    int32_t new_segment_tokens = 0;
    if (!estimate_new_segment_token_count(
            core,
            runtime,
            session.store,
            memory.text,
            new_segment_tokens,
            error)) {
        return false;
    }

    if (new_segment_tokens > soft_limit) {
        error = "memory is too large for a fresh segment";
        return false;
    }

    bool candidate_is_exact = false;
    if (last_segment == nullptr || starts_manual_segment) {
        need_new_segment = true;
        manual_start = starts_manual_segment;
    } else {
        int32_t empty_segment_tokens = 0;
        if (!estimate_empty_segment_token_count(
                core, runtime, session.store, empty_segment_tokens, error)) {
            return false;
        }

        candidate_existing_tokens =
            append_token_estimate(
                last_segment->estimated_tokens,
                empty_segment_tokens,
                new_segment_tokens);

        if (candidate_existing_tokens > soft_limit) {
            if (!estimate_segment_token_count_with_extra_memory(
                    core,
                    runtime,
                    session.store,
                    *last_segment,
                    memory.text,
                    candidate_existing_tokens,
                    error)) {
                return false;
            }
            candidate_is_exact = true;
            if (candidate_existing_tokens > soft_limit) {
                need_new_segment = true;
                auto_start = true;
            }
        }
    }

    if (need_new_segment) {
        if ((int32_t) session.segment_plan.segments.size() >= kMaxSegmentsPerSession) {
            error = "segment limit exceeded";
            return false;
        }

        Segment new_segment;
        new_segment.segment_id = (int32_t) session.segment_plan.segments.size();
        new_segment.estimated_tokens = new_segment_tokens;
        new_segment.exact_tokens = new_segment_tokens;
        new_segment.manual_start = manual_start;
        new_segment.auto_split_start = auto_start;

        persistent::MemoryRecord record;
        record.memory_idx = session.store.next_memory_idx++;
        record.has_id = memory.has_id;
        record.id = memory.id;
        record.text = memory.text;
        if (!estimate_memory_text_tokens(core, memory.text, record.estimated_tokens, error)) {
            record.estimated_tokens = 0;
            error.clear();
        }
        session.store.memories.push_back(std::move(record));
        session.store.system_locked = true;
        new_segment.memory_indices.push_back(session.store.memories.back().memory_idx);
        const int32_t new_segment_id = new_segment.segment_id;
        session.segment_plan.segments.push_back(std::move(new_segment));
        mark_segment_kv_missing(
            cache_dir,
            model_key,
            session,
            session.segment_plan.segments.back());

        token_count = new_segment_tokens;
        segment_id = new_segment_id;
        persist_session_facts = true;
        return true;
    }

    if (last_segment == nullptr) {
        error = "missing writable segment";
        return false;
    }

    persistent::MemoryRecord record;
    record.memory_idx = session.store.next_memory_idx++;
    record.has_id = memory.has_id;
    record.id = memory.id;
    record.text = memory.text;
    if (!estimate_memory_text_tokens(core, memory.text, record.estimated_tokens, error)) {
        record.estimated_tokens = 0;
        error.clear();
    }
    session.store.memories.push_back(std::move(record));
    session.store.system_locked = true;
    last_segment->memory_indices.push_back(session.store.memories.back().memory_idx);
    last_segment->estimated_tokens = candidate_existing_tokens;
    last_segment->exact_tokens = candidate_is_exact ? candidate_existing_tokens : 0;
    mark_segment_kv_missing(cache_dir, model_key, session, *last_segment);

    token_count = candidate_existing_tokens;
    segment_id = last_segment->segment_id;
    return true;
}

} // namespace

SessionManager::SessionManager() :
    impl_(std::make_unique<Impl>()) {
}

SessionManager::~SessionManager() {
    shutdown();
}

bool SessionManager::init(const ContextOptions & options, std::string & error) {
    shutdown();
    impl_->state.options = options;
    impl_->state.options.runtime.search_candidate_top_k = options.search.candidate_top_k;
    impl_->state.data_dir = normalize_storage_root(options.data_dir);
    impl_->state.cache_dir = normalize_storage_root(options.cache_dir);
    impl_->kv_manager.set_resident_budget_bytes(options.resident_kv_budget_bytes);

    atmcore::set_log_enabled(options.run_log);
    if (options.run_log) {
        std::fprintf(stderr, "startup: loading model %s\n", options.model_path.c_str());
    }
    atmcore::RuntimeCreateOptions core_config;
    core_config.model_path = options.model_path;
    core_config.model_tier = options.model_tier;
    core_config.runtime = impl_->state.options.runtime;
    core_config.internal.v = options.v;
    core_config.internal.truncate = options.truncate;
    if (!create_runtime(core_config, impl_->state.core, error)) {
        shutdown();
        return false;
    }
    if (options.run_log) {
        std::fprintf(stderr, "startup: model loaded\n");
    }

    const atmcore::RuntimeInfo & core_info = atmcore::runtime_info(impl_->state.core);
    impl_->state.options.runtime = core_info.runtime;
    impl_->state.options.v = core_info.internal.v;
    impl_->state.model_cache_key = build_model_cache_key(core_info);

    impl_->state.root_session_layout = session_storage_layout(impl_->state.data_dir, impl_->state.model_cache_key);
    const persistent::CacheLayout root_cache_layout =
        cache_layout_for_session(impl_->state.cache_dir, impl_->state.model_cache_key, std::string());
    if (options.run_log) {
        std::fprintf(stderr, "startup: scanning sessions\n");
    }
    if (!result_to_bool(
            persistent::ensure_directory(persistent::session_root_dir_path(impl_->state.root_session_layout)),
            error)) {
        shutdown();
        return false;
    }
    if (!result_to_bool(
            persistent::ensure_directory(persistent::cache_root_dir_path(root_cache_layout)),
            error)) {
        shutdown();
        return false;
    }

    persistent::SessionStoreMap session_stores;
    if (!result_to_bool(persistent::scan_sessions(impl_->state.root_session_layout, session_stores), error)) {
        if (options.run_log) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        error.clear();
    }
    for (auto & item : session_stores) {
        Session session;
        session.store = std::move(item.second);
        impl_->state.sessions.emplace(session.store.session_id, std::move(session));
    }
    if (options.run_log) {
        std::fprintf(stderr, "startup: scanned_sessions=%zu lazy_init=true\n", impl_->state.sessions.size());
    }

    impl_->state.startup = {};
    impl_->state.startup.runtime = core_info.runtime;
    impl_->state.startup.model_cache_key = impl_->state.model_cache_key.value;
    impl_->state.startup.startup_n_ctx = core_info.startup_n_ctx;
    impl_->state.startup.startup_n_ctx_source = core_info.startup_n_ctx_source;
    impl_->state.startup.truncate = options.truncate;
    impl_->state.startup.restored_sessions = impl_->state.sessions.size();
    impl_->state.startup.resident_kv_budget_bytes = options.resident_kv_budget_bytes;
    impl_->state.startup.search = options.search;

    error.clear();
    return true;
}

void SessionManager::shutdown() {
    impl_->kv_manager.clear_all();
    impl_->state.sessions.clear();
    destroy_runtime(impl_->state.core);
    impl_->state = {};
    atmcore::set_log_enabled(false);
}

const StartupInfo & SessionManager::startup_info() const {
    return impl_->state.startup;
}

const std::vector<int32_t> & SessionManager::v() const {
    return impl_->state.options.v;
}

std::vector<SessionStatus> SessionManager::list_sessions() {
    return list_session_statuses(
        impl_->state.cache_dir,
        impl_->state.model_cache_key,
        impl_->state.sessions,
        impl_->kv_manager);
}

const persistent::SessionStore * SessionManager::session_store(const std::string & session_id) const {
    const SessionMap & sessions = impl_->state.sessions;
    const Session * session = find_session(sessions, session_id);
    return session == nullptr ? nullptr : &session->store;
}

CommandResult SessionManager::create_session(
    const std::string & session_id,
    const CreateSessionOptions & options) {
    if (!persistent::is_valid_session_id(session_id)) {
        return make_error(ErrorCode::InvalidSessionId, "invalid session id");
    }

    CommandResult result;
    const persistent::StorageLayout layout =
        session_storage_layout(impl_->state.data_dir, impl_->state.model_cache_key, session_id);
    if (find_session(impl_->state.sessions, session_id) != nullptr ||
        std::filesystem::exists(persistent::session_dir_path(layout))) {
        return make_error(
            ErrorCode::SessionAlreadyExists,
            "session already exists: " + session_id,
            {{"session_id", session_id}});
    }

    Session session;
    session.store.session_id = session_id;
    session.store.kv_persist = options.kv_persist;
    const persistent::ResultStatus save_status =
        persistent::save_session_metadata(layout, session.store);
    if (!save_status.ok) {
        return make_error(ErrorCode::InternalError, save_status.error);
    }
    session.facts_dirty = false;

    initialize_session_plan(
        impl_->state.core,
        impl_->state.options.runtime,
        impl_->state.cache_dir,
        impl_->state.model_cache_key,
        session,
        impl_->state.options.run_log,
        result.error);
    impl_->state.sessions.emplace(session.store.session_id, std::move(session));

    return token_usage_result(
        impl_->state.core,
        impl_->state.options.runtime,
        0,
        0,
        kNoSegmentId);
}

CommandResult SessionManager::restore_session(const std::string & session_id) {
    if (!persistent::is_valid_session_id(session_id)) {
        return make_error(ErrorCode::InvalidSessionId, "invalid session id");
    }

    CommandResult result;
    result.payload = ResultPayload::RestoreSummary;

    Session * current = find_session(impl_->state.sessions, session_id);
    if (current != nullptr) {
        result.ok = true;
        result.restored = true;
        result.summary = summarize_session(impl_->kv_manager, current);
        return result;
    }

    const persistent::StorageLayout layout =
        session_storage_layout(impl_->state.data_dir, impl_->state.model_cache_key, session_id);
    if (!std::filesystem::exists(persistent::session_dir_path(layout))) {
        result.ok = false;
        result.restored = false;
        result.summary = summarize_session(impl_->kv_manager, nullptr);
        result.error_info = make_error_info(
            ErrorCode::SessionNotFound,
            "session not found: " + session_id,
            {{"session_id", session_id}});
        result.error = result.error_info.message;
        return result;
    }

    Session restored;
    const persistent::ResultStatus load_status = persistent::load_session(layout, restored.store);
    if (!load_status.ok) {
        result.ok = false;
        result.restored = false;
        result.summary = summarize_session(impl_->kv_manager, nullptr);
        result.error_info = make_error_info(ErrorCode::InternalError, load_status.error);
        result.error = result.error_info.message;
        return result;
    }
    result.ok = true;
    result.restored = true;
    restored.facts_dirty = false;

    restored.segment_plan = {};
    restored.segment_kv_metadata.clear();
    impl_->kv_manager.evict_session(session_id);
    impl_->kv_manager.clear_resident_session(session_id);
    impl_->state.sessions[session_id] = std::move(restored);
    Session * session = find_session(impl_->state.sessions, session_id);
    result.summary = summarize_session(impl_->kv_manager, session);
    return result;
}

CommandResult SessionManager::add_system(const std::string & session_id, const std::string & system) {
    SessionScope scope(impl_->command_context(), impl_->kv_manager, AttemoryCommand::ADD_SYSTEM, session_id);
    if (!scope.require_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();
    if (!session.store.memories.empty()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "add-system is only allowed before add-memory");
    }

    CommandResult result;
    int32_t token_count = 0;
    session.store.system_text = system;
    session.store.system_locked = true;
    session.facts_dirty = true;
    reset_session_derived_state(impl_->kv_manager, session);
    if (!atmcore::estimate_memory_kv_token_count(
            impl_->state.core,
            impl_->state.options.runtime,
            session.store.system_text,
            std::vector<std::string>{},
            token_count,
            result.error)) {
        token_count = -1;
        result.error.clear();
    }

    const persistent::ResultStatus save_status =
        persistent::save_session_metadata(
            session_storage_layout(impl_->state.data_dir, impl_->state.model_cache_key, session_id),
            session.store);
    if (!save_status.ok) {
        return make_error(ErrorCode::InternalError, save_status.error);
    }
    session.facts_dirty = false;

    return token_usage_result(
        impl_->state.core,
        impl_->state.options.runtime,
        (int32_t) session.segment_plan.segments.size(),
        token_count,
        kNoSegmentId);
}

CommandResult SessionManager::add_memory(const std::string & session_id, const MemoryInput & memory) {
    SessionScope scope(impl_->command_context(), impl_->kv_manager, AttemoryCommand::ADD_MEMORY, session_id);
    if (!scope.require_planned_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    CommandResult result;
    int32_t token_count = -1;
    int32_t segment_id = kNoSegmentId;
    bool persist_session_facts = false;
    const size_t previous_segment_count = session.segment_plan.segments.size();
    result.ok = append_memory_and_update_plan(
        impl_->state.core,
        impl_->state.options.runtime,
        impl_->state.cache_dir,
        impl_->state.model_cache_key,
        session,
        memory,
        token_count,
        segment_id,
        persist_session_facts,
        result.error);
    if (!result.ok) {
        return result;
    }
    impl_->kv_manager.evict_session(session.store.session_id);
    if (segment_id != kNoSegmentId &&
        session.segment_plan.segments.size() == previous_segment_count) {
        impl_->kv_manager.stage_incremental_prefix(session.store.session_id, segment_id);
    }
    session.facts_dirty = true;

    if (persist_session_facts && !scope.persist()) {
        return scope.failure_result();
    }

    CommandResult usage = token_usage_result(
        impl_->state.core,
        impl_->state.options.runtime,
        (int32_t) session.segment_plan.segments.size(),
        token_count,
        segment_id,
        (int32_t) session.store.memories.back().memory_idx);
    usage.token_usage.has_memory_id = session.store.memories.back().has_id;
    usage.token_usage.memory_id = session.store.memories.back().id;
    return usage;
}

CommandResult SessionManager::next_segment(const std::string & session_id) {
    SessionScope scope(impl_->command_context(), impl_->kv_manager, AttemoryCommand::NEXT_SEGMENT, session_id);
    if (!scope.require_planned_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    Segment * last_segment =
        session.segment_plan.segments.empty() ? nullptr : &session.segment_plan.segments.back();
    if (last_segment == nullptr || last_segment->memory_indices.empty()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "session has no current segment to seal");
    }
    if (std::find(
            session.store.manual_segment_boundaries.begin(),
            session.store.manual_segment_boundaries.end(),
            session.store.next_memory_idx) != session.store.manual_segment_boundaries.end()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "next segment has already been requested");
    }

    impl_->kv_manager.evict_session(session.store.session_id);
    session.store.manual_segment_boundaries.push_back(session.store.next_memory_idx);
    session.facts_dirty = true;
    if (!scope.persist()) {
        return scope.failure_result();
    }
    return success_result();
}

CommandResult SessionManager::clear_session(const std::string & session_id) {
    SessionScope scope(impl_->command_context(), impl_->kv_manager, AttemoryCommand::CLEAR, session_id);
    if (!scope.require_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    evict_session_runtime_state(impl_->kv_manager, session);
    mark_resident_segment_kv_metadata_cleared(session);
    return success_result();
}

CommandResult SessionManager::delete_session(const std::string & session_id) {
    SessionScope scope(impl_->command_context(), impl_->kv_manager, AttemoryCommand::DELETE_SESSION, session_id);
    if (!scope.require_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    const std::string erased_session_id = session.store.session_id;
    evict_session_runtime_state(impl_->kv_manager, session);

    CommandResult result;
    const persistent::ResultStatus remove_status =
        persistent::remove_session(
            session_storage_layout(impl_->state.data_dir, impl_->state.model_cache_key, session_id));
    if (!remove_status.ok) {
        result = make_error(ErrorCode::InternalError, remove_status.error);
    } else {
        result.ok = true;
    }
    delete_session_kv_cache(impl_->state.cache_dir, impl_->state.model_cache_key, erased_session_id);
    if (result.ok) {
        impl_->state.sessions.erase(erased_session_id);
    }
    return result;
}

CommandResult SessionManager::index_session(const std::string & session_id) {
    return impl_->kv_manager.index_session_kv_cache(
        { impl_->command_context(), impl_->state.native_search_warmup_done },
        session_id);
}

CommandResult SessionManager::save_session(const std::string & session_id) {
    return impl_->kv_manager.save_session_kv_cache(
        { impl_->command_context(), impl_->state.native_search_warmup_done },
        session_id);
}

CommandResult SessionManager::search(
    const std::string & session_id,
    const std::string & query_text,
    const SearchRequestOverrides & request_overrides) {
    return search(session_id, SearchInput{std::string(), query_text}, request_overrides);
}

CommandResult SessionManager::search(
    const std::string & session_id,
    const SearchInput & input,
    const SearchRequestOverrides & request_overrides) {
    const SearchConfig search_config = apply_search_request_overrides(impl_->state.options.search, request_overrides);
    return run_search(impl_->search_context(), impl_->kv_manager, session_id, input, search_config);
}

CommandResult SessionManager::oneshot_search(
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const std::string & query,
    const SearchRequestOverrides & request_overrides) {
    return oneshot_search(system, memories, SearchInput{std::string(), query}, request_overrides);
}

CommandResult SessionManager::oneshot_search(
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const SearchInput & input,
    const SearchRequestOverrides & request_overrides) {
    const SearchConfig search_config = apply_search_request_overrides(impl_->state.options.search, request_overrides);
    return run_oneshot_search(impl_->search_context(), impl_->kv_manager, system, memories, input, search_config);
}

} // namespace attemory::context
