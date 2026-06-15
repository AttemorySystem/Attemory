#pragma once

#include "attemory-core/attemory-core.h"
#include "context/context_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace attemory::server_args {

using RuntimeOptions = atmcore::RuntimeOptions;

struct Options {
    std::string model_path;
    std::string model_tier;
    std::string data_dir;
    std::string cache_dir;
    std::string host = "127.0.0.1";
    std::string chat_template_source = "model/default";
    int port = 9006;
    std::vector<int32_t> v;
    bool truncate = true;
    bool http_log = false;
    bool run_log = false;
    uint64_t resident_kv_budget_bytes = 0;
    RuntimeOptions runtime;
    attemory::context::SearchConfig search;
};

void print_usage(const char * program);
bool parse_args(int argc, char ** argv, Options & opts);
std::string join_v(const std::vector<int32_t> & v);

} // namespace attemory::server_args
