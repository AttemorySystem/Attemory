#include "context/session/session_status.h"

#include "tests/test_support.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

attemory::context::Session disk_cached_session(const std::filesystem::path & root) {
    attemory::context::Session session;
    session.store.session_id = "demo";
    session.store.kv_persist = true;
    session.store.next_memory_idx = 1;

    attemory::persistent::MemoryRecord memory;
    memory.memory_idx = 0;
    memory.text = "memory";
    session.store.memories.push_back(memory);

    session.segment_plan.session_id = "demo";
    session.segment_plan.config.model_cache_key = "model-key";

    attemory::context::Segment segment;
    segment.segment_id = 0;
    segment.memory_indices = {0};
    segment.exact_tokens = 17;
    session.segment_plan.segments.push_back(segment);

    const std::filesystem::path kv_path = root / "segment-0.kv";
    {
        std::ofstream out(kv_path, std::ios::binary);
        out << "kv";
    }

    attemory::persistent::KVCacheEntry cache;
    cache.state = attemory::persistent::CacheState::DiskOnly;
    cache.kv_path = kv_path.string();
    cache.token_count = 17;
    cache.bytes_on_disk = 2;
    session.segment_kv_metadata[0] = cache;
    return session;
}

void test_disk_cached_segments_count_as_indexed() {
    attemory::test::TempDir root("attemory-session-status-test");

    attemory::persistent::ModelCacheKey model;
    model.value = "model-key";

    attemory::context::SessionMap sessions;
    sessions.emplace("demo", disk_cached_session(root.path()));

    atmcore::Runtime * core = nullptr;
    attemory::context::kv::ContextKVState kv_state;
    attemory::context::kv::SegmentKVManager kv_manager(core, kv_state);

    const std::vector<attemory::context::SessionStatus> statuses =
        attemory::context::list_session_statuses(root.path(), model, sessions, kv_manager);

    EXPECT_EQ(statuses.size(), static_cast<size_t>(1));
    const attemory::context::SessionStatus & status = statuses[0];
    EXPECT_EQ(status.session_id, "demo");
    EXPECT_TRUE(status.kv_persist);
    EXPECT_EQ(status.resident_segments, 0);
    EXPECT_EQ(status.saved_segments, 1);
    EXPECT_EQ(status.indexed_segments, 1);
    EXPECT_TRUE(status.indexed);
    EXPECT_TRUE(status.disk_cached);
    EXPECT_EQ(status.total_tokens, 17);
}

} // namespace

int main() {
    test_disk_cached_segments_count_as_indexed();
    return attemory::test::test_main_result("session status test");
}
