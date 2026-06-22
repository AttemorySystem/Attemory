#include "persistent/persistent.h"

#include "tests/test_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

attemory::persistent::MemoryRecord memory_record(
    attemory::persistent::MemoryIndex memory_idx,
    const std::string & id,
    const std::string & text,
    int32_t estimated_tokens) {
    attemory::persistent::MemoryRecord record;
    record.memory_idx = memory_idx;
    record.has_id = !id.empty();
    record.id = id;
    record.text = text;
    record.estimated_tokens = estimated_tokens;
    return record;
}

attemory::persistent::SessionStore demo_store() {
    attemory::persistent::SessionStore store;
    store.session_id = "demo";
    store.system_text = "system prompt";
    store.system_locked = true;
    store.kv_persist = true;
    store.next_memory_idx = 3;
    store.memories.push_back(memory_record(0, "m0", "first memory", 11));
    store.memories.push_back(memory_record(2, "", "second memory", 13));
    store.manual_segment_boundaries = {2};
    return store;
}

attemory::persistent::StorageLayout layout_for(
    const std::filesystem::path & root,
    const std::string & session_id) {
    attemory::persistent::StorageLayout layout;
    layout.root_dir = root.string();
    layout.session_id = session_id;
    return layout;
}

void require_ok(const attemory::persistent::ResultStatus & status, const char * operation) {
    if (!status.ok) {
        std::cerr << operation << " failed: " << status.error << "\n";
        EXPECT_TRUE(status.ok);
    }
}

void test_save_load_and_scan_session_store() {
    attemory::test::TempDir root("attemory-persistent-session-test");
    const attemory::persistent::StorageLayout layout = layout_for(root.path(), "demo");
    const attemory::persistent::SessionStore store = demo_store();

    require_ok(attemory::persistent::save_session(layout, store), "save_session");

    attemory::persistent::SessionStore loaded;
    require_ok(attemory::persistent::load_session(layout, loaded), "load_session");

    EXPECT_EQ(loaded.session_id, "demo");
    EXPECT_EQ(loaded.system_text, "system prompt");
    EXPECT_TRUE(loaded.system_locked);
    EXPECT_TRUE(loaded.kv_persist);
    EXPECT_EQ(loaded.next_memory_idx, 3);
    EXPECT_EQ(loaded.memories.size(), static_cast<size_t>(2));
    EXPECT_EQ(loaded.memories[0].memory_idx, 0);
    EXPECT_TRUE(loaded.memories[0].has_id);
    EXPECT_EQ(loaded.memories[0].id, "m0");
    EXPECT_EQ(loaded.memories[0].text, "first memory");
    EXPECT_EQ(loaded.memories[0].estimated_tokens, 11);
    EXPECT_EQ(loaded.memories[1].memory_idx, 2);
    EXPECT_FALSE(loaded.memories[1].has_id);
    EXPECT_EQ(loaded.manual_segment_boundaries.size(), static_cast<size_t>(1));
    EXPECT_EQ(loaded.manual_segment_boundaries[0], 2);

    attemory::persistent::StorageLayout root_layout;
    root_layout.root_dir = root.path().string();
    attemory::persistent::SessionStoreMap sessions;
    require_ok(attemory::persistent::scan_sessions(root_layout, sessions), "scan_sessions");
    EXPECT_EQ(sessions.size(), static_cast<size_t>(1));
    EXPECT_TRUE(sessions.find("demo") != sessions.end());
}

void test_rejects_invalid_stores_before_writing() {
    attemory::test::TempDir root("attemory-persistent-invalid-test");

    attemory::persistent::SessionStore invalid_id = demo_store();
    invalid_id.session_id = "../bad";
    attemory::persistent::ResultStatus status =
        attemory::persistent::save_session(layout_for(root.path(), "../bad"), invalid_id);
    EXPECT_FALSE(status.ok);
    EXPECT_CONTAINS(status.error, "invalid session id");

    attemory::persistent::SessionStore duplicate = demo_store();
    duplicate.memories[1].memory_idx = duplicate.memories[0].memory_idx;
    status = attemory::persistent::save_session(layout_for(root.path(), "demo"), duplicate);
    EXPECT_FALSE(status.ok);
    EXPECT_CONTAINS(status.error, "duplicate memory index");

    attemory::persistent::SessionStore bad_boundary = demo_store();
    bad_boundary.manual_segment_boundaries = {4};
    status = attemory::persistent::save_session(layout_for(root.path(), "demo"), bad_boundary);
    EXPECT_FALSE(status.ok);
    EXPECT_CONTAINS(status.error, "invalid manual segment boundary");
}

void test_load_rejects_corrupt_metadata() {
    attemory::test::TempDir root("attemory-persistent-corrupt-test");
    const attemory::persistent::StorageLayout layout = layout_for(root.path(), "demo");
    std::filesystem::create_directories(attemory::persistent::session_dir_path(layout));
    {
        std::ofstream out(attemory::persistent::session_meta_path(layout), std::ios::binary);
        out << "not a session store";
    }

    attemory::persistent::SessionStore loaded;
    const attemory::persistent::ResultStatus status =
        attemory::persistent::load_session(layout, loaded);
    EXPECT_FALSE(status.ok);
    EXPECT_CONTAINS(status.error, "unsupported session store format");
}

} // namespace

int main() {
    test_save_load_and_scan_session_store();
    test_rejects_invalid_stores_before_writing();
    test_load_rejects_corrupt_metadata();
    return attemory::test::test_main_result("persistent session test");
}
