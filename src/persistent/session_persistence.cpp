#include "persistent/persistent.h"

#include "persistent/binary_io.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace attemory::persistent {
namespace {

constexpr uint32_t kSessionStoreMagic = 0x414d5331u;
constexpr uint32_t kSessionStoreVersion = 2;
constexpr uint32_t kSessionMemoriesMagic = 0x414d4d31u;
constexpr uint32_t kSessionMemoriesVersion = 1;
constexpr uint64_t kMaxSessionMemoryRecords = 1000000;

bool write_memory_record(
    std::ofstream & out,
    const MemoryRecord & memory) {
    using namespace binary_io;

    return write_pod(out, memory.memory_idx) &&
           write_bool(out, memory.has_id) &&
           write_string(out, memory.id) &&
           write_string(out, memory.text) &&
           write_pod(out, memory.estimated_tokens);
}

bool read_memory_record(
    std::ifstream & in,
    uint32_t version,
    MemoryRecord & memory) {
    using namespace binary_io;

    memory = {};
    if (!read_pod(in, memory.memory_idx)) {
        return false;
    }

    if (version == kSessionMemoriesVersion) {
        return read_bool(in, memory.has_id) &&
               read_string(in, memory.id) &&
               read_string(in, memory.text) &&
               read_pod(in, memory.estimated_tokens);
    }

    return false;
}

bool is_supported_session_store_version(uint32_t version) {
    return version == 1 || version == kSessionStoreVersion;
}

bool is_supported_session_memories_version(uint32_t version) {
    return version == kSessionMemoriesVersion;
}

ResultStatus write_store_metadata_to_path(
    const std::filesystem::path & path,
    const SessionStore & store) {
    using namespace binary_io;

    const std::filesystem::path temp_path = path.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {false, "failed to open session store file: " + temp_path.string()};
    }

    bool ok =
        write_pod(out, kSessionStoreMagic) &&
        write_pod(out, kSessionStoreVersion) &&
        write_pod(out, store.schema_version) &&
        write_string(out, store.session_id) &&
        write_string(out, store.system_text) &&
        write_bool(out, store.system_locked) &&
        write_bool(out, store.kv_persist) &&
        write_pod(out, store.next_memory_idx) &&
        write_i64_vector(out, store.manual_segment_boundaries) &&
        write_pod(out, (uint64_t) store.memories.size());

    out.close();
    if (!ok || !out.good()) {
        std::string ignored;
        remove_file_if_exists(temp_path);
        return {false, "failed to write session store file: " + path.string()};
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp_path, path, ec);
    }
    if (ec) {
        remove_file_if_exists(temp_path);
        return {false, "failed to replace session store file: " + path.string() + ": " + ec.message()};
    }

    return {true, {}};
}

ResultStatus write_store_memories_to_path(
    const std::filesystem::path & path,
    const SessionStore & store) {
    using namespace binary_io;

    const std::filesystem::path temp_path = path.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {false, "failed to open session memories file: " + temp_path.string()};
    }

    bool ok =
        write_pod(out, kSessionMemoriesMagic) &&
        write_pod(out, kSessionMemoriesVersion);
    for (const MemoryRecord & memory : store.memories) {
        ok = ok && write_memory_record(out, memory);
        if (!ok) {
            break;
        }
    }

    out.close();
    if (!ok || !out.good()) {
        remove_file_if_exists(temp_path);
        return {false, "failed to write session memories file: " + path.string()};
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp_path, path, ec);
    }
    if (ec) {
        remove_file_if_exists(temp_path);
        return {false, "failed to replace session memories file: " + path.string() + ": " + ec.message()};
    }

    return {true, {}};
}

ResultStatus read_store_metadata_from_path(
    const std::filesystem::path & path,
    SessionStore & store,
    uint64_t & memory_count) {
    using namespace binary_io;

    store = {};
    memory_count = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {false, "failed to open session store file: " + path.string()};
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!read_pod(in, magic) || !read_pod(in, version)) {
        return {false, "failed to read session store header"};
    }
    if (magic != kSessionStoreMagic || !is_supported_session_store_version(version)) {
        return {false, "unsupported session store format"};
    }

    if (!read_pod(in, store.schema_version) ||
        !read_string(in, store.session_id) ||
        !read_string(in, store.system_text) ||
        !read_bool(in, store.system_locked)) {
        return {false, "failed to read session store body"};
    }
    if (version >= 2 && !read_bool(in, store.kv_persist)) {
        return {false, "failed to read session store body"};
    }
    if (!read_pod(in, store.next_memory_idx) ||
        !read_i64_vector(in, store.manual_segment_boundaries)) {
        return {false, "failed to read session store body"};
    }

    if (!read_pod(in, memory_count)) {
        return {false, "failed to read session store body"};
    }
    if (memory_count > kMaxSessionMemoryRecords ||
        memory_count > (uint64_t) std::numeric_limits<size_t>::max()) {
        return {false, "session memory count exceeds supported limit"};
    }

    if (!is_valid_session_id(store.session_id)) {
        return {false, "invalid session id in store"};
    }

    return {true, {}};
}

