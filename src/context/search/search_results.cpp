#include "context/search/search_results.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace attemory::context {
namespace {

bool memory_ref_exists(const std::vector<AttentionMemoryRef> & memories, int32_t memory_index) {
    for (const AttentionMemoryRef & memory : memories) {
        if (memory.memory_index == memory_index) {
            return true;
        }
    }
    return false;
}

bool segment_memory_ref(
    const Segment & segment,
    int32_t local_memory_index,
    AttentionMemoryRef & memory) {
    if (local_memory_index < 0 || (size_t) local_memory_index >= segment.memory_indices.size()) {
        return false;
    }

    const persistent::MemoryIndex global_memory_index64 = segment.memory_indices[(size_t) local_memory_index];
    if (global_memory_index64 < 0 || global_memory_index64 > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    memory.memory_index = (int32_t) global_memory_index64;
    memory.segment_id = segment.segment_id;
    return true;
}

} // namespace

void merge_segment_ranked_results(
    const persistent::SessionStore & store,
    const Segment & segment,
    const std::vector<atmcore::RankedMemoryIndex> & segment_ranked_memories,
    std::vector<AttentionMemoryRef> & ordered_memories) {
    for (const atmcore::RankedMemoryIndex & item : segment_ranked_memories) {
        AttentionMemoryRef memory;
        if (!segment_memory_ref(segment, item.memory_index, memory)) {
            continue;
        }
        if (persistent::find_memory(store, memory.memory_index) == nullptr) {
            continue;
        }
        if (memory_ref_exists(ordered_memories, memory.memory_index)) {
            continue;
        }

        ordered_memories.push_back(memory);
    }
}

} // namespace attemory::context
