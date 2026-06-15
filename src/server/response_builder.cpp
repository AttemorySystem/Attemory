#include "server/response_builder.h"

#include "context/command/command_result.h"
#include "nlohmann/json.hpp"
#include "persistent/persistent.h"

#include <algorithm>
#include <cstdint>

namespace attemory::server {

int http_status_for_error_code(attemory::context::ErrorCode code) {
    switch (code) {
        case attemory::context::ErrorCode::SessionNotFound:
        case attemory::context::ErrorCode::NotFound:
            return 404;
        case attemory::context::ErrorCode::SessionAlreadyExists:
            return 409;
        case attemory::context::ErrorCode::UnsupportedMediaType:
            return 415;
        case attemory::context::ErrorCode::PayloadTooLarge:
            return 413;
        case attemory::context::ErrorCode::InternalError:
            return 500;
        case attemory::context::ErrorCode::InvalidRequest:
        case attemory::context::ErrorCode::InvalidSessionId:
        case attemory::context::ErrorCode::BadRequest:
            return 400;
    }
    return 400;
}

namespace {

using json = nlohmann::ordered_json;
using attemory::context::CommandResult;
using attemory::context::ErrorCode;
using attemory::context::ErrorInfo;
using attemory::context::ResultPayload;
using attemory::context::SessionSummary;
using attemory::context::SessionStatus;

int status_for_command_result(const CommandResult & result) {
    if (result.ok) {
        return 200;
    }
    return http_status_for_error_code(attemory::context::command_error_info(result).code);
}

const char * error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest:
            return "BAD_REQUEST";
        case ErrorCode::InvalidRequest:
            return "INVALID_REQUEST";
        case ErrorCode::InvalidSessionId:
            return "INVALID_SESSION_ID";
        case ErrorCode::SessionNotFound:
            return "SESSION_NOT_FOUND";
        case ErrorCode::SessionAlreadyExists:
            return "SESSION_ALREADY_EXISTS";
        case ErrorCode::NotFound:
            return "NOT_FOUND";
        case ErrorCode::UnsupportedMediaType:
            return "UNSUPPORTED_MEDIA_TYPE";
        case ErrorCode::PayloadTooLarge:
            return "PAYLOAD_TOO_LARGE";
        case ErrorCode::InternalError:
            return "INTERNAL_ERROR";
    }
    return "BAD_REQUEST";
}

json data_envelope(json data) {
    json body;
    body["data"] = std::move(data);
    return body;
}

json error_envelope(const ErrorInfo & error) {
    json body;
    body["error"] = {
        {"code", error_code_name(error.code)},
        {"message", error.message},
    };
    json details = json::object();
    for (const auto & detail : error.details) {
        details[detail.key] = detail.value;
    }
    if (!details.empty()) {
        body["error"]["details"] = std::move(details);
    }
    return body;
}

json session_status_json(const SessionStatus & status) {
    json body;
    body["session_id"] = status.session_id;
    body["memory_count"] = status.memory_count;
    body["segment_count"] = status.segment_count;
    body["total_tokens"] = status.total_tokens;
    body["resident_segments"] = status.resident_segments;
    body["indexed_segments"] = status.indexed_segments;
    body["saved_segments"] = status.saved_segments;
    body["indexed"] = status.indexed;
    body["disk_cached"] = status.disk_cached;
    body["plan_ready"] = status.plan_ready;
    body["facts_dirty"] = status.facts_dirty;
    return body;
}

void append_summary_fields(json & body, const SessionSummary & summary) {
    body["memory_count"] = summary.memory_count;
    body["segment_count"] = summary.segment_count;
    body["resident_segments"] = summary.resident_segments;
    body["saved_segments"] = summary.saved_segments;
    body["indexed_segments"] = summary.indexed_segments;
}

json token_usage_json(const CommandResult & result) {
    const auto & usage = result.token_usage;
    json body;
    body["prefill_tokens"] = usage.token_count;
    body["ctx_length"] = usage.ctx_length;
    body["remaining_tokens"] =
        usage.token_count < 0 ? -1 : std::max<int32_t>(usage.ctx_length - usage.token_count, 0);
    body["segment_id"] = usage.segment_id;
    body["segment_count"] = usage.segment_count;
    if (usage.memory_index >= 0) {
        body["memory_idx"] = usage.memory_index;
    }
    if (usage.has_memory_id) {
        body["id"] = usage.memory_id;
    }
    return body;
}

json restore_json(
    const std::string & session_id,
    const CommandResult & result) {
    json body;
    body["session_id"] = session_id;
    body["restored"] = result.restored;
    append_summary_fields(body, result.summary);
    return body;
}

