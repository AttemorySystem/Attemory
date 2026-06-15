#include "server/http_common.h"

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "server/response_builder.h"

#ifdef QUERY
#undef QUERY
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace attemory::server {
namespace {

std::string sanitize_log_value(const std::string & value, size_t max_len = 160) {
    std::string result;
    result.reserve(std::min(value.size(), max_len));
    for (char ch : value) {
        if (result.size() >= max_len) {
            break;
        }
        switch (ch) {
            case '\n':
            case '\r':
            case '\t':
                result.push_back(' ');
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    if (value.size() > max_len) {
        result += "...";
    }
    return result;
}

std::string current_log_time() {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    char seconds[32] = {};
    char zone[8] = {};
    if (std::strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &local_tm) == 0) {
        return "-";
    }
    std::strftime(zone, sizeof(zone), "%z", &local_tm);

    char timestamp[48] = {};
    std::snprintf(timestamp, sizeof(timestamp), "%s.%03lld%s", seconds, static_cast<long long>(ms), zone);
    return timestamp;
}

} // namespace

const char * command_name(AttemoryCommand command) {
    switch (command) {
        case AttemoryCommand::CREATE_SESSION:
            return "create-session";
        case AttemoryCommand::DELETE_SESSION:
            return "delete-session";
        case AttemoryCommand::ADD_SYSTEM:
            return "add-system";
        case AttemoryCommand::ADD_MEMORY:
            return "add-memory";
        case AttemoryCommand::NEXT_SEGMENT:
            return "next-segment";
        case AttemoryCommand::INDEX:
            return "index";
        case AttemoryCommand::SAVE_SESSION:
            return "save-session";
        case AttemoryCommand::RESTORE:
            return "restore";
        case AttemoryCommand::CLEAR:
            return "clear";
        case AttemoryCommand::SEARCH:
            return "search";
        case AttemoryCommand::ONESHOT_SEARCH:
            return "oneshot-search";
    }

    return "unknown";
}

void configure_http_server(httplib::Server & server) {
    server.set_payload_max_length(kMaxRequestBodyBytes);
    server.set_read_timeout(3600, 0);
    server.set_write_timeout(3600, 0);
    server.set_idle_interval(1, 0);
    server.set_keep_alive_max_count(100);
    server.set_keep_alive_timeout(3600);
    server.set_default_headers({
        {"X-Content-Type-Options", "nosniff"},
    });
    server.set_socket_options([](socket_t sock) {
#ifndef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#else
        (void) sock;
#endif
    });
}

void set_json_response(httplib::Response & res, int status, const std::string & body) {
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}

void set_error_response(httplib::Response & res, int status, const std::string & error) {
    set_json_response(res, status, build_error_body(error));
}

void set_error_response(
    httplib::Response & res,
    const attemory::context::ErrorInfo & error) {
    set_json_response(res, http_status_for_error_code(error.code), build_error_body(error));
}

void log_request_metadata(
    const httplib::Request & req,
    std::optional<AttemoryCommand> command,
    const std::string & session_id) {
    const std::string content_type = sanitize_log_value(req.get_header_value("Content-Type", "-"));
    const std::string user_agent = sanitize_log_value(req.get_header_value("User-Agent", "-"));
    const std::string now = current_log_time();
    std::fprintf(
        stderr,
        "time=%s request received method=%s path=%s remote=%s:%d content_type=%s payload_bytes=%zu session=%s command=%s user_agent=\"%s\"\n",
        now.c_str(),
        req.method.c_str(),
        req.path.c_str(),
        req.remote_addr.c_str(),
        req.remote_port,
        content_type.c_str(),
        req.body.size(),
        session_id.empty() ? "-" : session_id.c_str(),
        command.has_value() ? command_name(*command) : "-",
        user_agent.c_str());
    std::fflush(stderr);
}

void log_response_metadata(const httplib::Request & req, const httplib::Response & res) {
    const std::string now = current_log_time();
    std::fprintf(
        stderr,
        "time=%s request finished method=%s path=%s remote=%s:%d status=%d response_bytes=%zu\n",
        now.c_str(),
        req.method.c_str(),
        req.path.c_str(),
        req.remote_addr.c_str(),
        req.remote_port,
        res.status,
        res.body.size());
    std::fflush(stderr);
}

void log_command_failure(
    AttemoryCommand command,
    const std::string & session_id,
    const std::string & error) {
    std::fprintf(
        stderr,
        "time=%s command failed session=%s command=%s error=\"%s\"\n",
        current_log_time().c_str(),
        session_id.empty() ? "-" : session_id.c_str(),
        command_name(command),
        sanitize_log_value(error).c_str());
}

} // namespace attemory::server
