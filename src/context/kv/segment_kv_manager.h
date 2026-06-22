#pragma once

#include "context/context_types.h"
#include "context/kv/resident_kv_store.h"
#include "context/session/session_state.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace attemory::context {
struct KVCacheCommandContext;
}

namespace attemory::context::kv {

struct SegmentKVStatus {
    attemory::persistent::SegmentId segment_id = attemory::persistent::kInvalidSegmentId;
    bool has_disk_snapshot = false;
    bool has_resident_snapshot = false;
    bool has_resident_state_blob = false;
    bool has_live_context = false;
    bool is_active = false;
    int32_t token_count = 0;
    int64_t bytes_on_disk = 0;
};

class SegmentKVManager {
public:
    SegmentKVManager(
        atmcore::Runtime *& core,
        ContextKVState & kv);

    SegmentKVStatus status(
        const attemory::context::Session & session,
        attemory::persistent::SegmentId segment_id,
        const ActiveSegmentKV * active = nullptr) const;

    bool prepare_resident_segment(
        attemory::context::Session & session,
        const atmcore::RuntimeOptions & runtime,
        attemory::persistent::SegmentId segment_id,
        SegmentKVHandle & resident,
        std::string & error);

    bool activate_for_search(
        attemory::context::Session & session,
        const atmcore::RuntimeOptions & runtime,
        attemory::persistent::SegmentId segment_id,
        size_t query_stage_token_count,
        bool run_log,
        std::string & error);

    void clear_active();
    void evict_active_for_command(AttemoryCommand command, const std::string & session_id);
    void evict_session(const std::string & session_id);
    void clear_resident_session(const std::string & session_id);
    void remove_resident_segment(const std::string & session_id, int32_t segment_id);
    void stage_incremental_prefix(const std::string & session_id, int32_t segment_id);
    int32_t resident_segment_count(const std::string & session_id) const;
    bool release_all_live_contexts(std::string & error);
    bool keep_only_session_live(const std::string & session_id, std::string & error);
    void sync_active_search_cache_to_resident();
    void clear_all();
    void set_resident_budget_bytes(uint64_t bytes);
    uint64_t resident_budget_bytes() const;
    uint64_t resident_bytes() const;

    CommandResult index_session_kv_cache(
        attemory::context::KVCacheCommandContext context,
        const std::string & session_id);
    CommandResult save_session_kv_cache(
        attemory::context::KVCacheCommandContext context,
        const std::string & session_id);

private:
    bool prepare_resident_segment(
        attemory::context::Session & session,
        const atmcore::RuntimeOptions & runtime,
        const attemory::context::Segment & segment,
        SegmentKVHandle & resident,
        std::string & error);
    bool try_prepare_incremental_resident_segment(
        attemory::context::Session & session,
        const atmcore::RuntimeOptions & runtime,
        const attemory::context::Segment & segment,
        const std::vector<std::string> & memories,
        SegmentKVHandle & resident,
        std::string & error);

    bool keep_only_segment_live(
        const atmcore::RuntimeOptions & runtime,
        const std::string & session_id,
        int32_t segment_id,
        std::string & error);

    atmcore::Runtime *& core_;
    ContextKVState & kv_;
};

} // namespace attemory::context::kv
