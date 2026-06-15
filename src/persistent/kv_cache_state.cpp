#include "persistent/persistent.h"

#include "attemory-core/attemory-core.h"

using namespace atmcore;

#include <fstream>
#include <filesystem>

namespace attemory::persistent {
namespace {

ResultStatus validate_kv_cache_state_request(
    const KVCacheStateLoadRequest & request) {
    if (request.cache.kv_path.empty()) {
        return {false, "kv cache path is empty"};
    }

    return {true, {}};
}

ResultStatus to_result(bool ok, const std::string & error) {
    return ok ? ResultStatus{true, {}} : ResultStatus{false, error};
}

ResultStatus ensure_kv_cache_parent_directory(const std::string & path) {
    const std::filesystem::path file_path(path);
    const std::filesystem::path parent = file_path.parent_path();
    if (!parent.empty()) {
        ResultStatus status = ensure_directory(parent);
        if (!status.ok) {
            return status;
        }
    }

    return {true, {}};
}

} // namespace

ResultStatus save_kv_cache_state(
    const KVCacheEntry & cache,
    const KVSnapshot & snapshot) {
    if (cache.kv_path.empty()) {
        return {false, "kv cache path is empty"};
    }

    std::string error;
    ResultStatus status = ensure_kv_cache_parent_directory(cache.kv_path);
    if (!status.ok) {
        return status;
    }

    const std::filesystem::path final_path(cache.kv_path);
    const std::filesystem::path temp_path = final_path.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {false, "failed to open kv cache file for write: " + temp_path.string()};
    }

    status = to_result(encode_kv_snapshot_to_stream(snapshot, out, error), error);
    if (!status.ok) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return status;
    }

    out.close();
    if (!out.good()) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return {false, "failed to write kv cache file: " + temp_path.string()};
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return {false, "failed to replace kv cache file: " + cache.kv_path + ": " + ec.message()};
    }

    return {true, {}};
}

ResultStatus save_kv_cache_state(
    const KVCacheEntry & cache,
    ActiveKV & snapshot) {
    if (cache.kv_path.empty()) {
        return {false, "kv cache path is empty"};
    }

    std::string error;
    ResultStatus status = ensure_kv_cache_parent_directory(cache.kv_path);
    if (!status.ok) {
        return status;
    }

    const std::filesystem::path final_path(cache.kv_path);
    const std::filesystem::path temp_path = final_path.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {false, "failed to open kv cache file for write: " + temp_path.string()};
    }

    status = to_result(encode_kv_snapshot_to_stream(snapshot, out, error), error);
    if (!status.ok) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return status;
    }

    out.close();
    if (!out.good()) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return {false, "failed to write kv cache file: " + temp_path.string()};
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return {false, "failed to replace kv cache file: " + cache.kv_path + ": " + ec.message()};
    }

    return {true, {}};
}

bool kv_cache_state_exists(const KVCacheEntry & cache) {
    if (cache.kv_path.empty()) {
        return false;
    }

    std::error_code ec;
    return std::filesystem::exists(cache.kv_path, ec) && !ec;
}

ResultStatus get_kv_cache_state_size(
    const KVCacheEntry & cache,
    int64_t & bytes) {
    bytes = 0;
    if (cache.kv_path.empty()) {
        return {false, "kv cache path is empty"};
    }

    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(cache.kv_path, ec);
    if (ec) {
        return {false, "failed to read kv cache file size: " + cache.kv_path + ": " + ec.message()};
    }

    bytes = (int64_t) size;
    return {true, {}};
}

ResultStatus load_kv_cache_state_snapshot(
    Runtime * core,
    const RuntimeOptions & config,
    const KVCacheStateLoadRequest & request,
    KVSnapshot & snapshot) {
    ResultStatus status = validate_kv_cache_state_request(request);
    if (!status.ok) {
        return status;
    }

    std::string error;
    std::ifstream in(request.cache.kv_path, std::ios::binary);
    if (!in.is_open()) {
        return {false, "failed to open kv cache file: " + request.cache.kv_path};
    }

    return to_result(
        decode_kv_snapshot_from_stream(
            core,
            config,
            in,
            request.system_text,
            request.memories,
            snapshot,
            error),
        error);
}

} // namespace attemory::persistent
