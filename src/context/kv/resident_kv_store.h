#pragma once

#include "attemory-core/attemory-core.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace attemory::context::kv {

struct SegmentKVState {
    atmcore::KVSnapshot snapshot;
    atmcore::ActiveKV active;
    bool has_snapshot = false;
};

using SegmentKVHandle = std::shared_ptr<SegmentKVState>;

struct ActiveSegmentKV {
    std::string session_id;
    int32_t segment_id = -1;
    SegmentKVHandle handle;
};

struct ResidentKVEntry {
    SegmentKVHandle handle;
    uint64_t bytes = 0;
    uint64_t last_access = 0;
};

using ResidentSegmentKVMap = std::unordered_map<int32_t, ResidentKVEntry>;
using ResidentSessionKVMap = std::unordered_map<std::string, ResidentSegmentKVMap>;

class ResidentKVStore {
public:
    void set_budget_bytes(uint64_t bytes);
    uint64_t budget_bytes() const;
    uint64_t resident_bytes() const;

    SegmentKVHandle find(const std::string & session_id, int32_t segment_id);
    SegmentKVHandle peek(const std::string & session_id, int32_t segment_id) const;

    bool store(
        const std::string & session_id,
        int32_t segment_id,
        const SegmentKVHandle & handle,
        std::string & error);

    void remove(const std::string & session_id, int32_t segment_id);
    void clear_session(const std::string & session_id);
    void clear();
    int32_t count_session(const std::string & session_id) const;

    bool release_all_live_contexts(std::string & error);
    bool keep_only_session_live(const std::string & session_id, std::string & error);
    bool keep_only_segment_live(const std::string & session_id, int32_t segment_id, std::string & error);

private:
    bool evict_to_budget(const std::string & protected_session_id, int32_t protected_segment_id, std::string & error);
    bool refresh_entry_bytes(ResidentKVEntry & entry, std::string & error);
    void touch(ResidentKVEntry & entry);

    ResidentSessionKVMap resident_segments_;
    uint64_t budget_bytes_ = 0;
    uint64_t resident_bytes_ = 0;
    uint64_t access_clock_ = 0;
};

SegmentKVHandle make_segment_kv_handle();
bool segment_kv_has_live_context(const SegmentKVHandle & handle);
bool segment_kv_has_snapshot_blob(const SegmentKVHandle & handle);
const atmcore::KVMetadata * segment_kv_metadata(const SegmentKVHandle & handle);
size_t segment_kv_base_token_count(const SegmentKVHandle & handle);
bool ensure_segment_kv_snapshot(const SegmentKVHandle & handle, std::string & error);
void release_segment_kv_context(const SegmentKVHandle & handle);

struct ContextKVState {
    ResidentKVStore resident;
    ActiveSegmentKV active_segment;
    ResidentSessionKVMap incremental_prefixes;
};

} // namespace attemory::context::kv
