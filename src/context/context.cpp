#include "context/context.h"

#include "context/session/session_manager.h"

#include <memory>

namespace attemory::context {

struct AttemoryContext::Impl {
    SessionManager session_manager;
};

AttemoryContext::AttemoryContext() :
    impl_(new Impl()) {
}

AttemoryContext::~AttemoryContext() {
    shutdown();
}

bool AttemoryContext::init(const ContextOptions & options, std::string & error) {
    return impl_->session_manager.init(options, error);
}

void AttemoryContext::shutdown() {
    if (impl_ == nullptr) {
        return;
    }

    impl_->session_manager.shutdown();
}

const StartupInfo & AttemoryContext::startup_info() const {
    return impl_->session_manager.startup_info();
}

const std::vector<int32_t> & AttemoryContext::v() const {
    return impl_->session_manager.v();
}

std::vector<SessionStatus> AttemoryContext::list_sessions() {
    return impl_->session_manager.list_sessions();
}

const persistent::SessionStore * AttemoryContext::session_store(const std::string & session_id) const {
    return impl_->session_manager.session_store(session_id);
}

CommandResult AttemoryContext::create_session(
    const std::string & session_id,
    const CreateSessionOptions & options) {
    return impl_->session_manager.create_session(session_id, options);
}

CommandResult AttemoryContext::restore_session(const std::string & session_id) {
    return impl_->session_manager.restore_session(session_id);
}

CommandResult AttemoryContext::add_system(const std::string & session_id, const std::string & system) {
    return impl_->session_manager.add_system(session_id, system);
}

CommandResult AttemoryContext::add_memory(const std::string & session_id, const MemoryInput & memory) {
    return impl_->session_manager.add_memory(session_id, memory);
}

CommandResult AttemoryContext::next_segment(const std::string & session_id) {
    return impl_->session_manager.next_segment(session_id);
}

CommandResult AttemoryContext::clear_session(const std::string & session_id) {
    return impl_->session_manager.clear_session(session_id);
}

CommandResult AttemoryContext::delete_session(const std::string & session_id) {
    return impl_->session_manager.delete_session(session_id);
}

CommandResult AttemoryContext::index_session(const std::string & session_id) {
    return impl_->session_manager.index_session(session_id);
}

CommandResult AttemoryContext::save_session(const std::string & session_id) {
    return impl_->session_manager.save_session(session_id);
}

CommandResult AttemoryContext::search(
    const std::string & session_id,
    const std::string & query_text,
    const SearchRequestOverrides & request_overrides) {
    return search(session_id, SearchInput{std::string(), query_text}, request_overrides);
}

CommandResult AttemoryContext::search(
    const std::string & session_id,
    const SearchInput & input,
    const SearchRequestOverrides & request_overrides) {
    return impl_->session_manager.search(session_id, input, request_overrides);
}

CommandResult AttemoryContext::oneshot_search(
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const std::string & query,
    const SearchRequestOverrides & request_overrides) {
    return oneshot_search(system, memories, SearchInput{std::string(), query}, request_overrides);
}

CommandResult AttemoryContext::oneshot_search(
    const std::string & system,
    const std::vector<OneShotMemoryInput> & memories,
    const SearchInput & input,
    const SearchRequestOverrides & request_overrides) {
    return impl_->session_manager.oneshot_search(system, memories, input, request_overrides);
}

} // namespace attemory::context
