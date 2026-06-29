#include "context/session/segment_planner.h"

#include "context/kv/segment_kv_metadata.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace attemory::context {

namespace {

inline constexpr int32_t kAppendTokenEstimateSafetyMargin = 4;

bool estimate_memory_text_tokens(
    atmcore::Runtime * core,
    const std::string & text,
    int32_t & token_count,
    std::string & error) {
    return atmcore::estimate_text_token_count_fast(core, text, false, token_count, error);
}

int32_t effective_context_limit(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime) {
    return atmcore::effective_context_length(core, runtime);
}

} // namespace

int32_t segment_soft_limit(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime) {
    const int32_t ctx_length = effective_context_limit(core, runtime);
    if (ctx_length <= 0) {
        return 0;
    }

    return std::max<int32_t>(ctx_length * kSegmentSoftLimitPercent / 100, 1);
}

static void initialize_empty_segment_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const persistent::ModelCacheKey & model_key,
    Session & session) {
    session.segment_plan = {};
    session.segment_plan.session_id = session.store.session_id;
    session.segment_plan.config.model_cache_key = model_key.value;
    session.segment_plan.config.ctx_limit = effective_context_limit(core, runtime);
    session.segment_plan.config.split_ratio = (float) kSegmentSoftLimitPercent / 100.0f;
    session.segment_plan.config.max_segments = kMaxSegmentsPerSession;
}

static bool estimate_new_segment_token_count(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::string & system,
    const std::string & memory,
    int32_t & token_count,
    std::string & error) {
    const std::vector<std::string> memories = { memory };
    return atmcore::estimate_memory_kv_token_count(core, runtime, system, memories, token_count, error);
}

static bool estimate_empty_segment_token_count(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::string & system,
    int32_t & token_count,
    std::string & error) {
    const std::vector<std::string> memories;
    return atmcore::estimate_memory_kv_token_count(core, runtime, system, memories, token_count, error);
}

static bool estimate_segment_token_count_with_extra_memory(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const SessionFacts & facts,
    const Segment & segment,
    const std::string & memory,
    int32_t & token_count,
    std::string & error) {
    std::vector<std::string> memories = collect_segment_memory_texts(facts, segment);
    memories.push_back(memory);
    return atmcore::estimate_memory_kv_token_count(core, runtime, facts.system_text, memories, token_count, error);
}

