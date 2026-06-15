#pragma once

#include "attemory-core/attemory-core.h"
#include "context/attemory_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace attemory::context {

using RuntimeOptions = atmcore::RuntimeOptions;

inline constexpr int32_t kNoSegmentId = -1;

struct SearchConfig {
    int32_t candidate_top_k = 25;
    int32_t top_k = 0;
};

// Request-level optional overrides. -1 means the request did not set the field.
// Convert this to a SearchConfig at the API boundary before running search.
struct SearchRequestOverrides {
    // Per-segment result limit.
    int32_t top_k = -1;
};

struct SearchInput {
    std::string query_context;
    std::string query;
};

struct ContextOptions {
    std::string model_path;
    std::string model_tier;
    std::string data_dir = "sessions";
    std::string cache_dir = "cache";
    RuntimeOptions runtime;
    SearchConfig search;
    std::vector<int32_t> v;
    uint64_t resident_kv_budget_bytes = 0;
    bool truncate = false;
    bool run_log = false;
};

struct StartupInfo {
    RuntimeOptions runtime;
    std::string model_cache_key;

    int32_t startup_n_ctx = 0;
    std::string startup_n_ctx_source;

    bool truncate = false;

    size_t restored_sessions = 0;
    uint64_t resident_kv_budget_bytes = 0;
    std::vector<int32_t> v;
    SearchConfig search;
};

struct TokenUsage {
    int32_t token_count = -1;
    int32_t ctx_length = 0;
    int32_t segment_id = -1;
    int32_t segment_count = 0;
    int32_t memory_index = -1;
    bool has_memory_id = false;
    std::string memory_id;
};

struct SessionSummary {
    int32_t memory_count = 0;
    int32_t segment_count = 0;
    int32_t resident_segments = 0;
    int32_t saved_segments = 0;
    int32_t indexed_segments = 0;
};

struct SessionStatus {
    std::string session_id;
    int32_t memory_count = 0;
    int32_t segment_count = 0;
    int64_t total_tokens = 0;
    int32_t resident_segments = 0;
    int32_t indexed_segments = 0;
    int32_t saved_segments = 0;
    bool indexed = false;
    bool disk_cached = false;
    bool plan_ready = false;
    bool facts_dirty = false;
};

struct MemoryInput {
    bool has_id = false;
    std::string id;
    std::string text;
};

using OneShotMemoryInput = MemoryInput;

struct OneShotSearchMemory {
    bool has_id = false;
    std::string id;
    std::string text;
    int32_t segment_id = -1;
};

enum class ErrorCode {
    BadRequest,
    InvalidRequest,
    InvalidSessionId,
    SessionNotFound,
    SessionAlreadyExists,
    NotFound,
    UnsupportedMediaType,
    PayloadTooLarge,
    InternalError,
};

struct ErrorDetail {
    std::string key;
    std::string value;
};

struct ErrorInfo {
    ErrorCode code = ErrorCode::BadRequest;
    std::string message;
    std::vector<ErrorDetail> details;
};

enum class ResultPayload {
    None,
    TokenUsage,
    RestoreSummary,
    SaveSummary,
    Search,
    SearchOrdered,
    OneShotSearchOrdered,
};

struct CommandResult {
    bool ok = false;
    std::string error;
    ErrorInfo error_info;
    ResultPayload payload = ResultPayload::None;

    TokenUsage token_usage;

    bool restored = false;
    SessionSummary summary;

    std::vector<AttentionMemoryRef> ordered_search_memories;
    std::vector<OneShotSearchMemory> oneshot_search_memories;
};

} // namespace attemory::context