ResultStatus read_store_memories_from_path(
    const std::filesystem::path & path,
    uint64_t memory_count,
    SessionStore & store) {
    using namespace binary_io;

    if (memory_count == 0) {
        store.memories.clear();
        return {true, {}};
    }
    if (memory_count > kMaxSessionMemoryRecords ||
        memory_count > (uint64_t) std::numeric_limits<size_t>::max()) {
        return {false, "session memory count exceeds supported limit"};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {false, "failed to open session memories file: " + path.string()};
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!read_pod(in, magic) || !read_pod(in, version)) {
        return {false, "failed to read session memories header"};
    }
    if (magic != kSessionMemoriesMagic || !is_supported_session_memories_version(version)) {
        return {false, "unsupported session memories format"};
    }

    store.memories.resize((size_t) memory_count);
    for (MemoryRecord & memory : store.memories) {
        if (!read_memory_record(in, version, memory)) {
            return {false, "failed to read memory record"};
        }
    }

    return {true, {}};
}

ResultStatus validate_loaded_store(const SessionStore & store) {
    if (!is_valid_session_id(store.session_id)) {
        return {false, "invalid session id in store"};
    }
    if (store.next_memory_idx < 0) {
        return {false, "invalid next memory index in store"};
    }

    std::unordered_set<MemoryIndex> memory_indices;
    memory_indices.reserve(store.memories.size());
    for (const MemoryRecord & memory : store.memories) {
        if (memory.memory_idx < 0 || memory.memory_idx >= store.next_memory_idx) {
            return {false, "invalid memory index in store"};
        }
        if (!memory_indices.insert(memory.memory_idx).second) {
            return {false, "duplicate memory index in store"};
        }
    }

    for (MemoryIndex boundary : store.manual_segment_boundaries) {
        if (boundary < 0 || boundary > store.next_memory_idx) {
            return {false, "invalid manual segment boundary in store"};
        }
    }

    return {true, {}};
}

} // namespace

ResultStatus load_session(
    const StorageLayout & layout,
    SessionStore & store) {
    uint64_t memory_count = 0;
    ResultStatus status = read_store_metadata_from_path(session_meta_path(layout), store, memory_count);
    if (!status.ok) {
        return status;
    }
    status = read_store_memories_from_path(session_memories_path(layout), memory_count, store);
    if (!status.ok) {
        return status;
    }
    if (!layout.session_id.empty() && store.session_id != layout.session_id) {
        store = {};
        return {false, "session store path does not match session id"};
    }
    return validate_loaded_store(store);
}

ResultStatus save_session(
    const StorageLayout & layout,
    const SessionStore & store) {
    ResultStatus status = validate_loaded_store(store);
    if (!status.ok) {
        return status;
    }
    if (!layout.session_id.empty() && store.session_id != layout.session_id) {
        return {false, "session store path does not match session id"};
    }

    status = ensure_directory(session_dir_path(layout));
    if (!status.ok) {
        return status;
    }

    status = write_store_memories_to_path(session_memories_path(layout), store);
    if (!status.ok) {
        return status;
    }
    status = write_store_metadata_to_path(session_meta_path(layout), store);
    if (!status.ok) {
        return status;
    }
    return {true, {}};
}

ResultStatus save_session_metadata(
    const StorageLayout & layout,
    const SessionStore & store) {
    ResultStatus status = validate_loaded_store(store);
    if (!status.ok) {
        return status;
    }
    if (!layout.session_id.empty() && store.session_id != layout.session_id) {
        return {false, "session store path does not match session id"};
    }

    status = ensure_directory(session_dir_path(layout));
    if (!status.ok) {
        return status;
    }
    return write_store_metadata_to_path(session_meta_path(layout), store);
}

ResultStatus remove_session(
    const StorageLayout & layout) {
    return remove_dir_if_exists(session_dir_path(layout));
}

ResultStatus scan_sessions(
    const StorageLayout & root_layout,
    SessionStoreMap & sessions) {
    sessions.clear();

    ResultStatus status = ensure_directory(session_root_dir_path(root_layout));
    if (!status.ok) {
        return status;
    }

    std::error_code ec;
    for (const std::filesystem::directory_entry & entry :
         std::filesystem::directory_iterator(session_root_dir_path(root_layout), ec)) {
        if (ec) {
            return {false, "failed to scan sessions directory: " + ec.message()};
        }
        if (!entry.is_directory()) {
            continue;
        }

        const std::string session_id = entry.path().filename().string();
        if (!is_valid_session_id(session_id)) {
            continue;
        }

        StorageLayout layout = root_layout;
        layout.session_id = session_id;
        if (!std::filesystem::exists(session_meta_path(layout))) {
            continue;
        }

        SessionStore session;
        status = load_session(layout, session);
        if (!status.ok) {
            continue;
        }
        sessions.emplace(session.session_id, std::move(session));
    }

    return {true, {}};
}

} // namespace attemory::persistent
