#include "server/response_builder.h"

#include "context/command/command_result.h"
#include "context/context.h"
#include "nlohmann/json.hpp"
#include "tests/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using json = nlohmann::ordered_json;
using attemory::context::CommandResult;
using attemory::context::ErrorCode;
using attemory::context::ErrorInfo;
using attemory::context::ResultPayload;

json parse_json(const std::string & body) {
    json parsed = json::parse(body, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    return parsed;
}

std::string detail_value(const ErrorInfo & error, const std::string & key) {
    for (const auto & detail : error.details) {
        if (detail.key == key) {
            return detail.value;
        }
    }
    return std::string();
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

void test_status_and_error_envelopes() {
    json body = parse_json(attemory::server::build_status_body(true));
    EXPECT_TRUE(body.find("ok") == body.end());
    EXPECT_TRUE(body.find("error") == body.end());
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "ok");

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
    expect_error_envelope(
        attemory::context::make_error_info(ErrorCode::InternalError, "internal"),
        500,
        "INTERNAL_ERROR");
}

void test_legacy_command_errors_are_normalized() {
    const CommandResult result = attemory::context::make_error("session not found: legacy");
    const ErrorInfo & error = attemory::context::command_error_info(result);
    EXPECT_EQ(error.code, ErrorCode::SessionNotFound);
    EXPECT_EQ(detail_value(error, "session_id"), "legacy");
}

void test_session_list_body() {
    attemory::context::SessionStatus first;
    first.session_id = "a";
    first.memory_count = 3;
    first.segment_count = 2;
    first.total_tokens = 42;
    first.indexed = true;
    first.plan_ready = true;
    first.kv_persist = true;

    attemory::context::SessionStatus second;
    second.session_id = "b";
    second.facts_dirty = true;

    json body = parse_json(attemory::server::build_session_list_body({first, second}));
    EXPECT_EQ(body["data"]["sessions"].size(), static_cast<size_t>(2));
    EXPECT_EQ(body["data"]["sessions"][0]["session_id"].get<std::string>(), "a");
    EXPECT_EQ(body["data"]["sessions"][0]["memory_count"].get<int>(), 3);
    EXPECT_TRUE(body["data"]["sessions"][0]["indexed"].get<bool>());
    EXPECT_TRUE(body["data"]["sessions"][0]["kv_persist"].get<bool>());
    EXPECT_EQ(body["data"]["sessions"][1]["session_id"].get<std::string>(), "b");
    EXPECT_TRUE(body["data"]["sessions"][1]["facts_dirty"].get<bool>());
    EXPECT_FALSE(body["data"]["sessions"][1]["kv_persist"].get<bool>());
}

void test_token_usage_response() {
    attemory::context::AttemoryContext context;
    CommandResult result = attemory::context::success_result(ResultPayload::TokenUsage);
    result.token_usage.token_count = 12;
    result.token_usage.ctx_length = 20;
    result.token_usage.segment_id = 2;
    result.token_usage.segment_count = 4;
    result.token_usage.memory_index = 7;
    result.token_usage.has_memory_id = true;
    result.token_usage.memory_id = "external-id";

    const attemory::server::JsonResponse response =
        attemory::server::build_command_response(context, "unused", result);
    EXPECT_EQ(response.status, 200);

    json body = parse_json(response.body);
    const json & data = body["data"];
    EXPECT_EQ(data["prefill_tokens"].get<int>(), 12);
    EXPECT_EQ(data["ctx_length"].get<int>(), 20);
    EXPECT_EQ(data["remaining_tokens"].get<int>(), 8);
    EXPECT_EQ(data["segment_id"].get<int>(), 2);
    EXPECT_EQ(data["segment_count"].get<int>(), 4);
    EXPECT_EQ(data["memory_idx"].get<int>(), 7);
    EXPECT_EQ(data["id"].get<std::string>(), "external-id");
}

void test_oneshot_response_omits_idx_and_preserves_optional_id() {
    attemory::context::AttemoryContext context;
    CommandResult result = attemory::context::success_result(ResultPayload::OneShotSearchOrdered);

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
    EXPECT_TRUE(first.find("memory_idx") == first.end());
    EXPECT_TRUE(first.find("id") == first.end());
    EXPECT_EQ(first["rank"].get<int>(), 1);
    EXPECT_EQ(first["text"].get<std::string>(), "first");
    EXPECT_EQ(second["rank"].get<int>(), 2);
    EXPECT_EQ(second["id"].get<std::string>(), "external-id");
    EXPECT_TRUE(second.find("idx") == second.end());
    EXPECT_TRUE(second.find("memory_idx") == second.end());
}

} // namespace

int main() {
    test_status_and_error_envelopes();
    test_legacy_command_errors_are_normalized();
    test_session_list_body();
    test_token_usage_response();
    test_oneshot_response_omits_idx_and_preserves_optional_id();
    return attemory::test::test_main_result("server response builder test");
}
