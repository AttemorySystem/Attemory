#include "context/command/command_result.h"

#include <cstring>
#include <utility>

namespace attemory::context {

namespace {

bool starts_with(const std::string & text, const char * prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string after_prefix(const std::string & text, const char * prefix) {
    return text.substr(std::char_traits<char>::length(prefix));
}

} // namespace

ErrorInfo make_error_info(
    ErrorCode code,
    const std::string & message,
    std::vector<ErrorDetail> details) {
    ErrorInfo error;
    error.code = code;
    error.message = message;
    error.details = std::move(details);
    return error;
}

ErrorInfo infer_error_info(const std::string & message) {
    constexpr const char * session_not_found = "session not found: ";
    constexpr const char * session_already_exists = "session already exists: ";
    constexpr const char * unknown_field = " contains unknown field: ";

    if (starts_with(message, session_not_found)) {
        return make_error_info(
            ErrorCode::SessionNotFound,
            message,
            {{"session_id", after_prefix(message, session_not_found)}});
    }
    if (starts_with(message, session_already_exists)) {
        return make_error_info(
            ErrorCode::SessionAlreadyExists,
            message,
            {{"session_id", after_prefix(message, session_already_exists)}});
    }
    if (message == "invalid session id") {
        return make_error_info(ErrorCode::InvalidSessionId, message);
    }
    if (message == "not found") {
        return make_error_info(ErrorCode::NotFound, message);
    }
    if (message == "payload too large") {
        return make_error_info(ErrorCode::PayloadTooLarge, message);
    }
    if (message == "Content-Type must be application/json") {
        return make_error_info(ErrorCode::UnsupportedMediaType, message);
    }

    const size_t pos = message.find(unknown_field);
    if (pos != std::string::npos) {
        return make_error_info(
            ErrorCode::InvalidRequest,
            message,
            {
                {"location", message.substr(0, pos)},
                {"field", message.substr(pos + std::char_traits<char>::length(unknown_field))},
            });
    }

    if (message.find("must be") != std::string::npos ||
        message.find("requires") != std::string::npos ||
        message.find("exceeds maximum length") != std::string::npos ||
        message.find("invalid") != std::string::npos) {
        return make_error_info(ErrorCode::InvalidRequest, message);
    }

    return make_error_info(ErrorCode::BadRequest, message);
}

const ErrorInfo & command_error_info(const CommandResult & result) {
    if (!result.error_info.message.empty()) {
        return result.error_info;
    }

    static thread_local ErrorInfo inferred;
    inferred = infer_error_info(result.error);
    return inferred;
}

CommandResult make_error(const std::string & error) {
    return make_error(infer_error_info(error));
}

CommandResult make_error(
    ErrorCode code,
    const std::string & error,
    std::vector<ErrorDetail> details) {
    return make_error(make_error_info(code, error, std::move(details)));
}

CommandResult make_error(const ErrorInfo & error) {
    CommandResult result;
    result.ok = false;
    result.error = error.message;
    result.error_info = error;
    return result;
}

bool result_to_bool(const attemory::persistent::ResultStatus & status, std::string & error) {
    if (status.ok) {
        error.clear();
        return true;
    }
    error = status.error;
    return false;
}

CommandResult success_result(ResultPayload payload) {
    CommandResult result;
    result.ok = true;
    result.payload = payload;
    return result;
}

CommandResult token_usage_result(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    int32_t segment_count,
    int32_t token_count,
    int32_t segment_id,
    int32_t memory_index) {
    CommandResult result = success_result(ResultPayload::TokenUsage);
    result.token_usage.token_count = token_count;
    result.token_usage.ctx_length = atmcore::effective_context_length(core, runtime);
    result.token_usage.segment_id = segment_id;
    result.token_usage.segment_count = segment_count;
    result.token_usage.memory_index = memory_index;
    return result;
}

} // namespace attemory::context
