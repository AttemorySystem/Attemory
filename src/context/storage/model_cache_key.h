#pragma once

#include "attemory-core/attemory-core.h"
#include "persistent/persistent.h"

#include <filesystem>
#include <string>

namespace attemory::context {

std::string stable_hash_hex(const std::string & value);

attemory::persistent::ModelCacheKey build_model_cache_key(
    const atmcore::RuntimeInfo & core_info);

std::filesystem::path normalize_storage_root(const std::string & storage_root);

} // namespace attemory::context
