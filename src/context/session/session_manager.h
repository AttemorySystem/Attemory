#pragma once

#include "context/context_types.h"
#include "persistent/persistent.h"

#include <memory>
#include <string>
#include <vector>

namespace attemory::context {

class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager &) = delete;
    SessionManager & operator=(const SessionManager &) = delete;

    bool init(const ContextOptions & options, std::string & error);
    void shutdown();

    const StartupInfo & startup_info() const;
    const std::vector<int32_t> & v() const;

    std::vector<SessionStatus> list_sessions();
    const persistent::SessionStore * session_store(const std::string & session_id) const;

    CommandResult create_session(const std::string & session_id);
    CommandResult restore_session(const std::string & session_id);
    CommandResult add_system(const std::string & session_id, const std::string & system);
    CommandResult add_memory(const std::string & session_id, const MemoryInput & memory);
    CommandResult next_segment(const std::string & session_id);
    CommandResult clear_session(const std::string & session_id);
    CommandResult delete_session(const std::string & session_id);

    CommandResult index_session(const std::string & session_id);
    CommandResult save_session(const std::string & session_id);
    CommandResult search(
        const std::string & session_id,
        const std::string & query_text,
        const SearchRequestOverrides & request_overrides);
    CommandResult search(
        const std::string & session_id,
        const SearchInput & input,
        const SearchRequestOverrides & request_overrides);
    CommandResult oneshot_search(
        const std::string & system,
        const std::vector<OneShotMemoryInput> & memories,
        const std::string & query,
        const SearchRequestOverrides & request_overrides);
    CommandResult oneshot_search(
        const std::string & system,
        const std::vector<OneShotMemoryInput> & memories,
        const SearchInput & input,
        const SearchRequestOverrides & request_overrides);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace attemory::context
