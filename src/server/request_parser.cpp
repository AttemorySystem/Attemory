#include "server/request_parser.h"

#include "context/command/command_result.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace attemory::server {
namespace {

using json = nlohmann::ordered_json;
using attemory::context::ErrorCode;
using attemory::context::ErrorDetail;
using attemory::context::ErrorInfo;

constexpr size_t kMaxOneshotMemoryCount = 100000;
constexpr size_t kMaxMemoryIdBytes = 4096;
constexpr size_t kMaxTextBytes = 4ull * 1024ull * 1024ull;

void set_error(
    ErrorInfo & error,
    ErrorCode code,
    const std::string & message,
    std::vector<ErrorDetail> details = {}) {
    error = attemory::context::make_error_info(code, message, std::move(details));
}

std::string trim_http_ows(const std::string & value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string ascii_lower(std::string value) {
    for (char & ch : value) {
        ch = (char) std::tolower((unsigned char) ch);
    }
    return value;
}

bool is_application_json_media_type(const std::string & content_type) {
    if (content_type.empty()) {
        return false;
    }

    const size_t parameter_begin = content_type.find(';');
    const std::string media_type =
        ascii_lower(trim_http_ows(content_type.substr(0, parameter_begin)));
    return media_type == "application/json";
}

bool read_non_negative_i32_json(
    const json & body,
    const char * field,
    int32_t & value,
    const std::string & command_name,
    ErrorInfo & error) {
    const auto it = body.find(field);
    if (it == body.end()) {
        return true;
    }
    if (it->is_null()) {
        return true;
    }
    if (!it->is_number_integer()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field " + field + " must be a non-negative integer",
            {{"field", field}, {"expected", "non-negative integer"}});
        return false;
    }
    const int64_t parsed = it->get<int64_t>();
    if (parsed < 0 || parsed > std::numeric_limits<int32_t>::max()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field " + field + " must be a non-negative integer",
            {{"field", field}, {"expected", "non-negative integer"}});
        return false;
    }
    value = (int32_t) parsed;
    return true;
}

bool parse_json_object_payload(
    const std::string & payload,
    const std::string & command_name,
    json & body,
    ErrorInfo & error) {
    error = {};
    body = json::parse(payload, nullptr, false);
    if (body.is_discarded()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            "invalid " + command_name + " JSON",
            {{"expected", "JSON object"}});
        return false;
    }
    if (!body.is_object()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " request body must be a JSON object",
            {{"expected", "JSON object"}});
        return false;
    }
    return true;
}

bool reject_unknown_fields(
    const json & body,
    const std::string & location,
    std::initializer_list<std::string> allowed_fields,
    ErrorInfo & error) {
    for (auto it = body.begin(); it != body.end(); ++it) {
        const std::string key = it.key();
        const bool allowed =
            std::find(allowed_fields.begin(), allowed_fields.end(), key) != allowed_fields.end();
        if (!allowed) {
            set_error(
                error,
                ErrorCode::InvalidRequest,
                location + " contains unknown field: " + key,
                {{"location", location}, {"field", key}});
            return false;
        }
    }
    return true;
}

bool read_required_string_field(
    const json & body,
    const std::string & command_name,
    const char * field,
    size_t max_bytes,
    std::string & value,
    ErrorInfo & error) {
    const auto it = body.find(field);
    if (it == body.end() || !it->is_string()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " request requires string field: " + field,
            {{"field", field}, {"expected", "string"}});
        return false;
    }
    value = it->get<std::string>();
    if (value.size() > max_bytes) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field " + field + " exceeds maximum length",
            {{"field", field}, {"max_bytes", std::to_string(max_bytes)}});
        return false;
    }
    return true;
}

bool read_optional_string_field(
    const json & body,
    const std::string & command_name,
    const char * field,
    size_t max_bytes,
    std::string & value,
    ErrorInfo & error) {
    value.clear();
    const auto it = body.find(field);
    if (it == body.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field " + field + " must be a string",
            {{"field", field}, {"expected", "string"}});
        return false;
    }
    value = it->get<std::string>();
    if (value.size() > max_bytes) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field " + field + " exceeds maximum length",
            {{"field", field}, {"max_bytes", std::to_string(max_bytes)}});
        return false;
    }
    return true;
}

bool read_optional_id_field(
    const json & body,
    const std::string & command_name,
    attemory::context::MemoryInput & memory,
    ErrorInfo & error) {
    const auto id_it = body.find("id");
    if (id_it == body.end() || id_it->is_null()) {
        memory.has_id = false;
        memory.id.clear();
        return true;
    }
    if (!id_it->is_string()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field id must be a string",
            {{"field", "id"}, {"expected", "string"}});
        return false;
    }
    const std::string id = id_it->get<std::string>();
    if (id.size() > kMaxMemoryIdBytes) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            command_name + " field id exceeds maximum length",
            {{"field", "id"}, {"max_bytes", std::to_string(kMaxMemoryIdBytes)}});
        return false;
    }
    memory.has_id = true;
    memory.id = id;
    return true;
}

bool read_search_options(
    const json & body,
    const std::string & command_name,
    attemory::context::SearchRequestOverrides & overrides,
    ErrorInfo & error) {
    if (!read_non_negative_i32_json(
            body,
            "top_k",
            overrides.top_k,
            command_name,
            error)) {
        return false;
    }
    return true;
}

} // namespace

bool validate_json_content_type(
    const httplib::Request & req,
    ErrorInfo & error) {
    error = {};

    const std::string content_type = req.get_header_value("Content-Type");
    if (is_application_json_media_type(content_type)) {
        return true;
    }

    set_error(
        error,
        ErrorCode::UnsupportedMediaType,
        "Content-Type must be application/json",
        {{"header", "Content-Type"}, {"expected", "application/json"}});
    return false;
}

