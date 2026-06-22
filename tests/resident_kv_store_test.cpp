#include "context/kv/resident_kv_store.h"
#include "context/kv/segment_kv_manager.h"

#include "tests/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

int fake_context = 0;
int fake_base_kv = 0;
int fake_release_count = 0;

void fake_release(void *, void *) {
    ++fake_release_count;
}

bool fake_state_size(const void *, const void *, uint64_t & bytes, std::string &) {
    bytes = 1024ull * 1024ull;
    return true;
}

bool fake_encode(const void *, const void *, std::vector<uint8_t> & bytes, std::string &) {
    bytes = {1, 2, 3, 4};
    return true;
}

attemory::context::kv::SegmentKVHandle live_kv_with_snapshot() {
    attemory::context::kv::SegmentKVHandle handle =
        attemory::context::kv::make_segment_kv_handle();
    handle->active.ctx = &fake_context;
    handle->active.base_kv = &fake_base_kv;
    handle->active.release_context = fake_release;
    handle->active.encode_context = fake_encode;
    handle->active.state_size_context = fake_state_size;
    handle->has_snapshot = true;
    handle->snapshot.base_seq_state_blob = {0, 0, 0, 0};
    return handle;
}

void test_resident_budget_counts_snapshot_ram_not_live_context() {
    attemory::context::kv::ResidentKVStore store;
    store.set_budget_bytes(8);

    std::string error;
    attemory::context::kv::SegmentKVHandle handle = live_kv_with_snapshot();
    EXPECT_TRUE(store.store("s", 0, handle, error));

    EXPECT_EQ(store.resident_bytes(), static_cast<uint64_t>(4));
    EXPECT_TRUE(store.peek("s", 0) != nullptr);

    attemory::context::kv::SegmentKVHandle live_only =
        attemory::context::kv::make_segment_kv_handle();
    live_only->active.ctx = &fake_context;
    live_only->active.base_kv = &fake_base_kv;
    live_only->active.release_context = fake_release;
    live_only->active.state_size_context = fake_state_size;
    EXPECT_TRUE(store.store("s", 1, live_only, error));

    EXPECT_EQ(store.resident_bytes(), static_cast<uint64_t>(4));
    EXPECT_TRUE(store.peek("s", 1) != nullptr);
}

void test_keep_only_segment_live_releases_other_segments() {
    fake_release_count = 0;

    attemory::context::kv::ResidentKVStore store;
    std::string error;
    attemory::context::kv::SegmentKVHandle first = live_kv_with_snapshot();
    attemory::context::kv::SegmentKVHandle second = live_kv_with_snapshot();

    EXPECT_TRUE(store.store("s", 0, first, error));
    EXPECT_TRUE(store.store("s", 1, second, error));

    EXPECT_TRUE(store.keep_only_segment_live("s", 1, error));
    EXPECT_EQ(fake_release_count, 1);
    EXPECT_TRUE(!attemory::context::kv::segment_kv_has_live_context(first));
    EXPECT_TRUE(attemory::context::kv::segment_kv_has_live_context(second));
}

void test_clear_active_releases_live_context() {
    fake_release_count = 0;

    atmcore::Runtime * core = nullptr;
    attemory::context::kv::ContextKVState kv_state;
    attemory::context::kv::SegmentKVManager manager(core, kv_state);
    attemory::context::kv::SegmentKVHandle active = live_kv_with_snapshot();
    kv_state.active_segment.session_id = "s";
    kv_state.active_segment.segment_id = 0;
    kv_state.active_segment.handle = active;

    manager.clear_active();

    EXPECT_EQ(fake_release_count, 1);
    EXPECT_TRUE(kv_state.active_segment.handle == nullptr);
    EXPECT_TRUE(!attemory::context::kv::segment_kv_has_live_context(active));
}

} // namespace

int main() {
    test_resident_budget_counts_snapshot_ram_not_live_context();
    test_keep_only_segment_live_releases_other_segments();
    test_clear_active_releases_live_context();
    return attemory::test::test_main_result("resident kv store test");
}
