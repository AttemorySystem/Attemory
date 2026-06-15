#pragma once

#include "persistent/persistent.h"

#include <filesystem>
#include <string>

namespace attemory::context {

attemory::persistent::StorageLayout session_storage_layout(
    const std::filesystem::path & data_dir,
    const attemory::persistent::ModelCacheKey & model_key,
    const std::string & session_id = std::string());

attemory::persistent::CacheLayout cache_layout_for_session(
    const std::filesystem::path & cache_dir,
    const attemory::persistent::ModelCacheKey & model_key,
    const std::string & session_id);

} // namespace attemory::context
