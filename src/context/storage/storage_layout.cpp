#include "context/storage/storage_layout.h"

namespace attemory::context {
namespace {

std::string safe_storage_component(
    const std::string & value,
    const char * fallback) {
    std::string sanitized = attemory::persistent::sanitize_storage_component(value);
    return sanitized.empty() ? std::string(fallback) : sanitized;
}

std::string session_model_component(
    const attemory::persistent::ModelCacheKey & model_key) {
    if (!model_key.model_name.empty()) {
        return model_key.model_name;
    }
    if (!model_key.model_desc.empty()) {
        return model_key.model_desc;
    }
    return model_key.value;
}

} // namespace

attemory::persistent::StorageLayout session_storage_layout(
    const std::filesystem::path & data_dir,
    const attemory::persistent::ModelCacheKey & model_key,
    const std::string & session_id) {
    attemory::persistent::StorageLayout layout;
    const std::string model_component =
        safe_storage_component(session_model_component(model_key), "model");
    layout.root_dir = (data_dir / model_component).string();
    layout.session_id = session_id.empty() ? std::string() : safe_storage_component(session_id, "session");
    return layout;
}

attemory::persistent::CacheLayout cache_layout_for_session(
    const std::filesystem::path & cache_dir,
    const attemory::persistent::ModelCacheKey & model_key,
    const std::string & session_id) {
    attemory::persistent::CacheLayout layout;
    layout.root_dir = cache_dir.string();
    layout.model_cache_key = safe_storage_component(model_key.value, "model");
    layout.session_id = session_id.empty() ? std::string() : safe_storage_component(session_id, "session");
    return layout;
}

} // namespace attemory::context
