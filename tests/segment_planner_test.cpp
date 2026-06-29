#include "context/kv/segment_kv_metadata.h"
#include "context/session/segment_planner.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"
#include "tests/test_support.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

attemory::persistent::ModelCacheKey model_key() {
    attemory::persistent::ModelCacheKey key;
    key.value = "model-key";
    key.vendor = "vendor";
    key.model_desc = "model";
    key.model_name = "model";
    key.backend = "cpu";
    key.template_hash = "template";
    key.kv_type_k = "f16";
    key.kv_type_v = "f16";
    return key;
}

attemory::persistent::MemoryRecord memory(
    attemory::persistent::MemoryIndex index,
    const std::string & text) {
    attemory::persistent::MemoryRecord record;
    record.memory_idx = index;
    record.text = text;
    record.estimated_tokens = 10;
    return record;
}

void write_fake_kv_file(const std::filesystem::path & path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "kv";
}

void test_manifest_restore_accepts_segments_above_current_soft_limit() {
    attemory::test::TempDir root("attemory-segment-planner-test");
    const attemory::persistent::ModelCacheKey key = model_key();

    attemory::context::Session session;
    session.store.session_id = "legacy";
    session.store.system_text = "system";
    session.store.kv_persist = true;
    session.store.next_memory_idx = 2;
    session.store.memories.push_back(memory(0, "first memory"));
    session.store.memories.push_back(memory(1, "second memory"));

    attemory::context::Segment segment;
    segment.segment_id = 0;
    segment.memory_indices = {0, 1};

    const attemory::persistent::CacheLayout layout =
        attemory::context::cache_layout_for_session(root.path(), key, session.store.session_id);
    const std::filesystem::path kv_path =
        attemory::persistent::cache_segment_kv_path(layout, segment.segment_id);
    write_fake_kv_file(kv_path);

    attemory::persistent::CacheManifest manifest;
    manifest.model = key;
    manifest.session_id = session.store.session_id;

    attemory::persistent::SegmentCacheManifest manifest_segment;
    manifest_segment.segment_id = segment.segment_id;
    manifest_segment.first_memory_idx = 0;
    manifest_segment.last_memory_idx_exclusive = 2;
    manifest_segment.segment_content_hash =
        attemory::context::segment_kv_content_hash(session.store, segment);
    manifest_segment.kv_file = kv_path.string();
    manifest_segment.token_count = 90;
    manifest_segment.bytes = 2;
    manifest.segments.push_back(manifest_segment);

    const attemory::persistent::ResultStatus save_status =
        attemory::persistent::save_kv_cache_manifest(layout, manifest);
    EXPECT_TRUE(save_status.ok);

    atmcore::RuntimeOptions runtime;
    runtime.n_ctx = 100;
    runtime.n_ctx_is_ceiling = true;

    std::string error;
    const bool ok = attemory::context::initialize_session_plan(
        nullptr,
        runtime,
        root.path(),
        key,
        session,
        false,
        error);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(session.segment_plan.segments.size(), static_cast<size_t>(1));
    EXPECT_EQ(session.segment_plan.segments[0].segment_id, 0);
    EXPECT_EQ(session.segment_plan.segments[0].exact_tokens, 90);
    EXPECT_EQ(session.segment_plan.segments[0].memory_indices.size(), static_cast<size_t>(2));
    EXPECT_TRUE(session.segment_kv_metadata.find(0) != session.segment_kv_metadata.end());
    EXPECT_EQ(session.segment_kv_metadata[0].state, attemory::persistent::CacheState::DiskOnly);
}

} // namespace

int main() {
    test_manifest_restore_accepts_segments_above_current_soft_limit();
    return attemory::test::test_main_result("segment planner test");
}
