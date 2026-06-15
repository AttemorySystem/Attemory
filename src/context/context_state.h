#pragma once

#include "attemory-core/attemory-core.h"
#include "context/context_types.h"
#include "context/kv/resident_kv_store.h"
#include "context/session/session_state.h"
#include "persistent/persistent.h"

#include <filesystem>

namespace attemory::context {

struct ContextState {
    ContextOptions options;
    std::filesystem::path data_dir;
    std::filesystem::path cache_dir;

    atmcore::Runtime * core = nullptr;

    attemory::persistent::ModelCacheKey model_cache_key;
    attemory::persistent::StorageLayout root_session_layout;

    SessionMap sessions;
    kv::ContextKVState kv;
    StartupInfo startup;
    bool native_search_warmup_done = false;
};

} // namespace attemory::context
