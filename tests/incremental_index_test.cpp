#include "context/context.h"

#include "tests/test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<int32_t> parse_v(const char * value) {
    std::vector<int32_t> result;
    if (value == nullptr || *value == '\0') {
        return result;
    }

    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (item.empty()) {
            continue;
        }
        result.push_back(std::stoi(item));
    }
    return result;
}

attemory::context::MemoryInput memory(std::string text) {
    attemory::context::MemoryInput input;
    input.text = std::move(text);
    return input;
}

void require_ok(const attemory::context::CommandResult & result, const char * command) {
    if (!result.ok) {
        std::cerr << command << " failed: " << result.error << "\n";
        EXPECT_TRUE(result.ok);
    }
}

std::vector<AttentionMemoryRef> build_incremental_result(
    attemory::context::AttemoryContext & context,
    const std::string & session_id,
    const std::string & system,
    const std::string & first_memory,
    const std::string & second_memory,
    const std::string & query) {
    require_ok(context.create_session(session_id), "create incremental session");
    require_ok(context.add_system(session_id, system), "add incremental system");
    require_ok(context.add_memory(session_id, memory(first_memory)), "add first incremental memory");
    require_ok(context.index_session(session_id), "index first incremental memory");
    require_ok(context.add_memory(session_id, memory(second_memory)), "add second incremental memory");
    require_ok(context.index_session(session_id), "index appended incremental memory");

    attemory::context::SearchRequestOverrides overrides;
    overrides.top_k = 2;
    const attemory::context::CommandResult result =
        context.search(session_id, query, overrides);
    require_ok(result, "search incremental session");
    return result.ordered_search_memories;
}

std::vector<AttentionMemoryRef> build_full_result(
    attemory::context::AttemoryContext & context,
    const std::string & session_id,
    const std::string & system,
    const std::string & first_memory,
    const std::string & second_memory,
    const std::string & query) {
    require_ok(context.create_session(session_id), "create full session");
    require_ok(context.add_system(session_id, system), "add full system");
    require_ok(context.add_memory(session_id, memory(first_memory)), "add first full memory");
    require_ok(context.add_memory(session_id, memory(second_memory)), "add second full memory");
    require_ok(context.index_session(session_id), "index full session");

    attemory::context::SearchRequestOverrides overrides;
    overrides.top_k = 2;
    const attemory::context::CommandResult result =
        context.search(session_id, query, overrides);
    require_ok(result, "search full session");
    return result.ordered_search_memories;
}

void expect_same_results(
    const std::vector<AttentionMemoryRef> & incremental,
    const std::vector<AttentionMemoryRef> & full) {
    EXPECT_TRUE(!incremental.empty());
    EXPECT_EQ(incremental.size(), full.size());
    const size_t limit = std::min(incremental.size(), full.size());
    for (size_t i = 0; i < limit; ++i) {
        EXPECT_EQ(incremental[i].memory_index, full[i].memory_index);
        EXPECT_EQ(incremental[i].segment_id, full[i].segment_id);
    }
}

} // namespace

int main() {
    const char * model_path = std::getenv("ATTEMORY_TEST_MODEL");
    const std::vector<int32_t> v = parse_v(std::getenv("ATTEMORY_TEST_V"));
    if (model_path == nullptr || *model_path == '\0' || v.empty()) {
        std::cout << "skip: set ATTEMORY_TEST_MODEL and ATTEMORY_TEST_V to run incremental index test\n";
        return 0;
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "attemory-incremental-index-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    attemory::context::ContextOptions options;
    options.model_path = model_path;
    options.data_dir = (root / "sessions").string();
    options.cache_dir = (root / "cache").string();
    options.runtime.backend = atmcore::BackendKind::CPU;
    options.runtime.kv_type_k = atmcore::KvType::F16;
    options.runtime.kv_type_v = atmcore::KvType::F16;
    options.search.top_k = 2;
    options.v = v;
    options.truncate = true;

    attemory::context::AttemoryContext context;
    std::string error;
    if (!context.init(options, error)) {
        std::cerr << "context init failed: " << error << "\n";
        EXPECT_TRUE(false);
        return attemory::test::test_main_result("incremental index test");
    }

    const std::string system =
        "Read the following memories carefully and find the most relevant memory to the query.";
    const std::string first_memory =
        "The blue key is inside the ceramic bowl on the kitchen counter";
    const std::string second_memory =
        "The red key is under the desk in the study";
    const std::string query = "Where is the blue key stored?";

    const std::vector<AttentionMemoryRef> incremental =
        build_incremental_result(context, "incremental", system, first_memory, second_memory, query);
    const std::vector<AttentionMemoryRef> full =
        build_full_result(context, "full", system, first_memory, second_memory, query);
    expect_same_results(incremental, full);

    context.shutdown();
    std::filesystem::remove_all(root);
    return attemory::test::test_main_result("incremental index test");
}