json save_session_json(
    const std::string & session_id,
    const CommandResult & result) {
    json body;
    body["session_id"] = session_id;
    body["saved"] = true;
    append_summary_fields(body, result.summary);
    return body;
}

json ordered_search_json(
    const attemory::persistent::SessionStore & store,
    const std::vector<AttentionMemoryRef> & ordered_memories) {
    json body = json::array();
    for (size_t i = 0; i < ordered_memories.size(); ++i) {
        const AttentionMemoryRef & memory = ordered_memories[i];
        const attemory::persistent::MemoryRecord * record =
            attemory::persistent::find_memory(store, memory.memory_index);

        json item;
        item["memory_idx"] = memory.memory_index;
        item["segment_id"] = memory.segment_id;
        item["text"] = record != nullptr ? record->text : std::string();
        if (record != nullptr && record->has_id) {
            item["id"] = record->id;
        }
        item["rank"] = (int64_t) i + 1;
        body.push_back(std::move(item));
    }
    return body;
}

json oneshot_search_json(const CommandResult & result) {
    json body = json::array();
    for (size_t i = 0; i < result.oneshot_search_memories.size(); ++i) {
        const auto & memory = result.oneshot_search_memories[i];
        json item;
        if (memory.has_id) {
            item["id"] = memory.id;
        }
        item["segment_id"] = memory.segment_id;
        item["text"] = memory.text;
        item["rank"] = (int64_t) i + 1;
        body.push_back(std::move(item));
    }
    return body;
}

} // namespace

std::string build_status_body(bool ok, const std::string & error) {
    if (!ok) {
        return error_envelope(attemory::context::infer_error_info(error)).dump();
    }
    json data;
    data["status"] = "ok";
    return data_envelope(std::move(data)).dump();
}

std::string build_error_body(const ErrorInfo & error) {
    return error_envelope(error).dump();
}

std::string build_error_body(const std::string & error) {
    return error_envelope(attemory::context::infer_error_info(error)).dump();
}

std::string build_session_list_body(const std::vector<SessionStatus> & sessions) {
    json data;
    data["sessions"] = json::array();
    for (const SessionStatus & status : sessions) {
        data["sessions"].push_back(session_status_json(status));
    }
    return data_envelope(std::move(data)).dump();
}

JsonResponse build_command_response(
    const attemory::context::AttemoryContext & context,
    const std::string & session_id,
    const CommandResult & result) {
    JsonResponse response;
    response.status = status_for_command_result(result);

    switch (result.payload) {
        case ResultPayload::TokenUsage:
            response.body = result.ok ?
                data_envelope(token_usage_json(result)).dump() :
                error_envelope(attemory::context::command_error_info(result)).dump();
            return response;
        case ResultPayload::RestoreSummary:
            response.body = result.ok ?
                data_envelope(restore_json(session_id, result)).dump() :
                error_envelope(attemory::context::command_error_info(result)).dump();
            return response;
        case ResultPayload::SaveSummary:
            response.body = result.ok ?
                data_envelope(save_session_json(session_id, result)).dump() :
                error_envelope(attemory::context::command_error_info(result)).dump();
            return response;
        case ResultPayload::Search:
        case ResultPayload::SearchOrdered:
            if (result.ok) {
                const attemory::persistent::SessionStore * store = context.session_store(session_id);
                if (store == nullptr) {
                    ErrorInfo error = attemory::context::make_error_info(
                        ErrorCode::SessionNotFound,
                        "session not found: " + session_id,
                        {{"session_id", session_id}});
                    response.status = http_status_for_error_code(error.code);
                    response.body = error_envelope(error).dump();
                    return response;
                }
                json data;
                data["results"] = ordered_search_json(*store, result.ordered_search_memories);
                response.body = data_envelope(std::move(data)).dump();
            } else {
                response.body = error_envelope(attemory::context::command_error_info(result)).dump();
            }
            return response;
        case ResultPayload::OneShotSearchOrdered:
            if (result.ok) {
                json data;
                data["results"] = oneshot_search_json(result);
                response.body = data_envelope(std::move(data)).dump();
            } else {
                response.body = error_envelope(attemory::context::command_error_info(result)).dump();
            }
            return response;
        case ResultPayload::None:
            response.body = result.ok ?
                data_envelope(json::object()).dump() :
                error_envelope(attemory::context::command_error_info(result)).dump();
            return response;
    }

    ErrorInfo error = attemory::context::make_error_info(
        ErrorCode::InternalError,
        "unsupported response payload");
    response.body = error_envelope(error).dump();
    response.status = http_status_for_error_code(error.code);
    return response;
}

} // namespace attemory::server
