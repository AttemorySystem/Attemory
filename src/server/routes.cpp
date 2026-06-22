#include "server/routes.h"

#include "context/command/command_result.h"
#include "context/context.h"
#include "server/http_common.h"
#include "server/request_parser.h"
#include "server/response_builder.h"

#include "httplib.h"

#ifdef QUERY
#undef QUERY
#endif

#include <string>

namespace attemory::server {
namespace {

using attemory::context::AttemoryContext;
using attemory::context::CommandResult;
using attemory::context::ErrorCode;
using attemory::context::ErrorInfo;

bool is_add_memory_path(const httplib::Request & req) {
    constexpr const char * prefix = "/v1/sessions/";
    constexpr const char * suffix = "/memories";
    constexpr size_t prefix_len = std::char_traits<char>::length(prefix);
    constexpr size_t suffix_len = std::char_traits<char>::length(suffix);

    const std::string & path = req.path;
    return req.method == "POST" &&
           path.rfind(prefix, 0) == 0 &&
           path.size() > prefix_len + suffix_len &&
           path.compare(path.size() - suffix_len, suffix_len, suffix) == 0;
}

bool should_log_command(const RouteOptions & options, AttemoryCommand command) {
    return options.http_log && command != AttemoryCommand::ADD_MEMORY;
}

bool should_log_response(const RouteOptions & options, const httplib::Request & req) {
    return options.http_log && !is_add_memory_path(req);
}

void maybe_log_request(
    const RouteOptions & options,
    const httplib::Request & req,
    AttemoryCommand command,
    const std::string & session_id = std::string()) {
    if (should_log_command(options, command)) {
        log_request_metadata(req, command, session_id);
    }
}

void maybe_log_request(
    const RouteOptions & options,
    const httplib::Request & req) {
    if (options.http_log) {
        log_request_metadata(req);
    }
}

CommandResult dispatch_text_command(
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const std::string & payload,
    const attemory::context::SearchRequestOverrides & request_overrides) {
    switch (command) {
        case AttemoryCommand::CREATE_SESSION:
            return context.create_session(session_id);
        case AttemoryCommand::DELETE_SESSION:
            return context.delete_session(session_id);
        case AttemoryCommand::ADD_SYSTEM:
            return context.add_system(session_id, payload);
        case AttemoryCommand::ADD_MEMORY:
            break;
        case AttemoryCommand::NEXT_SEGMENT:
            return context.next_segment(session_id);
        case AttemoryCommand::INDEX:
            return context.index_session(session_id);
        case AttemoryCommand::SAVE_SESSION:
            return context.save_session(session_id);
        case AttemoryCommand::RESTORE:
            return context.restore_session(session_id);
        case AttemoryCommand::CLEAR:
            return context.clear_session(session_id);
        case AttemoryCommand::SEARCH:
            return context.search(session_id, payload, request_overrides);
        case AttemoryCommand::ONESHOT_SEARCH:
            break;
    }

    return attemory::context::make_error(
        ErrorCode::InternalError,
        "unsupported command");
}

void write_command_response(
    const RouteOptions & options,
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const CommandResult & result,
    httplib::Response & res) {
    if (!result.ok && should_log_command(options, command)) {
        log_command_failure(command, session_id, result.error);
    }

    const JsonResponse response = build_command_response(context, session_id, result);
    set_json_response(res, response.status, response.body);
}

void handle_text_command(
    const RouteOptions & options,
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const std::string & payload,
    httplib::Response & res,
    const attemory::context::SearchRequestOverrides & request_overrides = {}) {
    const CommandResult result =
        dispatch_text_command(context, command, session_id, payload, request_overrides);
    write_command_response(options, context, command, session_id, result, res);
}

void handle_empty_command(
    const RouteOptions & options,
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const httplib::Request & req,
    httplib::Response & res) {
    ErrorInfo error;
    if (!validate_empty_body(req, command_name(command), error)) {
        set_error_response(res, error);
        return;
    }
    handle_text_command(options, context, command, session_id, std::string(), res);
}

void handle_create_session(
    const RouteOptions & options,
    AttemoryContext & context,
    const std::string & session_id,
    const httplib::Request & req,
    httplib::Response & res) {
    attemory::context::CreateSessionOptions create_options;
    if (!req.body.empty()) {
        ErrorInfo error;
        if (!validate_json_content_type(req, error)) {
            set_error_response(res, error);
            return;
        }
        if (!parse_create_session_payload(req.body, create_options, error)) {
            set_error_response(res, error);
            return;
        }
    }

    const CommandResult result = context.create_session(session_id, create_options);
    write_command_response(options, context, AttemoryCommand::CREATE_SESSION, session_id, result, res);
}

bool require_json_content_type(
    const httplib::Request & req,
    httplib::Response & res) {
    ErrorInfo error;
    if (validate_json_content_type(req, error)) {
        return true;
    }
    set_error_response(res, error);
    return false;
}

void handle_json_text_command(
    const RouteOptions & options,
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const httplib::Request & req,
    httplib::Response & res,
    const std::string & command_name,
    const std::string & field_name) {
    if (!require_json_content_type(req, res)) {
        return;
    }

    std::string text;
    ErrorInfo error;
    if (!parse_text_payload(req.body, command_name, field_name, text, error)) {
        set_error_response(res, error);
        return;
    }
    handle_text_command(options, context, command, session_id, text, res);
}

void handle_memory_command(
    const RouteOptions & options,
    AttemoryContext & context,
    const std::string & session_id,
    const httplib::Request & req,
    httplib::Response & res,
    AttemoryCommand command) {
    if (!require_json_content_type(req, res)) {
        return;
    }

    attemory::context::MemoryInput memory;
    ErrorInfo error;
    if (!parse_memory_payload(req.body, memory, error)) {
        set_error_response(res, error);
        return;
    }

    const CommandResult result = context.add_memory(session_id, memory);
    write_command_response(options, context, command, session_id, result, res);
}

void handle_search_command(
    const RouteOptions & options,
    AttemoryContext & context,
    AttemoryCommand command,
    const std::string & session_id,
    const httplib::Request & req,
    httplib::Response & res) {
    if (!require_json_content_type(req, res)) {
        return;
    }

    SearchRequest parsed;
    ErrorInfo error;
    if (!parse_search_payload(req.body, parsed, error)) {
        set_error_response(res, error);
        return;
    }
    const CommandResult result = context.search(session_id, parsed.input, parsed.search_overrides);
    write_command_response(options, context, command, session_id, result, res);
}

void handle_oneshot_search(
    const RouteOptions & options,
    AttemoryContext & context,
    const httplib::Request & req,
    httplib::Response & res) {
    ErrorInfo error;
    if (!validate_json_content_type(req, error)) {
        set_error_response(res, error);
        return;
    }

    OneShotSearchRequest parsed;
    if (!parse_oneshot_search_payload(req.body, parsed, error)) {
        set_error_response(res, error);
        return;
    }

    const CommandResult result =
        context.oneshot_search(parsed.system, parsed.memories, parsed.input, parsed.search_overrides);
    write_command_response(options, context, AttemoryCommand::ONESHOT_SEARCH, std::string(), result, res);
}

void install_error_handler(httplib::Server & server) {
    server.set_error_handler([](const httplib::Request &, httplib::Response & res) {
        if (!res.body.empty()) {
            return;
        }
        if (res.status == httplib::StatusCode::NotFound_404) {
            set_error_response(
                res,
                attemory::context::make_error_info(ErrorCode::NotFound, "not found"));
            return;
        }
        if (res.status == httplib::StatusCode::PayloadTooLarge_413) {
            set_error_response(
                res,
                attemory::context::make_error_info(ErrorCode::PayloadTooLarge, "payload too large"));
            return;
        }
        set_error_response(
            res,
            attemory::context::make_error_info(ErrorCode::InternalError, "http error"));
    });
}

} // namespace

void install_routes(
    httplib::Server & server,
    AttemoryContext & context,
    RouteOptions options) {
    server.Get("/health", [&, options](const httplib::Request & req, httplib::Response & res) {
        maybe_log_request(options, req);
        set_json_response(res, httplib::StatusCode::OK_200, build_status_body(true));
    });

    server.Get("/v1/sessions", [&, options](const httplib::Request & req, httplib::Response & res) {
        maybe_log_request(options, req);
        set_json_response(
            res,
            httplib::StatusCode::OK_200,
            build_session_list_body(context.list_sessions()));
    });

    server.Post(R"(/v1/sessions/([^/]+))", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::CREATE_SESSION, session_id);
        handle_create_session(options, context, session_id, req, res);
    });
    server.Delete(R"(/v1/sessions/([^/]+))", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::DELETE_SESSION, session_id);
        handle_empty_command(options, context, AttemoryCommand::DELETE_SESSION, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/system)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::ADD_SYSTEM, session_id);
        handle_json_text_command(
            options,
            context,
            AttemoryCommand::ADD_SYSTEM,
            session_id,
            req,
            res,
            "add-system",
            "text");
    });
    server.Post(R"(/v1/sessions/([^/]+)/memories)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::ADD_MEMORY, session_id);
        handle_memory_command(options, context, session_id, req, res, AttemoryCommand::ADD_MEMORY);
    });
    server.Post(R"(/v1/sessions/([^/]+)/segments/next)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::NEXT_SEGMENT, session_id);
        handle_empty_command(options, context, AttemoryCommand::NEXT_SEGMENT, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/index)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::INDEX, session_id);
        handle_empty_command(options, context, AttemoryCommand::INDEX, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/save)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::SAVE_SESSION, session_id);
        handle_empty_command(options, context, AttemoryCommand::SAVE_SESSION, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/restore)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::RESTORE, session_id);
        handle_empty_command(options, context, AttemoryCommand::RESTORE, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/clear-cache)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::CLEAR, session_id);
        handle_empty_command(options, context, AttemoryCommand::CLEAR, session_id, req, res);
    });
    server.Post(R"(/v1/sessions/([^/]+)/search)", [&, options](const httplib::Request & req, httplib::Response & res) {
        const std::string session_id = req.matches[1].str();
        maybe_log_request(options, req, AttemoryCommand::SEARCH, session_id);
        handle_search_command(options, context, AttemoryCommand::SEARCH, session_id, req, res);
    });
    server.Post("/v1/oneshot/search", [&, options](const httplib::Request & req, httplib::Response & res) {
        maybe_log_request(options, req, AttemoryCommand::ONESHOT_SEARCH);
        handle_oneshot_search(options, context, req, res);
    });

    install_error_handler(server);
    if (options.http_log) {
        server.set_logger([options](const httplib::Request & req, const httplib::Response & res) {
            if (should_log_response(options, req)) {
                log_response_metadata(req, res);
            }
        });
    }
}

} // namespace attemory::server
