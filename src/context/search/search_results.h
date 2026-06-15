#pragma once

#include "attemory-core/attemory-core.h"
#include "context/context_types.h"
#include "context/session/session_state.h"

#include <string>
#include <vector>

namespace attemory::context {

void merge_segment_ranked_results(
    const persistent::SessionStore & store,
    const Segment & segment,
    const std::vector<atmcore::RankedMemoryIndex> & segment_ranked_memories,
    std::vector<AttentionMemoryRef> & ordered_memories);

} // namespace attemory::context
