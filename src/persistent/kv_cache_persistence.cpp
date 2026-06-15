#include "persistent/persistent.h"

#include "persistent/binary_io.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

namespace attemory::persistent {
namespace {

constexpr uint32_t kCacheManifestMagic = 0x414d4331u;
constexpr uint32_t kCacheManifestVersion = 1;
constexpr uint64_t kMaxCacheManifestSegments = 1000000;

bool write_model_key(std::ofstream & out, const ModelCacheKey & key) {
    using namespace binary_io;
    return write_string(out, key.value) &&
           write_string(out, key.vendor) &&
           write_string(out, key.model_desc) &&
           write_string(out, key.model_name) &&
           write_string(out, key.backend) &&
           write_string(out, key.template_hash) &&
           write_string(out, key.kv_type_k) &&
           write_string(out, key.kv_type_v);
}

bool read_model_key(std::ifstream & in, ModelCacheKey & key) {
    using namespace binary_io;
    if (!read_string(in, key.value) ||
        !read_string(in, key.vendor) ||
        !read_string(in, key.model_desc) ||
        !read_string(in, key.model_name) ||
        !read_string(in, key.backend) ||
        !read_string(in, key.template_hash) ||
        !read_string(in, key.kv_type_k) ||
        !read_string(in, key.kv_type_v)) {
        return false;
    }
    return true;
}

} // namespace

ResultStatus load_kv_cache_manifest(
    const CacheLayout & layout,
    CacheManifest & manifest) {
    using namespace binary_io;

    manifest = {};
    const std::filesystem::path path = cache_manifest_path(layout);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {false, "failed to open cache manifest file: " + path.string()};
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!read_pod(in, magic) || !read_pod(in, version)) {
        return {false, "failed to read cache manifest header"};
    }
    if (magic != kCacheManifestMagic || version != kCacheManifestVersion) {
        return {false, "unsupported cache manifest format"};
    }

    if (!read_pod(in, manifest.schema_version) ||
        !read_model_key(in, manifest.model) ||
        !read_string(in, manifest.session_id)) {
        return {false, "failed to read cache manifest body"};
    }

    uint64_t segment_count = 0;
    if (!read_pod(in, segment_count)) {
        return {false, "failed to read cache manifest segment count"};
    }
    if (segment_count > kMaxCacheManifestSegments ||
        segment_count > (uint64_t) std::numeric_limits<size_t>::max()) {
        return {false, "cache manifest segment count exceeds supported limit"};
    }

    manifest.segments.resize((size_t) segment_count);
    for (SegmentCacheManifest & segment : manifest.segments) {
        if (!read_pod(in, segment.segment_id) ||
            !read_pod(in, segment.first_memory_idx) ||
            !read_pod(in, segment.last_memory_idx_exclusive) ||
            !read_string(in, segment.segment_content_hash) ||
            !read_string(in, segment.kv_file) ||
            !read_string(in, segment.search_cache_file) ||
            !read_pod(in, segment.token_count) ||
            !read_pod(in, segment.bytes) ||
            !read_pod(in, segment.created_at_ms) ||
            !read_pod(in, segment.last_access_ms)) {
            return {false, "failed to read cache manifest segment"};
        }
    }

    return {true, {}};
}

ResultStatus save_kv_cache_manifest(
    const CacheLayout & layout,
    const CacheManifest & manifest) {
    using namespace binary_io;

    ResultStatus status = ensure_directory(cache_session_dir_path(layout));
    if (!status.ok) {
        return status;
    }

    const std::filesystem::path path = cache_manifest_path(layout);
    const std::filesystem::path temp_path = path.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {false, "failed to open cache manifest file: " + temp_path.string()};
    }

    bool ok =
        write_pod(out, kCacheManifestMagic) &&
        write_pod(out, kCacheManifestVersion) &&
        write_pod(out, manifest.schema_version) &&
        write_model_key(out, manifest.model) &&
        write_string(out, manifest.session_id);

    const uint64_t segment_count = (uint64_t) manifest.segments.size();
    ok = ok && write_pod(out, segment_count);
    for (const SegmentCacheManifest & segment : manifest.segments) {
        ok = ok &&
             write_pod(out, segment.segment_id) &&
             write_pod(out, segment.first_memory_idx) &&
             write_pod(out, segment.last_memory_idx_exclusive) &&
             write_string(out, segment.segment_content_hash) &&
             write_string(out, segment.kv_file) &&
             write_string(out, segment.search_cache_file) &&
             write_pod(out, segment.token_count) &&
             write_pod(out, segment.bytes) &&
             write_pod(out, segment.created_at_ms) &&
             write_pod(out, segment.last_access_ms);
        if (!ok) {
            break;
        }
    }

    out.close();
    if (!ok || !out.good()) {
        remove_file_if_exists(temp_path);
        return {false, "failed to write cache manifest file: " + path.string()};
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
        return {false, "failed to replace cache manifest file: " + path.string() + ": " + ec.message()};
    }

    return {true, {}};
}

ResultStatus remove_kv_cache(const CacheLayout & layout) {
    return remove_dir_if_exists(cache_session_dir_path(layout));
}

} // namespace attemory::persistent