static int32_t append_token_estimate(
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

static void log_manifest_skip(
    bool run_log,
    const Session & session,
    const std::string & reason) {
    if (!run_log) {
        return;
    }
    std::fprintf(
        stderr,
        "restore-cache-manifest skipped session=%s reason=\"%s\"\n",
        session.store.session_id.c_str(),
        reason.c_str());
    std::fflush(stderr);
}

static bool rebuild_segment_plan_fast(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool honor_manual_boundaries,
    bool refresh_memory_token_estimates,
    int32_t fresh_segment_token_limit,
    std::string & error) {
    error.clear();

    initialize_empty_segment_plan(core, runtime, model_key, session);

    if (session.store.memories.empty()) {
        return true;
    }

    const int32_t soft_limit = segment_soft_limit(core, runtime);
    if (soft_limit <= 0 || fresh_segment_token_limit <= 0) {
        error = "failed to resolve effective context length";
        return false;
    }

    int32_t empty_segment_tokens = 0;
    if (!estimate_empty_segment_token_count(
            core, runtime, session.store.system_text, empty_segment_tokens, error)) {
        return false;
    }

    const std::set<persistent::MemoryIndex> manual_boundaries(
        session.store.manual_segment_boundaries.begin(),
        session.store.manual_segment_boundaries.end());

    for (persistent::MemoryRecord & memory : session.store.memories) {
        if (refresh_memory_token_estimates || memory.estimated_tokens <= 0) {
            if (!estimate_memory_text_tokens(core, memory.text, memory.estimated_tokens, error)) {
                return false;
            }
        }

        Segment * last_segment =
            session.segment_plan.segments.empty() ? nullptr : &session.segment_plan.segments.back();
        const bool manual_start =
            honor_manual_boundaries &&
            manual_boundaries.find(memory.memory_idx) != manual_boundaries.end();
        bool need_new_segment = last_segment == nullptr || manual_start;
        bool auto_start = false;
        int32_t candidate_existing_tokens = -1;
        bool candidate_is_exact = false;

        int32_t new_segment_tokens = 0;
        if (!estimate_new_segment_token_count(
                core,
                runtime,
                session.store.system_text,
                memory.text,
                new_segment_tokens,
                error)) {
            return false;
        }
        if (new_segment_tokens > fresh_segment_token_limit) {
            error = "memory is too large for a fresh segment";
            return false;
        }

        if (!need_new_segment && last_segment != nullptr) {
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

            Segment segment;
            segment.segment_id = (int32_t) session.segment_plan.segments.size();
            segment.estimated_tokens = new_segment_tokens;
            segment.exact_tokens = new_segment_tokens;
            segment.manual_start = manual_start;
            segment.auto_split_start = auto_start;
            session.segment_plan.segments.push_back(std::move(segment));
            last_segment = &session.segment_plan.segments.back();
        } else {
            last_segment->estimated_tokens = candidate_existing_tokens;
            last_segment->exact_tokens = candidate_is_exact ? candidate_existing_tokens : 0;
        }

        last_segment->memory_indices.push_back(memory.memory_idx);
    }

    return true;
}

static bool restore_segment_plan_from_cache_manifest(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool & restored_from_manifest,
    bool run_log,
    std::string & error) {
    restored_from_manifest = false;
    error.clear();

    if (session.store.memories.empty()) {
        initialize_empty_segment_plan(core, runtime, model_key, session);
        restored_from_manifest = true;
        return true;
    }

    const persistent::CacheLayout layout =
        cache_layout_for_session(cache_dir, model_key, session.store.session_id);
    persistent::CacheManifest manifest;
    const persistent::ResultStatus status = persistent::load_kv_cache_manifest(layout, manifest);
    if (!status.ok) {
        log_manifest_skip(run_log, session, status.error);
        return true;
    }
    if (!persistent::model_cache_key_equal(manifest.model, model_key)) {
        log_manifest_skip(run_log, session, "model cache key mismatch");
        return true;
    }
    if (manifest.session_id != session.store.session_id) {
        log_manifest_skip(run_log, session, "session id mismatch");
        return true;
    }
    if (manifest.segments.empty()) {
        log_manifest_skip(run_log, session, "manifest has no segments");
        return true;
    }

    std::vector<persistent::SegmentCacheManifest> manifest_segments = manifest.segments;
    std::sort(
        manifest_segments.begin(),
        manifest_segments.end(),
        [](const persistent::SegmentCacheManifest & a, const persistent::SegmentCacheManifest & b) {
            return a.segment_id < b.segment_id;
        });

    SegmentPlan candidate;
    candidate.session_id = session.store.session_id;
    candidate.config.model_cache_key = model_key.value;
    candidate.config.ctx_limit = effective_context_limit(core, runtime);
    candidate.config.split_ratio = (float) kSegmentSoftLimitPercent / 100.0f;
    candidate.config.max_segments = kMaxSegmentsPerSession;

    const int32_t hard_limit = effective_context_limit(core, runtime);
    if (hard_limit <= 0) {
        log_manifest_skip(run_log, session, "failed to resolve effective context length");
        return true;
    }

    const std::set<persistent::MemoryIndex> manual_boundaries(
        session.store.manual_segment_boundaries.begin(),
        session.store.manual_segment_boundaries.end());
    std::set<persistent::MemoryIndex> covered;

    for (const persistent::SegmentCacheManifest & manifest_segment : manifest_segments) {
        if (manifest_segment.segment_id < 0 ||
            manifest_segment.first_memory_idx < 0 ||
            manifest_segment.last_memory_idx_exclusive <= manifest_segment.first_memory_idx) {
            log_manifest_skip(
                run_log,
                session,
                "invalid segment range: segment=" + std::to_string(manifest_segment.segment_id));
            return true;
        }

        Segment segment;
        segment.segment_id = manifest_segment.segment_id;
        segment.estimated_tokens = manifest_segment.token_count;
        segment.exact_tokens = manifest_segment.token_count;
        segment.manual_start =
            manual_boundaries.find(manifest_segment.first_memory_idx) != manual_boundaries.end();
        segment.auto_split_start = !candidate.segments.empty() && !segment.manual_start;
        if (segment.exact_tokens > hard_limit) {
            log_manifest_skip(
                run_log,
                session,
                "segment exceeds context limit: segment=" + std::to_string(segment.segment_id));
            return true;
        }

        for (persistent::MemoryIndex memory_idx = manifest_segment.first_memory_idx;
             memory_idx < manifest_segment.last_memory_idx_exclusive;
             ++memory_idx) {
            if (persistent::find_memory(session.store, memory_idx) == nullptr ||
                covered.find(memory_idx) != covered.end()) {
                log_manifest_skip(
                    run_log,
                    session,
                    "segment memory coverage is invalid: segment=" + std::to_string(segment.segment_id));
                return true;
            }
            segment.memory_indices.push_back(memory_idx);
            covered.insert(memory_idx);
        }

        if (segment_kv_content_hash(session.store, segment) != manifest_segment.segment_content_hash) {
            log_manifest_skip(
                run_log,
                session,
                "segment content hash mismatch: segment=" + std::to_string(segment.segment_id));
            return true;
        }

        candidate.segments.push_back(std::move(segment));
    }

    if (covered.size() != session.store.memories.size()) {
        log_manifest_skip(
            run_log,
            session,
            "partial manifest coverage: covered=" +
                std::to_string(covered.size()) +
                "/" +
                std::to_string(session.store.memories.size()));
        return true;
    }
    for (const persistent::MemoryRecord & memory : session.store.memories) {
        if (covered.find(memory.memory_idx) == covered.end()) {
            log_manifest_skip(
                run_log,
                session,
                "manifest missing memory index: memory_idx=" + std::to_string(memory.memory_idx));
            return true;
        }
    }

    session.segment_plan = std::move(candidate);
    restored_from_manifest = true;
    return true;
}

bool initialize_session_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool run_log,
    std::string & error) {
    if (!session.segment_plan.config.model_cache_key.empty() &&
        session.segment_plan.config.model_cache_key != model_key.value) {
        clear_session_resident_kv_metadata(session);
        session.segment_kv_metadata.clear();
        session.segment_plan = {};
    }

    bool restored_from_manifest = false;
    if (!restore_segment_plan_from_cache_manifest(
            core,
            runtime,
            cache_dir,
            model_key,
            session,
            restored_from_manifest,
            run_log,
            error)) {
        return false;
    }

    if (!restored_from_manifest) {
        const int32_t soft_limit = segment_soft_limit(core, runtime);
        if (!rebuild_segment_plan_fast(
                core,
                runtime,
                model_key,
                session,
                /*honor_manual_boundaries =*/ true,
                /*refresh_memory_token_estimates =*/ false,
                soft_limit,
                error)) {
            return false;
        }
    }

    refresh_session_kv_metadata(cache_dir, model_key, session);
    return true;
}

static bool session_plan_ready(const Session & session) {
    return session.segment_plan.session_id == session.store.session_id;
}

static bool session_plan_ready(
    const Session & session,
    const persistent::ModelCacheKey & model_key) {
    return session_plan_ready(session) &&
           session.segment_plan.config.model_cache_key == model_key.value;
}

bool prepare_session_plan(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const std::filesystem::path & cache_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    bool run_log,
    std::string & error) {
    if (session_plan_ready(session, model_key)) {
        error.clear();
        return true;
    }

    return initialize_session_plan(core, runtime, cache_dir, model_key, session, run_log, error);
}

bool rebuild_segment_plan_fast_for_oneshot(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    std::string & error) {
    error.clear();

    const int32_t hard_limit = effective_context_limit(core, runtime);
    return rebuild_segment_plan_fast(
        core,
        runtime,
        model_key,
        session,
        /*honor_manual_boundaries =*/ false,
        /*refresh_memory_token_estimates =*/ true,
        hard_limit,
        error);
}

} // namespace attemory::context
