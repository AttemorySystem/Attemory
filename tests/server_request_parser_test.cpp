#include "server/request_parser.h"

#include "context/command/command_result.h"
#include "tests/test_support.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using attemory::context::ErrorCode;
using attemory::context::ErrorInfo;

std::string detail_value(const ErrorInfo & error, const std::string & key) {
    for (const auto & detail : error.details) {
        if (detail.key == key) {
            return detail.value;
        }
    }
    return std::string();
}

void test_memory_payload_schema() {
    attemory::context::MemoryInput memory;
    ErrorInfo error;

    EXPECT_TRUE(attemory::server::parse_memory_payload(
        R"({"id":"external-id","text":"memory"})",
        memory,
        error));
    EXPECT_TRUE(memory.has_id);
    EXPECT_EQ(memory.id, "external-id");
    EXPECT_EQ(memory.text, "memory");

    EXPECT_TRUE(attemory::server::parse_memory_payload(
        R"({"id":null,"text":"memory"})",
        memory,
        error));
    EXPECT_FALSE(memory.has_id);
    EXPECT_EQ(memory.text, "memory");

    EXPECT_FALSE(attemory::server::parse_memory_payload(
        R"({"text":"memory","metadata":{}})",
        memory,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "location"), "add-memory request");
    EXPECT_EQ(detail_value(error, "field"), "metadata");

    EXPECT_FALSE(attemory::server::parse_memory_payload(
        "{\"text\":\"memory\",\"code\":\"print(1)\"}",
        memory,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "code");

    const std::string too_long_id(4097, 'x');
    EXPECT_FALSE(attemory::server::parse_memory_payload(
        std::string(R"({"id":")") + too_long_id + R"(","text":"memory"})",
        memory,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "id");
    EXPECT_EQ(detail_value(error, "max_bytes"), "4096");
}

void test_text_payload_schema_and_size_errors() {
    std::string text;
    ErrorInfo error;

    EXPECT_TRUE(attemory::server::parse_text_payload(
        R"({"text":"system"})",
        "add-system",
        "text",
        text,
        error));
    EXPECT_EQ(text, "system");

    EXPECT_FALSE(attemory::server::parse_text_payload(
        R"({"text":7})",
        "add-system",
        "text",
        text,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "text");
    EXPECT_EQ(detail_value(error, "expected"), "string");

    EXPECT_FALSE(attemory::server::parse_text_payload(
        R"({"text":"ok","extra":"nope"})",
        "add-system",
        "text",
        text,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "location"), "add-system request");
    EXPECT_EQ(detail_value(error, "field"), "extra");

    const std::string too_long(4ull * 1024ull * 1024ull + 1ull, 'x');
    EXPECT_FALSE(attemory::server::parse_text_payload(
        std::string(R"({"text":")") + too_long + R"("})",
        "add-system",
        "text",
        text,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "text");
    EXPECT_EQ(detail_value(error, "max_bytes"), "4194304");
}

void test_search_payload_schema() {
    attemory::server::SearchRequest parsed;
    ErrorInfo error;

    EXPECT_TRUE(attemory::server::parse_search_payload(
        R"({"query_context":"Current date: 2026-06-04","query":"needle","top_k":3})",
        parsed,
        error));
    EXPECT_EQ(parsed.input.query_context, "Current date: 2026-06-04");
    EXPECT_EQ(parsed.input.query, "needle");
    EXPECT_EQ(parsed.search_overrides.top_k, 3);

    EXPECT_TRUE(attemory::server::parse_search_payload(
        R"({"query":"needle"})",
        parsed,
        error));
    EXPECT_EQ(parsed.input.query_context, "");
    EXPECT_EQ(parsed.input.query, "needle");

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"({"query_context":7,"query":"needle"})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "query_context");
    EXPECT_EQ(detail_value(error, "expected"), "string");

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"({"query":"needle","top-k":3})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "top-k");

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"({"query":"needle","top_k":-1})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "top_k");
    EXPECT_EQ(detail_value(error, "expected"), "non-negative integer");

    EXPECT_TRUE(attemory::server::parse_search_payload(
        R"({"query":"needle","top_k":0})",
        parsed,
        error));
    EXPECT_EQ(parsed.search_overrides.top_k, 0);

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"({"query":"needle","top_k":2147483648})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "top_k");

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"(["query"])",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "expected"), "JSON object");
}

void test_oneshot_payload_schema() {
    attemory::server::OneShotSearchRequest parsed;
    ErrorInfo error;

    EXPECT_TRUE(attemory::server::parse_oneshot_search_payload(
        R"({"system":"system","query_context":"time","query":"query","memories":[{"text":"a"},{"id":"b-id","text":"b"}],"top_k":1})",
        parsed,
        error));
    EXPECT_EQ(parsed.system, "system");
    EXPECT_EQ(parsed.input.query_context, "time");
    EXPECT_EQ(parsed.input.query, "query");
    EXPECT_EQ(parsed.memories.size(), static_cast<size_t>(2));
    EXPECT_FALSE(parsed.memories[0].has_id);
    EXPECT_TRUE(parsed.memories[1].has_id);
    EXPECT_EQ(parsed.memories[1].id, "b-id");
    EXPECT_EQ(parsed.search_overrides.top_k, 1);

    EXPECT_FALSE(attemory::server::parse_oneshot_search_payload(
        R"({"system":"system","query":"query","memories":{}})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "memories");
    EXPECT_EQ(detail_value(error, "expected"), "array");

    EXPECT_FALSE(attemory::server::parse_oneshot_search_payload(
        R"({"system":"system","query":"query","memories":[{"text":"a","file":"a.py"}]})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "location"), "oneshot-search memory entry");
    EXPECT_EQ(detail_value(error, "field"), "file");

    EXPECT_FALSE(attemory::server::parse_oneshot_search_payload(
        R"({"system":"system","query":"query","memories":["a"]})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "memories");
    EXPECT_EQ(detail_value(error, "index"), "0");
    EXPECT_EQ(detail_value(error, "expected"), "object");
}

} // namespace

int main() {
    test_memory_payload_schema();
    test_text_payload_schema_and_size_errors();
    test_search_payload_schema();
    test_oneshot_payload_schema();
    return attemory::test::test_main_result("server request parser test");
}
