#pragma once

#include "context/context.h"

#include <string>
#include <vector>

namespace attemory::server {

struct JsonResponse {
    int status = 200;
    std::string body;
};

JsonResponse build_command_response(
    const attemory::context::AttemoryContext & context,
    const std::string & session_id,
    const attemory::context::CommandResult & result);

std::string build_session_list_body(const std::vector<attemory::context::SessionStatus> & sessions);
std::string build_status_body(bool ok, const std::string & error = std::string());
std::string build_error_body(const attemory::context::ErrorInfo & error);
std::string build_error_body(const std::string & error);
int http_status_for_error_code(attemory::context::ErrorCode code);

} // namespace attemory::server
