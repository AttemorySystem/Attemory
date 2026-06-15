#include "context/storage/model_cache_key.h"

#include <cstdint>
#include <cstdio>

namespace attemory::context {

std::string stable_hash_hex(const std::string & value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }

    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) hash);
    return buf;
}

attemory::persistent::ModelCacheKey build_model_cache_key(
    const atmcore::RuntimeInfo & core_info) {
    const atmcore::RuntimeOptions & runtime = core_info.runtime;
    attemory::persistent::ModelCacheKey key;
    key.value = core_info.fingerprint;
    key.vendor = "core";
    key.model_desc = core_info.fingerprint;
    key.model_name = core_info.fingerprint;
    key.backend = atmcore::backend_name(runtime.backend);
    key.template_hash = stable_hash_hex(runtime.chat_template);
    key.kv_type_k = atmcore::kv_type_name(runtime.kv_type_k);
    key.kv_type_v = atmcore::kv_type_name(runtime.kv_type_v);

    return key;
}

std::filesystem::path normalize_storage_root(const std::string & storage_root) {
    const std::filesystem::path root =
        storage_root.empty() ? std::filesystem::path(".") : std::filesystem::path(storage_root);
    return std::filesystem::absolute(root).lexically_normal();
}

} // namespace attemory::context
