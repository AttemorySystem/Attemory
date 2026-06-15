#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace atmcore {
struct KVSnapshot;
struct ActiveKV;
struct Runtime;
struct RuntimeOptions;
} // namespace atmcore

namespace attemory::persistent {

using MemoryIndex = int64_t;
using SegmentId = int32_t;

constexpr MemoryIndex kInvalidMemoryIndex = -1;
constexpr SegmentId kInvalidSegmentId = -1;

enum class CacheState : uint8_t {
    Missing,
    Building,
    Resident,
    DiskOnly,
    Invalid,
};

struct ResultStatus {
    bool ok = false;
    std::string error;
};

struct MemoryRecord {
    MemoryIndex memory_idx = kInvalidMemoryIndex;
    bool has_id = false;
    std::string id;
    std::string text;
    int32_t estimated_tokens = 0;
};

struct SessionStore {
    static constexpr uint32_t kCurrentSchemaVersion = 1;

    uint32_t schema_version = kCurrentSchemaVersion;
    std::string session_id;

    std::string system_text;
    bool system_locked = false;

    MemoryIndex next_memory_idx = 0;
    std::vector<MemoryRecord> memories;

    // User requested boundaries. A value N means memory_idx >= N starts a new
    // logical segment before automatic ctx-based splitting is applied.
    std::vector<MemoryIndex> manual_segment_boundaries;
};

struct StorageLayout {
    std::string root_dir = "sessions";
    std::string session_id;
};

using SessionStoreMap = std::unordered_map<std::string, SessionStore>;

inline bool is_valid_session_id(const std::string & session_id) {
    if (session_id.empty()) {
        return false;
    }
    if (session_id == "." || session_id == "..") {
        return false;
    }

    for (unsigned char ch : session_id) {
        const bool ok =
            std::isalnum(ch) != 0 ||
            ch == '.' ||
            ch == '_' ||
            ch == '-';
        if (!ok) {
            return false;
        }
    }

    return true;
}

inline const MemoryRecord * find_memory(
    const SessionStore & store,
    MemoryIndex memory_idx) {
    if (memory_idx >= 0 && (size_t) memory_idx < store.memories.size()) {
        const MemoryRecord & candidate = store.memories[(size_t) memory_idx];
        if (candidate.memory_idx == memory_idx) {
            return &candidate;
        }
    }

    for (const MemoryRecord & memory : store.memories) {
        if (memory.memory_idx == memory_idx) {
            return &memory;
        }
    }

    return nullptr;
}

struct ModelCacheKey {
    std::string value;
    std::string vendor;
    std::string model_desc;
    std::string model_name;
    std::string backend;
    std::string template_hash;

    std::string kv_type_k;
    std::string kv_type_v;
};

inline bool model_cache_key_equal(
    const ModelCacheKey & lhs,
    const ModelCacheKey & rhs) {
    return lhs.value == rhs.value &&
           lhs.vendor == rhs.vendor &&
           lhs.model_desc == rhs.model_desc &&
           lhs.model_name == rhs.model_name &&
           lhs.backend == rhs.backend &&
           lhs.template_hash == rhs.template_hash &&
           lhs.kv_type_k == rhs.kv_type_k &&
           lhs.kv_type_v == rhs.kv_type_v;
}

struct SegmentCacheKey {
    ModelCacheKey model;
    std::string session_id;
    SegmentId segment_id = kInvalidSegmentId;
    std::string segment_content_hash;
};

struct SegmentCacheManifest {
    SegmentId segment_id = kInvalidSegmentId;
    MemoryIndex first_memory_idx = kInvalidMemoryIndex;
    MemoryIndex last_memory_idx_exclusive = kInvalidMemoryIndex;

    std::string segment_content_hash;
    std::string kv_file;
    std::string search_cache_file;

    int32_t token_count = 0;
    int64_t bytes = 0;
    int64_t created_at_ms = 0;
    int64_t last_access_ms = 0;
};

struct CacheManifest {
    static constexpr uint32_t kCurrentSchemaVersion = 1;

    uint32_t schema_version = kCurrentSchemaVersion;
    ModelCacheKey model;
    std::string session_id;
    std::vector<SegmentCacheManifest> segments;
};

struct KVCacheEntry {
    SegmentCacheKey key;
    CacheState state = CacheState::Missing;

    std::string kv_path;
    std::string search_cache_path;

    int32_t token_count = 0;
    int64_t bytes_on_disk = 0;
    int64_t bytes_in_memory = 0;

    int64_t created_at_ms = 0;
    int64_t last_access_ms = 0;
};

struct CacheLayout {
    std::string root_dir = "cache";
    std::string model_cache_key;
    std::string session_id;
};

struct KVCacheStateLoadRequest {
    KVCacheEntry cache;
    std::string system_text;
    std::vector<std::string> memories;
};

std::string sanitize_storage_component(const std::string & value);

std::filesystem::path session_root_dir_path(const StorageLayout & layout);
std::filesystem::path session_dir_path(const StorageLayout & layout);
std::filesystem::path session_meta_path(const StorageLayout & layout);
std::filesystem::path session_memories_path(const StorageLayout & layout);

std::filesystem::path cache_root_dir_path(const CacheLayout & layout);
std::filesystem::path cache_session_dir_path(const CacheLayout & layout);
std::filesystem::path cache_manifest_path(const CacheLayout & layout);
std::filesystem::path cache_segment_kv_path(const CacheLayout & layout, SegmentId segment_id);
std::filesystem::path cache_segment_search_cache_path(const CacheLayout & layout, SegmentId segment_id);

ResultStatus ensure_directory(const std::filesystem::path & path);
ResultStatus remove_file_if_exists(const std::filesystem::path & path);
ResultStatus remove_dir_if_exists(const std::filesystem::path & path);

ResultStatus load_session(const StorageLayout & layout, SessionStore & session);
ResultStatus save_session(const StorageLayout & layout, const SessionStore & session);
ResultStatus save_session_metadata(const StorageLayout & layout, const SessionStore & session);
ResultStatus remove_session(const StorageLayout & layout);
ResultStatus scan_sessions(const StorageLayout & root_layout, SessionStoreMap & sessions);

ResultStatus load_kv_cache_manifest(const CacheLayout & layout, CacheManifest & manifest);
ResultStatus save_kv_cache_manifest(const CacheLayout & layout, const CacheManifest & manifest);
ResultStatus remove_kv_cache(const CacheLayout & layout);

ResultStatus save_kv_cache_state(const KVCacheEntry & cache, const atmcore::KVSnapshot & snapshot);
ResultStatus save_kv_cache_state(const KVCacheEntry & cache, atmcore::ActiveKV & snapshot);
bool kv_cache_state_exists(const KVCacheEntry & cache);
ResultStatus get_kv_cache_state_size(const KVCacheEntry & cache, int64_t & bytes);
ResultStatus load_kv_cache_state_snapshot(
    atmcore::Runtime * core,
    const atmcore::RuntimeOptions & config,
    const KVCacheStateLoadRequest & request,
    atmcore::KVSnapshot & snapshot);

} // namespace attemory::persistent
