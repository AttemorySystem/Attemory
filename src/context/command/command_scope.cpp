#include "context/command/command_scope.h"

#include "context/session/segment_planner.h"
#include "context/session/session_lookup.h"
#include "context/storage/storage_layout.h"
#include "persistent/persistent.h"

#include <filesystem>

namespace attemory::context {
namespace {

bool persist_session_store(
    const std::filesystem::path & data_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    std::string & error) {
    const persistent::StorageLayout layout =
        session_storage_layout(data_dir, model_key, session.store.session_id);
    if (!result_to_bool(persistent::save_session(layout, session.store), error)) {
        session.facts_dirty = true;
        return false;
    }

    session.facts_dirty = false;
    return true;
}

bool persist_session_store_if_dirty(
    const std::filesystem::path & data_dir,
    const persistent::ModelCacheKey & model_key,
    Session & session,
    std::string & error) {
    if (!session.facts_dirty) {
        error.clear();
        return true;
    }

    return persist_session_store(data_dir, model_key, session, error);
}

} // namespace

Status Status::ok() {
    return {};
}

Status Status::fail(const std::string & message) {
    Status status;
    status.success = false;
    status.error = message;
    status.error_info = infer_error_info(message);
    return status;
}

Status Status::fail(const ErrorInfo & error) {
    Status status;
    status.success = false;
    status.error = error.message;
    status.error_info = error;
    return status;
}

CommandScope::CommandScope(
    SessionCommandContext context,
    kv::SegmentKVManager & kv_manager,
    AttemoryCommand command,
    const std::string & session_id) :
    context_(context),
    kv_manager_(kv_manager),
    session_id_(session_id) {
    if (!persistent::is_valid_session_id(session_id_)) {
        fail(ErrorCode::InvalidSessionId, "invalid session id");
        return;
    }

    kv_manager_.evict_active_for_command(command, session_id_);
    if (command != AttemoryCommand::CREATE_SESSION) {
        if (!kv_manager_.keep_only_session_live(session_id_, status_.error)) {
            status_.success = false;
            return;
        }
    }
}

bool CommandScope::ok() const {
    return status_.success;
}

CommandResult CommandScope::failure_result() const {
    if (!status_.error_info.message.empty()) {
        return make_error(status_.error_info);
    }
    return make_error(status_.error);
}

void CommandScope::fail(const std::string & error) {
    status_ = Status::fail(error);
}

void CommandScope::fail(const ErrorInfo & error) {
    status_ = Status::fail(error);
}

void CommandScope::fail(ErrorCode code, const std::string & error) {
    fail(make_error_info(code, error));
}

std::string & CommandScope::error_ref() {
    status_.success = false;
    status_.error_info = {};
    return status_.error;
}

SessionScope::SessionScope(
    SessionCommandContext context,
    kv::SegmentKVManager & kv_manager,
    AttemoryCommand command,
    const std::string & session_id) :
    CommandScope(context, kv_manager, command, session_id) {
}

bool SessionScope::require_session() {
    if (!ok()) {
        return false;
    }
    if (session_ != nullptr) {
        return true;
    }
    session_ = find_session(context_.sessions, session_id_);
    if (session_ == nullptr) {
        fail(
            make_error_info(
                ErrorCode::SessionNotFound,
                "session not found: " + session_id_,
                {{"session_id", session_id_}}));
        return false;
    }
    status_ = Status::ok();
    return true;
}

bool SessionScope::require_planned_session() {
    if (!require_session()) {
        return false;
    }
    if (!prepare_session_plan(
            context_.core,
            context_.runtime,
            context_.cache_dir,
            context_.model_key,
            *session_,
            error_ref())) {
        status_.error_info = make_error_info(ErrorCode::InternalError, status_.error);
        return false;
    }
    status_ = Status::ok();
    return true;
}

bool SessionScope::require_segments(const std::string & error) {
    if (!require_session()) {
        return false;
    }
    if (session_->segment_plan.segments.empty()) {
        fail(ErrorCode::InvalidRequest, error);
        return false;
    }
    return true;
}

bool SessionScope::persist() {
    if (!require_session()) {
        return false;
    }
    if (!persist_session_store(context_.data_dir, context_.model_key, *session_, error_ref())) {
        status_.error_info = make_error_info(ErrorCode::InternalError, status_.error);
        return false;
    }
    status_ = Status::ok();
    return true;
}

bool SessionScope::persist_if_dirty() {
    if (!require_session()) {
        return false;
    }
    if (!persist_session_store_if_dirty(context_.data_dir, context_.model_key, *session_, error_ref())) {
        status_.error_info = make_error_info(ErrorCode::InternalError, status_.error);
        return false;
    }
    status_ = Status::ok();
    return true;
}

Session & SessionScope::session() {
    return *session_;
}

const Session & SessionScope::session() const {
    return *session_;
}

void SessionScope::evict_runtime() {
    if (session_ != nullptr) {
        kv_manager_.evict_session(session_->store.session_id);
    }
}

void SessionScope::clear_resident_kv() {
    if (session_ != nullptr) {
        kv_manager_.clear_resident_session(session_->store.session_id);
    }
}

} // namespace attemory::context
