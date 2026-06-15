#pragma once

#include "context/command/command_result.h"
#include "context/kv/resident_kv_store.h"
#include "context/kv/segment_kv_manager.h"
#include "context/session/session_state.h"

#include <filesystem>
#include <string>

namespace attemory::context {

struct SessionCommandContext {
    SessionMap & sessions;
    kv::ContextKVState & kv;
    atmcore::Runtime * core = nullptr;
    const RuntimeOptions & runtime;
    const std::filesystem::path & data_dir;
    const std::filesystem::path & cache_dir;
    const persistent::ModelCacheKey & model_key;
    bool run_log = false;
};

struct KVCacheCommandContext {
    SessionCommandContext command;
    bool & native_search_warmup_done;
};

struct Status {
    bool success = true;
    std::string error;
    ErrorInfo error_info;

    static Status ok();
    static Status fail(const std::string & message);
    static Status fail(const ErrorInfo & error);
};

class CommandScope {
public:
    CommandScope(
        SessionCommandContext context,
        kv::SegmentKVManager & kv_manager,
        AttemoryCommand command,
        const std::string & session_id);

    bool ok() const;
    CommandResult failure_result() const;

protected:
    void fail(const std::string & error);
    void fail(const ErrorInfo & error);
    void fail(ErrorCode code, const std::string & error);
    std::string & error_ref();

    SessionCommandContext context_;
    kv::SegmentKVManager & kv_manager_;
    std::string session_id_;
    Status status_;
};

class SessionScope : public CommandScope {
public:
    SessionScope(
        SessionCommandContext context,
        kv::SegmentKVManager & kv_manager,
        AttemoryCommand command,
        const std::string & session_id);

    bool require_session();
    bool require_planned_session();
    bool require_segments(const std::string & error = "session has no memory segments");
    bool persist();
    bool persist_if_dirty();

    Session & session();
    const Session & session() const;
    void evict_runtime();
    void clear_resident_kv();

private:
    Session * session_ = nullptr;
};

} // namespace attemory::context