bool validate_empty_body(
    const httplib::Request & req,
    const std::string & command_name,
    ErrorInfo & error) {
    error = {};
    if (req.body.empty()) {
        return true;
    }

    set_error(
        error,
        ErrorCode::InvalidRequest,
        command_name + " request body must be empty",
        {
            {"expected", "empty body"},
            {"body_bytes", std::to_string(req.body.size())},
        });
    return false;
}

bool parse_text_payload(
    const std::string & payload,
    const std::string & command_name,
    const std::string & field_name,
    std::string & text,
    ErrorInfo & error) {
    text.clear();
    json body;
    if (!parse_json_object_payload(payload, command_name, body, error)) {
        return false;
    }
    if (!reject_unknown_fields(body, command_name + " request", {field_name}, error)) {
        return false;
    }
    return read_required_string_field(
        body,
        command_name,
        field_name.c_str(),
        kMaxTextBytes,
        text,
        error);
}

bool parse_memory_payload(
    const std::string & payload,
    attemory::context::MemoryInput & memory,
    ErrorInfo & error) {
    memory = {};
    json body;
    if (!parse_json_object_payload(payload, "add-memory", body, error)) {
        return false;
    }
    if (!reject_unknown_fields(body, "add-memory request", {"id", "text"}, error)) {
        return false;
    }
    if (!read_required_string_field(
            body,
            "add-memory",
            "text",
            kMaxTextBytes,
            memory.text,
            error)) {
        return false;
    }
    return read_optional_id_field(body, "add-memory", memory, error);
}

bool parse_search_payload(
    const std::string & payload,
    SearchRequest & parsed,
    ErrorInfo & error) {
    parsed = {};
    json body;
    if (!parse_json_object_payload(payload, "search", body, error)) {
        return false;
    }
    if (!reject_unknown_fields(
            body,
            "search request",
            {"query", "query_context", "top_k"},
            error)) {
        return false;
    }
    if (!read_optional_string_field(
            body,
            "search",
            "query_context",
            kMaxTextBytes,
            parsed.input.query_context,
            error)) {
        return false;
    }
    if (!read_required_string_field(
            body,
            "search",
            "query",
            kMaxTextBytes,
            parsed.input.query,
            error)) {
        return false;
    }
    return read_search_options(body, "search", parsed.search_overrides, error);
}

bool parse_oneshot_search_payload(
    const std::string & payload,
    OneShotSearchRequest & parsed,
    ErrorInfo & error) {
    parsed = {};

    json body;
    if (!parse_json_object_payload(payload, "oneshot-search", body, error)) {
        return false;
    }
    if (!reject_unknown_fields(
            body,
            "oneshot-search request",
            {"system", "query_context", "query", "memories", "top_k"},
            error)) {
        return false;
    }

    if (!read_required_string_field(
            body,
            "oneshot-search",
            "system",
            kMaxTextBytes,
            parsed.system,
            error)) {
        return false;
    }
    if (!read_optional_string_field(
            body,
            "oneshot-search",
            "query_context",
            kMaxTextBytes,
            parsed.input.query_context,
            error)) {
        return false;
    }
    if (!read_required_string_field(
            body,
            "oneshot-search",
            "query",
            kMaxTextBytes,
            parsed.input.query,
            error)) {
        return false;
    }

    const auto memories_it = body.find("memories");
    if (memories_it == body.end() || !memories_it->is_array()) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            "oneshot-search request requires array field: memories",
            {{"field", "memories"}, {"expected", "array"}});
        return false;
    }
    if (memories_it->size() > kMaxOneshotMemoryCount) {
        set_error(
            error,
            ErrorCode::InvalidRequest,
            "oneshot-search memories exceeds maximum count",
            {{"field", "memories"}, {"max_count", std::to_string(kMaxOneshotMemoryCount)}});
        return false;
    }

    parsed.memories.reserve(memories_it->size());
    for (size_t i = 0; i < memories_it->size(); ++i) {
        const json & item = (*memories_it)[i];
        if (!item.is_object()) {
            set_error(
                error,
                ErrorCode::InvalidRequest,
                "oneshot-search memories entries must be JSON objects",
                {
                    {"field", "memories"},
                    {"index", std::to_string(i)},
                    {"expected", "object"},
                });
            return false;
        }
        if (!reject_unknown_fields(
                item,
                "oneshot-search memory entry",
                {"id", "text"},
                error)) {
            return false;
        }

        const auto text_it = item.find("text");
        if (text_it == item.end() || !text_it->is_string()) {
            set_error(
                error,
                ErrorCode::InvalidRequest,
                "oneshot-search memory entry requires string field: text",
                {
                    {"field", "text"},
                    {"index", std::to_string(i)},
                    {"expected", "string"},
                });
            return false;
        }

        attemory::context::OneShotMemoryInput memory;
        if (!read_optional_id_field(item, "oneshot-search memory entry", memory, error)) {
            return false;
        }
        memory.text = text_it->get<std::string>();
        if (memory.text.size() > kMaxTextBytes) {
            set_error(
                error,
                ErrorCode::InvalidRequest,
                "oneshot-search memory entry field text exceeds maximum length",
                {
                    {"field", "text"},
                    {"index", std::to_string(i)},
                    {"max_bytes", std::to_string(kMaxTextBytes)},
                });
            return false;
        }
        parsed.memories.push_back(std::move(memory));
    }

    return read_search_options(body, "oneshot-search", parsed.search_overrides, error);
}

} // namespace attemory::server
