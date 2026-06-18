#include "context/search/search_runner.h"

#include "attemory-core/attemory-core.h"
#include "context/search/search_results.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace attemory::context {
namespace {

using steady_clock = std::chrono::steady_clock;

int64_t elapsed_ms(
    const steady_clock::time_point & start,
    const steady_clock::time_point & end = steady_clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

std::vector<const Segment *> sorted_segments_by_id(const SegmentPlan & plan) {
    std::vector<const Segment *> segments;
    segments.reserve(plan.segments.size());
    for (const Segment & segment : plan.segments) {
        segments.push_back(&segment);
    }

    std::stable_sort(
        segments.begin(),
        segments.end(),
        [](const Segment * lhs, const Segment * rhs) {
            return lhs->segment_id < rhs->segment_id;
        });
    return segments;
}

size_t query_stage_context_token_count(const atmcore::SearchQueryTokens & tokens) {
    return tokens.context_tokens.size() + tokens.query_tokens.size();
}

void demote_removed_resident_metadata(Session & session, int32_t segment_id) {
    persistent::KVCacheEntry * cache = find_segment_kv_metadata(session, segment_id);
    if (cache == nullptr ||
        cache->state != persistent::CacheState::Resident) {
        return;
    }

    cache->state = persistent::kv_cache_state_exists(*cache)
        ? persistent::CacheState::DiskOnly
        : persistent::CacheState::Missing;
}

} // namespace

SegmentSearchRunner::SegmentSearchRunner(
    SearchRuntimeContext context,
    kv::SegmentKVManager & kv_manager,
    Session & session,
    const atmcore::SearchQueryTokens & query_stage_tokens) :
    context_(context),
    kv_manager_(kv_manager),
    session_(session),
    query_stage_tokens_(query_stage_tokens) {
}

bool SegmentSearchRunner::run_cached(
    std::vector<AttentionMemoryRef> & ordered_memories,
    std::string & error) {
    error.clear();

    const bool stream_resident_segments =
        kv_manager_.resident_budget_bytes() == 0 &&
        session_.segment_plan.segments.size() > 1;

    for (const Segment * segment_ptr : sorted_segments_by_id(session_.segment_plan)) {
        const Segment & segment = *segment_ptr;
        if (!kv_manager_.activate_for_search(
                session_,
                context_.command.runtime,
                segment.segment_id,
                query_stage_context_token_count(query_stage_tokens_),
                error)) {
            kv_manager_.clear_active();
            return false;
        }

        if (context_.command.kv.active_segment.handle == nullptr) {
            error = "active segment KV is unavailable";
            return false;
        }

        const atmcore::RetrievalResult inference = atmcore::run_search_with_query_tokens(
            context_.command.core,
            context_.command.runtime,
            context_.command.kv.active_segment.handle->active,
            query_stage_tokens_);
        if (!inference.ok) {
            error = inference.error;
            kv_manager_.clear_active();
            return false;
        }

        kv_manager_.sync_active_search_cache_to_resident();
        merge_segment_ranked_results(session_.store, segment, inference.ranked_memories, ordered_memories);

        if (stream_resident_segments) {
            kv_manager_.remove_resident_segment(session_.store.session_id, segment.segment_id);
            demote_removed_resident_metadata(session_, segment.segment_id);
            kv_manager_.clear_active();
        }
    }

    return true;
}

bool SegmentSearchRunner::run_ephemeral(
    const std::string & system,
    std::vector<AttentionMemoryRef> & ordered_memories,
    std::string & error) {
    error.clear();

    for (const Segment * segment_ptr : sorted_segments_by_id(session_.segment_plan)) {
        const Segment & segment = *segment_ptr;
        const steady_clock::time_point segment_start = steady_clock::now();
        const std::vector<std::string> segment_memories =
            collect_segment_memory_texts(session_.store, segment);

        atmcore::ActiveKV base_state;
        const steady_clock::time_point build_start = steady_clock::now();
        if (!atmcore::build_active_kv_ephemeral(
                context_.command.core,
                context_.command.runtime,
                system,
                segment_memories,
                base_state,
                error)) {
            if (context_.command.run_log) {
                std::fprintf(
                    stderr,
                    "oneshot-search segment=%d failed stage=build memories=%zu estimated_tokens=%d elapsed_ms=%lld error=\"%s\"\n",
                    segment.segment_id,
                    segment.memory_indices.size(),
                    segment.estimated_tokens,
                    (long long) elapsed_ms(segment_start),
                    error.c_str());
                std::fflush(stderr);
            }
            return false;
        }
        const int64_t build_ms = elapsed_ms(build_start);

        const steady_clock::time_point search_start = steady_clock::now();
        const atmcore::RetrievalResult inference = atmcore::run_search_with_query_tokens(
            context_.command.core,
            context_.command.runtime,
            base_state,
            query_stage_tokens_);
        const int64_t search_ms = elapsed_ms(search_start);
        const size_t base_token_count = base_state.metadata.base_tokens.size();
        atmcore::release_active_kv_context(base_state);
        if (!inference.ok) {
            error = inference.error;
            if (context_.command.run_log) {
                std::fprintf(
                    stderr,
                    "oneshot-search segment=%d failed stage=search memories=%zu estimated_tokens=%d build_ms=%lld search_ms=%lld elapsed_ms=%lld error=\"%s\"\n",
                    segment.segment_id,
                    segment.memory_indices.size(),
                    segment.estimated_tokens,
                    (long long) build_ms,
                    (long long) search_ms,
                    (long long) elapsed_ms(segment_start),
                    error.c_str());
                std::fflush(stderr);
            }
            return false;
        }

        const steady_clock::time_point merge_start = steady_clock::now();
        merge_segment_ranked_results(session_.store, segment, inference.ranked_memories, ordered_memories);
        const int64_t merge_ms = elapsed_ms(merge_start);
        if (context_.command.run_log) {
            std::fprintf(
                stderr,
                "oneshot-search segment=%d memories=%zu estimated_tokens=%d base_tokens=%zu build_ms=%lld search_ms=%lld merge_ms=%lld total_ms=%lld\n",
                segment.segment_id,
                segment.memory_indices.size(),
                segment.estimated_tokens,
                base_token_count,
                (long long) build_ms,
                (long long) search_ms,
                (long long) merge_ms,
                (long long) elapsed_ms(segment_start));
            std::fflush(stderr);
        }
    }

    return true;
}

} // namespace attemory::context
