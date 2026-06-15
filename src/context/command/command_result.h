#pragma once

#include "context/context_types.h"
#include "persistent/persistent.h"

#include <cstdint>
#include <vector>
#include <string>

namespace attemory::context {

struct Session;

ErrorInfo make_error_info(
    ErrorCode code,
    const std::string & message,
    std::vector<ErrorDetail> details = {});
ErrorInfo infer_error_info(const std::string & message);
const ErrorInfo & command_error_info(const CommandResult & result);

CommandResult make_error(const std::string & error);
CommandResult make_error(
    ErrorCode code,
    const std::string & error,
    std::vector<ErrorDetail> details = {});
CommandResult make_error(const ErrorInfo & error);
bool result_to_bool(const attemory::persistent::ResultStatus & status, std::string & error);

CommandResult success_result(ResultPayload payload = ResultPayload::None);
CommandResult token_usage_result(
    atmcore::Runtime * core,
    const RuntimeOptions & runtime,
    int32_t segment_count,
    int32_t token_count,
    int32_t segment_id,
    int32_t memory_index = -1);

} // namespace attemory::context
