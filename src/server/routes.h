#pragma once

namespace httplib {
class Server;
} // namespace httplib

namespace attemory::context {
class AttemoryContext;
} // namespace attemory::context

namespace attemory::server {

struct RouteOptions {
    bool http_log = false;
};

void install_routes(
    httplib::Server & server,
    attemory::context::AttemoryContext & context,
    RouteOptions options = {});

} // namespace attemory::server
