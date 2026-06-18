#include "context/search/search_results.h"

#include "tests/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

attemory::persistent::MemoryRecord memory_record(
    attemory::persistent::MemoryIndex memory_idx,
    const std::string & text) {
    attemory::persistent::MemoryRecord record;
    record.memory_idx = memory_idx;
    record.text = text;
    return record;
}

atmcore::RankedMemoryIndex ranked(int32_t local_memory_index, double score = 1.0) {
    atmcore::RankedMemoryIndex item;
    item.memory_index = local_memory_index;
    item.score = score;
    return item;
}

void test_merges_valid_local_indices_to_global_memory_refs() {
    attemory::persistent::SessionStore store;
    store.memories.push_back(memory_record(10, "first"));
    store.memories.push_back(memory_record(11, "second"));

    attemory::context::Segment segment;
    segment.segment_id = 4;
    segment.memory_indices = {10, 11};

    std::vector<AttentionMemoryRef> ordered;
    attemory::context::merge_segment_ranked_results(
        store,
        segment,
        {ranked(1, 0.9), ranked(0, 0.8)},
        ordered);

    EXPECT_EQ(ordered.size(), static_cast<size_t>(2));
    EXPECT_EQ(ordered[0].memory_index, 11);
    EXPECT_EQ(ordered[0].segment_id, 4);
    EXPECT_EQ(ordered[1].memory_index, 10);
    EXPECT_EQ(ordered[1].segment_id, 4);
}

void test_ignores_invalid_missing_and_duplicate_memory_refs() {
    attemory::persistent::SessionStore store;
    store.memories.push_back(memory_record(1, "one"));
    store.memories.push_back(memory_record(2, "two"));

    attemory::context::Segment segment;
    segment.segment_id = 3;
    segment.memory_indices = {1, 99, -1, 2};

    std::vector<AttentionMemoryRef> ordered;
    ordered.push_back({2, 9});

    attemory::context::merge_segment_ranked_results(
        store,
        segment,
        {
            ranked(-1),
            ranked(4),
            ranked(1),
            ranked(2),
            ranked(3),
            ranked(0),
        },
        ordered);

    EXPECT_EQ(ordered.size(), static_cast<size_t>(2));
    EXPECT_EQ(ordered[0].memory_index, 2);
    EXPECT_EQ(ordered[0].segment_id, 9);
    EXPECT_EQ(ordered[1].memory_index, 1);
    EXPECT_EQ(ordered[1].segment_id, 3);
}

} // namespace

int main() {
    test_merges_valid_local_indices_to_global_memory_refs();
    test_ignores_invalid_missing_and_duplicate_memory_refs();
    return attemory::test::test_main_result("search results test");
}
