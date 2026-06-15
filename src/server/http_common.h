#pragma once

#include "context/attemory_types.h"
#include "context/context_types.h"

#include <cstddef>
#include <optional>
#include <string>

namespace httplib {
class Server;
struct Request;
struct Response;
} // namespace httplib

namespace attemory::server {

constexpr size_t kMaxRequestBodyBytes = 100ull * 1024ull * 1024ull;

const char * command_name(AttemoryCommand command);

void configure_http_server(httplib::Server & server);
void set_json_response(httplib::Response & res, int status, const std::string & body);
void set_error_response(httplib::Response & res, int status, const std::string & error);
void set_error_response(
    httplib::Response & res,
    const attemory::context::ErrorInfo & error);

void log_request_metadata(
    const httplib::Request & req,
    std::optional<AttemoryCommand> command = std::nullopt,
    const std::string & session_id = std::string());
void log_response_metadata(const httplib::Request & req, const httplib::Response & res);
void log_command_failure(
    AttemoryCommand command,
    const std::string & session_id,
    const std::string & error);

} // namespace attemory::server
