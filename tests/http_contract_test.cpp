#include "context/command/command_result.h"
#include "httplib.h"
#include "server/http_common.h"
#include "nlohmann/json.hpp"
#include "server/request_parser.h"
#include "server/response_builder.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

using json = nlohmann::ordered_json;
using attemory::context::ErrorCode;
using attemory::context::ErrorInfo;

int g_failures = 0;

void fail(const char * expr, const char * file, int line) {
    std::cerr << file << ":" << line << ": expectation failed: " << expr << "\n";
    ++g_failures;
}

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fail(#expr, __FILE__, __LINE__); \
        } \
    } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(actual, expected) \
    do { \
        const auto actual_value = (actual); \
        const auto expected_value = (expected); \
        if (!(actual_value == expected_value)) { \
            std::cerr << __FILE__ << ":" << __LINE__ \
                      << ": expectation failed: " << #actual << " == " << #expected \
                      << "\n"; \
            ++g_failures; \
        } \
    } while (false)

std::string detail_value(const ErrorInfo & error, const std::string & key) {
    for (const auto & detail : error.details) {
        if (detail.key == key) {
            return detail.value;
        }
    }
    return std::string();
}

json parse_json(const std::string & body) {
    json parsed = json::parse(body, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    return parsed;
}

void expect_error_envelope(
    const ErrorInfo & error,
    int32_t expected_status,
    const std::string & expected_code) {
    EXPECT_EQ(attemory::server::http_status_for_error_code(error.code), expected_status);

    json body = parse_json(attemory::server::build_error_body(error));
    EXPECT_TRUE(body.is_object());
    EXPECT_TRUE(body.find("ok") == body.end());
    EXPECT_TRUE(body.find("data") == body.end());
    EXPECT_TRUE(body.find("error") != body.end());
    EXPECT_EQ(body["error"]["code"].get<std::string>(), expected_code);
    EXPECT_EQ(body["error"]["message"].get<std::string>(), error.message);
}

void test_status_envelope() {
    json body = parse_json(attemory::server::build_status_body(true));
    EXPECT_TRUE(body.find("ok") == body.end());
    EXPECT_TRUE(body.find("error") == body.end());
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "ok");
}

void test_error_status_mapping() {
    expect_error_envelope(
        attemory::context::make_error_info(ErrorCode::InvalidRequest, "invalid request"),
        400,
        "INVALID_REQUEST");
    expect_error_envelope(
        attemory::context::make_error_info(
            ErrorCode::SessionNotFound,
            "session not found: demo",
            {{"session_id", "demo"}}),
        404,
        "SESSION_NOT_FOUND");
    expect_error_envelope(
        attemory::context::make_error_info(
            ErrorCode::SessionAlreadyExists,
            "session already exists: demo",
            {{"session_id", "demo"}}),
        409,
        "SESSION_ALREADY_EXISTS");
    expect_error_envelope(
        attemory::context::make_error_info(
            ErrorCode::UnsupportedMediaType,
            "Content-Type must be application/json"),
        415,
        "UNSUPPORTED_MEDIA_TYPE");
    expect_error_envelope(
        attemory::context::make_error_info(ErrorCode::PayloadTooLarge, "payload too large"),
        413,
        "PAYLOAD_TOO_LARGE");
}

void test_legacy_command_errors_are_normalized() {
    const attemory::context::CommandResult result =
        attemory::context::make_error("session not found: legacy");
    const ErrorInfo & error = attemory::context::command_error_info(result);
    EXPECT_EQ(error.code, ErrorCode::SessionNotFound);
    EXPECT_EQ(detail_value(error, "session_id"), "legacy");
}

void test_json_content_type_validation() {
    httplib::Request req;
    ErrorInfo error;
    EXPECT_FALSE(attemory::server::validate_json_content_type(req, error));
    EXPECT_EQ(error.code, ErrorCode::UnsupportedMediaType);
    EXPECT_EQ(detail_value(error, "header"), "Content-Type");
    expect_error_envelope(error, 415, "UNSUPPORTED_MEDIA_TYPE");

    req.set_header("Content-Type", "Application/JSON; charset=utf-8");
    EXPECT_TRUE(attemory::server::validate_json_content_type(req, error));
}

