#include "context/session/session_summary.h"

namespace attemory::context {

SessionSummary summarize_session(
    kv::SegmentKVManager & kv_manager,
    const Session * session) {
    SessionSummary summary;
    if (session == nullptr) {
        return summary;
    }

    summary.memory_count = (int32_t) session->store.memories.size();
    summary.segment_count = (int32_t) session->segment_plan.segments.size();
    for (const Segment & segment : session->segment_plan.segments) {
        const kv::SegmentKVStatus kv_status =
            kv_manager.status(*session, segment.segment_id);
        if (kv_status.has_resident_snapshot) {
            ++summary.resident_segments;
            ++summary.indexed_segments;
        }
        if (kv_status.has_disk_snapshot) {
            ++summary.saved_segments;
        }
    }

    return summary;
}

} // namespace attemory::context
