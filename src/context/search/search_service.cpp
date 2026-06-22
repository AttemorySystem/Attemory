#include "context/search/search_service.h"

#include "attemory-core/attemory-core.h"
#include "context/command/command_result.h"
#include "context/command/command_scope.h"
#include "context/search/search_results.h"
#include "context/search/search_runner.h"
#include "context/session/segment_planner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace attemory::context {
namespace {

using steady_clock = std::chrono::steady_clock;

int64_t elapsed_ms(
    const steady_clock::time_point & start,
    const steady_clock::time_point & end = steady_clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

Session build_oneshot_session(
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories) {
    Session session;
    session.store.session_id = "__oneshot__";
    session.store.system_text = system;
    session.store.system_locked = true;
    session.store.next_memory_idx = 0;
    session.store.memories.reserve(memories.size());

    for (size_t i = 0; i < memories.size(); ++i) {
        persistent::MemoryRecord record;
        record.memory_idx = (persistent::MemoryIndex) i;
        record.text = memories[i].text;
        session.store.memories.push_back(std::move(record));
        session.store.next_memory_idx = (persistent::MemoryIndex) (i + 1);
    }

    return session;
}

std::vector<AttentionMemoryRef> apply_search_result_limits(
    const std::vector<AttentionMemoryRef> & ordered_memories,
    const SearchConfig & config) {
    std::vector<AttentionMemoryRef> limited;
    limited.reserve(ordered_memories.size());

    std::unordered_map<int32_t, int32_t> counts_by_segment;
    if (config.top_k > 0) {
        counts_by_segment.reserve(ordered_memories.size());
    }

    for (const AttentionMemoryRef & memory : ordered_memories) {
        if (config.top_k > 0) {
            int32_t & segment_count = counts_by_segment[memory.segment_id];
            if (segment_count >= config.top_k) {
                continue;
            }
            ++segment_count;
        }

        limited.push_back(memory);
    }

    return limited;
}

} // namespace

SearchConfig apply_search_request_overrides(
    const SearchConfig & base,
    const SearchRequestOverrides & request_overrides) {
    SearchConfig config = base;
    if (request_overrides.top_k >= 0) {
        config.top_k = request_overrides.top_k;
    }
    return config;
}

static size_t query_stage_context_token_count(const atmcore::SearchQueryTokens & tokens) {
    return tokens.context_tokens.size() + tokens.query_tokens.size();
}

CommandResult run_search(
    SearchRuntimeContext context,
    kv::SegmentKVManager & kv_manager,
    const std::string & session_id,
    const SearchInput & input,
    const SearchConfig & search_config) {
    SessionScope scope(context.command, kv_manager, AttemoryCommand::SEARCH, session_id);
    if (!scope.require_planned_session()) {
        return scope.failure_result();
    }
    Session & session = scope.session();

    if (!scope.require_segments()) {
        return scope.failure_result();
    }

    CommandResult result;
    atmcore::SearchQueryTokens query_stage_tokens;
    if (!atmcore::build_query_stage_tokens(
            context.command.core,
            context.command.runtime,
            session.store.system_text,
            input.query_context,
            input.query,
            query_stage_tokens,
            result.error)) {
        result.ok = false;
        return result;
    }
    if (context.command.run_log) {
        std::fprintf(
            stderr,
            "search-query-stage session=%s query_context_chars=%zu query_chars=%zu "
            "context_tokens=%zu query_tokens=%zu query_stage_tokens=%zu\n",
            session.store.session_id.c_str(),
            input.query_context.size(),
            input.query.size(),
            query_stage_tokens.context_tokens.size(),
            query_stage_tokens.query_tokens.size(),
            query_stage_context_token_count(query_stage_tokens));
        std::fflush(stderr);
    }

    result = success_result(ResultPayload::Search);
    SegmentSearchRunner runner(context, kv_manager, session, query_stage_tokens);
    std::vector<AttentionMemoryRef> ordered_memories;
    if (!runner.run_cached(ordered_memories, result.error)) {
        result.ok = false;
        return result;
    }

    result.ordered_search_memories = apply_search_result_limits(ordered_memories, search_config);
    return result;
}

CommandResult run_oneshot_search(
    SearchRuntimeContext context,
    kv::SegmentKVManager & kv_manager,
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const SearchInput & input,
    const SearchConfig & search_config) {
    CommandResult result;
    const steady_clock::time_point total_start = steady_clock::now();

    if (memories.empty()) {
        return make_error(
            ErrorCode::InvalidRequest,
            "oneshot-search requires at least one memory");
    }

    std::string pre_cleanup_error;
    if (!kv_manager.release_all_live_contexts(pre_cleanup_error)) {
        if (context.command.run_log) {
            std::fprintf(
                stderr,
                "oneshot-search cleanup failed stage=before-search memories=%zu payload_query_bytes=%zu payload_context_bytes=%zu elapsed_ms=%lld error=\"%s\"\n",
                memories.size(),
                input.query.size(),
                input.query_context.size(),
                (long long) elapsed_ms(total_start),
                pre_cleanup_error.c_str());
            std::fflush(stderr);
        }
        return make_error(make_error_info(ErrorCode::InternalError, pre_cleanup_error));
    }

    Session session = build_oneshot_session(system, memories);

    const steady_clock::time_point plan_start = steady_clock::now();
    if (!rebuild_segment_plan_fast_for_oneshot(
            context.command.core,
            context.command.runtime,
            context.command.model_key,
            session,
            result.error)) {
        result.error_info = make_error_info(ErrorCode::InternalError, result.error);
        if (context.command.run_log) {
            std::fprintf(
                stderr,
                "oneshot-search failed stage=segment-plan memories=%zu payload_query_bytes=%zu payload_context_bytes=%zu elapsed_ms=%lld error=\"%s\"\n",
                memories.size(),
                input.query.size(),
                input.query_context.size(),
                (long long) elapsed_ms(total_start),
                result.error.c_str());
            std::fflush(stderr);
        }
        return result;
    }
    const int64_t plan_ms = elapsed_ms(plan_start);

    const steady_clock::time_point query_plan_start = steady_clock::now();
    atmcore::SearchQueryTokens query_stage_tokens;
    if (!atmcore::build_query_stage_tokens(
            context.command.core,
            context.command.runtime,
            system,
            input.query_context,
            input.query,
            query_stage_tokens,
            result.error)) {
        if (context.command.run_log) {
            std::fprintf(
                stderr,
                "oneshot-search failed stage=query-plan segments=%zu memories=%zu elapsed_ms=%lld error=\"%s\"\n",
                session.segment_plan.segments.size(),
                memories.size(),
                (long long) elapsed_ms(total_start),
                result.error.c_str());
            std::fflush(stderr);
        }
        return result;
    }
    const int64_t query_plan_ms = elapsed_ms(query_plan_start);

    if (context.command.run_log) {
        std::fprintf(
            stderr,
            "oneshot-search begin memories=%zu segments=%zu plan_ms=%lld query_plan_ms=%lld context_tokens=%zu query_tokens=%zu query_stage_capacity_tokens=%zu v=%zu\n",
            memories.size(),
            session.segment_plan.segments.size(),
            (long long) plan_ms,
            (long long) query_plan_ms,
            query_stage_tokens.context_tokens.size(),
            query_stage_tokens.query_tokens.size(),
            query_stage_context_token_count(query_stage_tokens),
            atmcore::runtime_info(context.command.core).internal.v.size());
        std::fflush(stderr);
    }

    std::vector<AttentionMemoryRef> ordered_memories;
    SegmentSearchRunner runner(context, kv_manager, session, query_stage_tokens);
    result.ok = runner.run_ephemeral(system, ordered_memories, result.error);
    std::string cleanup_error;
    const bool cleanup_ok = kv_manager.release_all_live_contexts(cleanup_error);
    if (!cleanup_ok && context.command.run_log) {
        std::fprintf(
            stderr,
            "oneshot-search cleanup failed segments=%zu elapsed_ms=%lld error=\"%s\"\n",
            session.segment_plan.segments.size(),
            (long long) elapsed_ms(total_start),
            cleanup_error.c_str());
        std::fflush(stderr);
    }
    if (!result.ok) {
        if (context.command.run_log) {
            std::fprintf(
                stderr,
                "oneshot-search finished ok=false segments=%zu elapsed_ms=%lld error=\"%s\"\n",
                session.segment_plan.segments.size(),
                (long long) elapsed_ms(total_start),
                result.error.c_str());
            std::fflush(stderr);
        }
        return result;
    }
    if (!cleanup_ok) {
        result.ok = false;
        result.error = cleanup_error;
        result.error_info = make_error_info(ErrorCode::InternalError, cleanup_error);
        return result;
    }

    const steady_clock::time_point rank_start = steady_clock::now();
    const std::vector<AttentionMemoryRef> limited_memories =
        apply_search_result_limits(ordered_memories, search_config);
    const int64_t rank_ms = elapsed_ms(rank_start);

    result.payload = ResultPayload::OneShotSearchOrdered;
    result.oneshot_search_memories.reserve(limited_memories.size());
    for (const AttentionMemoryRef & memory : limited_memories) {
        const persistent::MemoryRecord * record = persistent::find_memory(session.store, memory.memory_index);
        if (record == nullptr) {
            continue;
        }
        if (memory.memory_index < 0 || (size_t) memory.memory_index >= memories.size()) {
            continue;
        }

        OneShotSearchMemory item;
        item.has_id = memories[(size_t) memory.memory_index].has_id;
        item.id = memories[(size_t) memory.memory_index].id;
        item.text = record->text;
        item.segment_id = memory.segment_id;
        result.oneshot_search_memories.push_back(std::move(item));
    }

    if (context.command.run_log) {
        std::fprintf(
            stderr,
            "oneshot-search finished ok=true segments=%zu results=%zu rank_ms=%lld total_ms=%lld\n",
            session.segment_plan.segments.size(),
            result.oneshot_search_memories.size(),
            (long long) rank_ms,
            (long long) elapsed_ms(total_start));
        std::fflush(stderr);
    }

    return result;
}

} // namespace attemory::context