void test_empty_body_validation() {
    httplib::Request req;
    ErrorInfo error;
    EXPECT_TRUE(attemory::server::validate_empty_body(req, "create-session", error));

    req.body = "{}";
    EXPECT_FALSE(attemory::server::validate_empty_body(req, "create-session", error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(error.message, "create-session request body must be empty");
    EXPECT_EQ(detail_value(error, "expected"), "empty body");
    EXPECT_EQ(detail_value(error, "body_bytes"), "2");
    expect_error_envelope(error, 400, "INVALID_REQUEST");
}

void test_memory_payload_schema_errors() {
    attemory::context::MemoryInput memory;
    ErrorInfo error;

    EXPECT_FALSE(attemory::server::parse_memory_payload(
        R"({"text":"memory","metadata":{}})",
        memory,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "location"), "add-memory request");
    EXPECT_EQ(detail_value(error, "field"), "metadata");
    expect_error_envelope(error, 400, "INVALID_REQUEST");

    const std::string too_long_id(4097, 'x');
    EXPECT_FALSE(attemory::server::parse_memory_payload(
        std::string(R"({"id":")") + too_long_id + R"(","text":"memory"})",
        memory,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "id");
    EXPECT_EQ(detail_value(error, "max_bytes"), "4096");

    EXPECT_TRUE(attemory::server::parse_memory_payload(
        R"({"id":"external-id","text":"memory"})",
        memory,
        error));
    EXPECT_TRUE(memory.has_id);
    EXPECT_EQ(memory.id, "external-id");
    EXPECT_EQ(memory.text, "memory");
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

    EXPECT_FALSE(attemory::server::parse_search_payload(
        R"({"query":"needle","top_k_per_segment":2})",
        parsed,
        error));
    EXPECT_EQ(error.code, ErrorCode::InvalidRequest);
    EXPECT_EQ(detail_value(error, "field"), "top_k_per_segment");
}

void test_oneshot_payload_id_contract() {
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
}

void test_oneshot_response_omits_idx_and_preserves_optional_id() {
    attemory::context::AttemoryContext context;
    attemory::context::CommandResult result =
        attemory::context::success_result(attemory::context::ResultPayload::OneShotSearchOrdered);

    attemory::context::OneShotSearchMemory without_id;
    without_id.text = "first";
    without_id.segment_id = 0;
    result.oneshot_search_memories.push_back(without_id);

    attemory::context::OneShotSearchMemory with_id;
    with_id.has_id = true;
    with_id.id = "external-id";
    with_id.text = "second";
    with_id.segment_id = 1;
    result.oneshot_search_memories.push_back(with_id);

    const attemory::server::JsonResponse response =
        attemory::server::build_command_response(context, "unused", result);
    EXPECT_EQ(response.status, 200);

    json body = parse_json(response.body);
    const json & first = body["data"]["results"][0];
    const json & second = body["data"]["results"][1];
    EXPECT_TRUE(first.find("idx") == first.end());
    EXPECT_TRUE(first.find("id") == first.end());
    EXPECT_EQ(first["text"].get<std::string>(), "first");
    EXPECT_EQ(second["id"].get<std::string>(), "external-id");
    EXPECT_TRUE(second.find("idx") == second.end());
}

void test_configured_server_rejects_duplicate_bind() {
    httplib::Server first;
    httplib::Server second;
    attemory::server::configure_http_server(first);
    attemory::server::configure_http_server(second);

    const int port = first.bind_to_any_port("127.0.0.1");
    EXPECT_TRUE(port > 0);
    if (port <= 0) {
        return;
    }

    EXPECT_FALSE(second.bind_to_port("127.0.0.1", port));
}

} // namespace

int main() {
    test_status_envelope();
    test_error_status_mapping();
    test_legacy_command_errors_are_normalized();
    test_json_content_type_validation();
    test_empty_body_validation();
    test_memory_payload_schema_errors();
    test_search_payload_schema();
    test_oneshot_payload_id_contract();
    test_oneshot_response_omits_idx_and_preserves_optional_id();
    test_configured_server_rejects_duplicate_bind();

    if (g_failures != 0) {
        std::cerr << g_failures << " HTTP contract test expectation(s) failed\n";
        return 1;
    }
    return 0;
}
