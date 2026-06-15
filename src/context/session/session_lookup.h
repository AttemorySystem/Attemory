#pragma once

#include "context/session/session_state.h"

#include <string>

namespace attemory::context {

Session * find_session(SessionMap & sessions, const std::string & session_id);
const Session * find_session(const SessionMap & sessions, const std::string & session_id);

} // namespace attemory::context
