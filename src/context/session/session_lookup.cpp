#include "context/session/session_lookup.h"

namespace attemory::context {

Session * find_session(SessionMap & sessions, const std::string & session_id) {
    const auto it = sessions.find(session_id);
    return it == sessions.end() ? nullptr : &it->second;
}

const Session * find_session(const SessionMap & sessions, const std::string & session_id) {
    const auto it = sessions.find(session_id);
    return it == sessions.end() ? nullptr : &it->second;
}

} // namespace attemory::context
