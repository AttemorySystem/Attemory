#include "httplib.h"
#include "nlohmann/json.hpp"
#include "server/http_common.h"
#include "server/request_parser.h"
#include "tests/test_support.h"

#include <string>

namespace {

using json = nlohmann::ordered_json;
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

json parse_json(const std::string & body) {
    json parsed = json::parse(body, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded());
    return parsed;
}

void test_json_content_type_validation() {
    httplib::Request req;
    ErrorInfo error;
    EXPECT_FALSE(attemory::server::validate_json_content_type(req, error));
    EXPECT_EQ(error.code, ErrorCode::UnsupportedMediaType);
    EXPECT_EQ(detail_value(error, "header"), "Content-Type");

    req.set_header("Content-Type", "Application/JSON; charset=utf-8");
    EXPECT_TRUE(attemory::server::validate_json_content_type(req, error));

    httplib::Request plain_req;
    plain_req.set_header("Content-Type", " text/plain ");
    EXPECT_FALSE(attemory::server::validate_json_content_type(plain_req, error));
    EXPECT_EQ(error.code, ErrorCode::UnsupportedMediaType);
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
}

void test_response_helpers_set_json_content_type() {
    httplib::Response response;
    attemory::server::set_json_response(response, 202, R"({"data":{"ok":true}})");

    EXPECT_EQ(response.status, 202);
    EXPECT_EQ(response.get_header_value("Content-Type"), "application/json; charset=utf-8");
    EXPECT_EQ(parse_json(response.body)["data"]["ok"].get<bool>(), true);

    attemory::server::set_error_response(response, 400, "invalid request");
    EXPECT_EQ(response.status, 400);
    json body = parse_json(response.body);
    EXPECT_TRUE(body.find("error") != body.end());
    EXPECT_EQ(body["error"]["code"].get<std::string>(), "INVALID_REQUEST");
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
    test_json_content_type_validation();
    test_empty_body_validation();
    test_response_helpers_set_json_content_type();
    test_configured_server_rejects_duplicate_bind();
    return attemory::test::test_main_result("HTTP contract test");
}
