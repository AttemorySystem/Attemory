#include "persistent/persistent.h"

#include <system_error>

namespace attemory::persistent {

std::string sanitize_storage_component(const std::string & value) {
    std::string result;
    result.reserve(value.size());

    bool last_was_sep = false;
    for (unsigned char ch : value) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '.' ||
            ch == '_' ||
            ch == '-';
        if (ok) {
            result.push_back((char) ch);
            last_was_sep = false;
        } else if (!result.empty() && !last_was_sep) {
            result.push_back('_');
            last_was_sep = true;
        }
    }

    while (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }

    if (result == "." || result == "..") {
        return {};
    }

    return result;
}

std::filesystem::path session_root_dir_path(const StorageLayout & layout) {
    return std::filesystem::path(layout.root_dir);
}

std::filesystem::path session_dir_path(const StorageLayout & layout) {
    return session_root_dir_path(layout) / layout.session_id;
}

std::filesystem::path session_meta_path(const StorageLayout & layout) {
    return session_dir_path(layout) / "session.meta";
}

std::filesystem::path session_memories_path(const StorageLayout & layout) {
    return session_dir_path(layout) / "memories.bin";
}

std::filesystem::path cache_root_dir_path(const CacheLayout & layout) {
    return std::filesystem::path(layout.root_dir) / layout.model_cache_key;
}

std::filesystem::path cache_session_dir_path(const CacheLayout & layout) {
    return cache_root_dir_path(layout) / layout.session_id;
}

std::filesystem::path cache_manifest_path(const CacheLayout & layout) {
    return cache_session_dir_path(layout) / "manifest.meta";
}

std::filesystem::path cache_segment_kv_path(const CacheLayout & layout, SegmentId segment_id) {
    return cache_session_dir_path(layout) / ("seg" + std::to_string(segment_id) + ".db");
}

std::filesystem::path cache_segment_search_cache_path(const CacheLayout & layout, SegmentId segment_id) {
    return cache_session_dir_path(layout) / ("seg" + std::to_string(segment_id) + ".search-cache");
}

ResultStatus ensure_directory(const std::filesystem::path & path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return {false, "failed to create directory: " + path.string() + ": " + ec.message()};
    }
    return {true, {}};
}

ResultStatus remove_file_if_exists(const std::filesystem::path & path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return {false, "failed to remove file: " + path.string() + ": " + ec.message()};
    }
    return {true, {}};
}

ResultStatus remove_dir_if_exists(const std::filesystem::path & path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        return {false, "failed to remove directory: " + path.string() + ": " + ec.message()};
    }
    return {true, {}};
}

} // namespace attemory::persistent
