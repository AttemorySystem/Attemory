#pragma once

#include "context/context_types.h"
#include "context/kv/segment_kv_manager.h"

namespace attemory::context {

struct Session;

SessionSummary summarize_session(
    kv::SegmentKVManager & kv_manager,
    const Session * session);

} // namespace attemory::context
