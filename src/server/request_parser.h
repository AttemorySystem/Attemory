#pragma once

#include "context/context_types.h"

#include <string>
#include <vector>

namespace httplib {
struct Request;
} // namespace httplib

namespace attemory::server {

struct OneShotSearchRequest {
    std::string system;
    attemory::context::SearchInput input;
    std::vector<attemory::context::OneShotMemoryInput> memories;
    attemory::context::SearchRequestOverrides search_overrides;
};

struct SearchRequest {
    attemory::context::SearchInput input;
    attemory::context::SearchRequestOverrides search_overrides;
};

bool validate_json_content_type(
    const httplib::Request & req,
    attemory::context::ErrorInfo & error);

bool validate_empty_body(
    const httplib::Request & req,
    const std::string & command_name,
    attemory::context::ErrorInfo & error);

bool parse_text_payload(
    const std::string & payload,
    const std::string & command_name,
    const std::string & field_name,
    std::string & text,
    attemory::context::ErrorInfo & error);

bool parse_create_session_payload(
    const std::string & payload,
    attemory::context::CreateSessionOptions & options,
    attemory::context::ErrorInfo & error);

bool parse_memory_payload(
    const std::string & payload,
    attemory::context::MemoryInput & memory,
    attemory::context::ErrorInfo & error);

bool parse_search_payload(
    const std::string & payload,
    SearchRequest & parsed,
    attemory::context::ErrorInfo & error);

bool parse_oneshot_search_payload(
    const std::string & payload,
    OneShotSearchRequest & parsed,
    attemory::context::ErrorInfo & error);

} // namespace attemory::server
