#include <clocale>
#include <csignal>
#include <cstdio>
#include <string>

#include "attemory-core/attemory-core.h"
#include "context/context.h"
#include "server/http_common.h"
#include "server/routes.h"
#include "server/server_args.h"

#include "httplib.h"

#ifdef QUERY
#undef QUERY
#endif

namespace {

using attemory::context::AttemoryContext;
using attemory::context::ContextOptions;
using attemory::context::StartupInfo;

void print_startup_log(
    const attemory::server_args::Options & opts,
    const StartupInfo & info) {
    std::fprintf(
        stderr,
        "listening on %s:%d backend=%s engine=%s n_ctx=%d n_ctx_source=%s n_batch=%d n_ubatch=%d n_threads=%d n_threads_batch=%d kv_type_k=%s kv_type_v=%s chat_template=%s jinja=%s cache_scope=%s\n",
        opts.host.c_str(),
        opts.port,
        atmcore::backend_name(info.runtime.backend),
        info.runtime.use_native_engine ? "native" : "llama",
        info.startup_n_ctx,
        info.startup_n_ctx_source.c_str(),
        info.runtime.n_batch,
        info.runtime.n_ubatch,
        info.runtime.n_threads,
        info.runtime.n_threads_batch,
        atmcore::kv_type_name(info.runtime.kv_type_k),
        atmcore::kv_type_name(info.runtime.kv_type_v),
        opts.chat_template_source.c_str(),
        info.runtime.chat_template_use_jinja ? "true" : "false",
        info.model_cache_key.c_str());
    std::fprintf(
        stderr,
        "data_dir=%s cache_dir=%s\n",
        opts.data_dir.c_str(),
        opts.cache_dir.c_str());
    std::fprintf(
        stderr,
        "search_candidate_top_k=%d search_top_k=%d resident_kv_budget_bytes=%llu\n",
        info.search.candidate_top_k,
        info.search.top_k,
        (unsigned long long) info.resident_kv_budget_bytes);
    std::fprintf(stderr, "truncate=%s\n", info.truncate ? "true" : "false");
    std::fprintf(stderr, "http_log=%s\n", opts.http_log ? "true" : "false");
    std::fprintf(stderr, "run_log=%s\n", opts.run_log ? "true" : "false");
    std::fprintf(stderr, "restored_sessions=%zu\n", info.restored_sessions);
}

ContextOptions make_context_options(const attemory::server_args::Options & opts) {
    ContextOptions context_options;
    context_options.model_path = opts.model_path;
    context_options.model_tier = opts.model_tier;
    context_options.data_dir = opts.data_dir;
    context_options.cache_dir = opts.cache_dir;
    context_options.runtime = opts.runtime;
    context_options.search = opts.search;
    context_options.v = opts.v;
    context_options.resident_kv_budget_bytes = opts.resident_kv_budget_bytes;
    context_options.truncate = opts.truncate;
    context_options.run_log = opts.run_log;
    return context_options;
}

std::string startup_model_label(const attemory::server_args::Options & opts) {
    if (!opts.model_tier.empty()) {
        return "tier=" + opts.model_tier;
    }
    if (!opts.model_path.empty()) {
        return "model=" + opts.model_path;
    }
    return "model=unknown";
}

} // namespace

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");
    std::signal(SIGPIPE, SIG_IGN);

    attemory::server_args::Options opts;
    if (!attemory::server_args::parse_args(argc, argv, opts)) {
        attemory::server_args::print_usage(argv[0]);
        return 1;
    }

    AttemoryContext context;
    std::string error;
    if (!context.init(make_context_options(opts), error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    if (opts.run_log) {
        print_startup_log(opts, context.startup_info());
    }

    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(1); };
    attemory::server::configure_http_server(server);
    attemory::server::RouteOptions route_options;
    route_options.http_log = opts.http_log;
    attemory::server::install_routes(server, context, route_options);

    if (!server.bind_to_port(opts.host, opts.port)) {
        std::fprintf(stderr, "failed to listen on %s:%d\n", opts.host.c_str(), opts.port);
        context.shutdown();
        return 1;
    }

    const std::string model_label = startup_model_label(opts);
    std::fprintf(
        stderr,
        "attemory-server started; listening on %s:%d %s\n",
        opts.host.c_str(),
        opts.port,
        model_label.c_str());
    std::fflush(stderr);

    const bool ok = server.listen_after_bind();
    context.shutdown();
    return ok ? 0 : 1;
}
