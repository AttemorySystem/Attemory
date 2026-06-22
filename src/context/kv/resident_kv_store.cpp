#include "context/kv/resident_kv_store.h"

#include "attemory-core/attemory-core.h"

#include <limits>
#include <memory>
#include <string>

namespace attemory::context::kv {
namespace {

uint64_t resident_snapshot_ram_bytes(const SegmentKVHandle & handle) {
    if (handle == nullptr) {
        return 0;
    }
    return handle->has_snapshot ? (uint64_t) handle->snapshot.base_seq_state_blob.size() : 0;
}

bool release_live_context_to_snapshot(
    const SegmentKVHandle & handle,
    std::string & error) {
    error.clear();
    if (handle == nullptr) {
        return true;
    }
    if (!segment_kv_has_live_context(handle)) {
        return true;
    }
    if (!ensure_segment_kv_snapshot(handle, error)) {
        return false;
    }
    atmcore::release_active_kv_context(handle->active);
    handle->active.metadata = atmcore::KVMetadata();
    return true;
}

ResidentSegmentKVMap * find_resident_session_kv(
    ResidentSessionKVMap & resident_segments,
    const std::string & session_id) {
    const auto it = resident_segments.find(session_id);
    if (it == resident_segments.end()) {
        return nullptr;
    }
    return &it->second;
}

const ResidentSegmentKVMap * find_resident_session_kv(
    const ResidentSessionKVMap & resident_segments,
    const std::string & session_id) {
    const auto it = resident_segments.find(session_id);
    if (it == resident_segments.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace

SegmentKVHandle make_segment_kv_handle() {
    return std::make_shared<SegmentKVState>();
}

bool segment_kv_has_live_context(const SegmentKVHandle & handle) {
    return handle != nullptr &&
           (handle->active.ctx != nullptr || handle->active.base_kv != nullptr);
}

bool segment_kv_has_snapshot_blob(const SegmentKVHandle & handle) {
    return handle != nullptr &&
           handle->has_snapshot &&
           !handle->snapshot.base_seq_state_blob.empty();
}

const atmcore::KVMetadata * segment_kv_metadata(const SegmentKVHandle & handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    if (segment_kv_has_live_context(handle) ||
        !handle->active.metadata.base_tokens.empty()) {
        return &handle->active.metadata;
    }
    return handle->has_snapshot ? &handle->snapshot.metadata : nullptr;
}

size_t segment_kv_base_token_count(const SegmentKVHandle & handle) {
    const atmcore::KVMetadata * metadata = segment_kv_metadata(handle);
    return metadata != nullptr ? metadata->base_tokens.size() : 0;
}

bool ensure_segment_kv_snapshot(const SegmentKVHandle & handle, std::string & error) {
    error.clear();
    if (handle == nullptr) {
        error = "segment KV handle is null";
        return false;
    }
    if (!segment_kv_has_live_context(handle)) {
        if (handle->has_snapshot) {
            return true;
        }
        error = "segment KV has neither snapshot nor live context";
        return false;
    }
    if (!atmcore::export_kv_snapshot(handle->active, handle->snapshot, error)) {
        return false;
    }
    handle->has_snapshot = true;
    return true;
}

void release_segment_kv_context(const SegmentKVHandle & handle) {
    if (handle != nullptr) {
        atmcore::release_active_kv_context(handle->active);
        if (handle->has_snapshot) {
            handle->active.metadata = atmcore::KVMetadata();
        }
    }
}

void ResidentKVStore::set_budget_bytes(uint64_t bytes) {
    budget_bytes_ = bytes;
}

uint64_t ResidentKVStore::budget_bytes() const {
    return budget_bytes_;
}

uint64_t ResidentKVStore::resident_bytes() const {
    return resident_bytes_;
}

SegmentKVHandle ResidentKVStore::find(const std::string & session_id, int32_t segment_id) {
    ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    if (session_segments == nullptr) {
        return nullptr;
    }

    const auto it = session_segments->find(segment_id);
    if (it == session_segments->end()) {
        return nullptr;
    }

    touch(it->second);
    return it->second.handle;
}

SegmentKVHandle ResidentKVStore::peek(const std::string & session_id, int32_t segment_id) const {
    const ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    if (session_segments == nullptr) {
        return nullptr;
    }

    const auto it = session_segments->find(segment_id);
    if (it == session_segments->end()) {
        return nullptr;
    }

    return it->second.handle;
}

bool ResidentKVStore::store(
    const std::string & session_id,
    int32_t segment_id,
    const SegmentKVHandle & handle,
    std::string & error) {
    error.clear();
    if (handle == nullptr) {
        remove(session_id, segment_id);
        return true;
    }

    ResidentKVEntry entry;
    entry.handle = handle;
    if (!refresh_entry_bytes(entry, error)) {
        return false;
    }
    touch(entry);

    ResidentSegmentKVMap & session_segments = resident_segments_[session_id];
    const auto previous = session_segments.find(segment_id);
    const uint64_t previous_bytes = previous == session_segments.end() ? 0 : previous->second.bytes;
    const uint64_t current_without_previous = resident_bytes_ - previous_bytes;
    if (entry.bytes > std::numeric_limits<uint64_t>::max() - current_without_previous) {
        error = "resident KV byte accounting overflow";
        return false;
    }

    if (previous != session_segments.end()) {
        resident_bytes_ -= previous_bytes;
    }

    session_segments[segment_id] = entry;
    resident_bytes_ += entry.bytes;

    return evict_to_budget(session_id, segment_id, error);
}

void ResidentKVStore::remove(const std::string & session_id, int32_t segment_id) {
    ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    if (session_segments == nullptr) {
        return;
    }

    const auto it = session_segments->find(segment_id);
    if (it == session_segments->end()) {
        return;
    }

    resident_bytes_ -= it->second.bytes;
    session_segments->erase(it);
    if (session_segments->empty()) {
        resident_segments_.erase(session_id);
    }
}

void ResidentKVStore::clear_session(const std::string & session_id) {
    ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    if (session_segments == nullptr) {
        return;
    }

    for (const auto & item : *session_segments) {
        resident_bytes_ -= item.second.bytes;
    }
    resident_segments_.erase(session_id);
}

void ResidentKVStore::clear() {
    resident_segments_.clear();
    resident_bytes_ = 0;
}

int32_t ResidentKVStore::count_session(const std::string & session_id) const {
    const ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    return session_segments == nullptr ? 0 : (int32_t) session_segments->size();
}

bool ResidentKVStore::release_all_live_contexts(std::string & error) {
    error.clear();
    for (auto & session_item : resident_segments_) {
        for (auto & segment_item : session_item.second) {
            if (!release_live_context_to_snapshot(segment_item.second.handle, error)) {
                error =
                    "failed to release resident live context: session=" +
                    session_item.first +
                    " segment=" +
                    std::to_string(segment_item.first) +
                    " reason=" +
                    error;
                return false;
            }

            const uint64_t previous_bytes = segment_item.second.bytes;
            if (!refresh_entry_bytes(segment_item.second, error)) {
                return false;
            }
            resident_bytes_ = resident_bytes_ - previous_bytes + segment_item.second.bytes;
        }
    }
    return true;
}

bool ResidentKVStore::keep_only_session_live(const std::string & session_id, std::string & error) {
    error.clear();
    for (auto & session_item : resident_segments_) {
        if (session_item.first == session_id) {
            continue;
        }
        for (auto & segment_item : session_item.second) {
            if (!release_live_context_to_snapshot(segment_item.second.handle, error)) {
                error =
                    "failed to release resident live context: session=" +
                    session_item.first +
                    " segment=" +
                    std::to_string(segment_item.first) +
                    " reason=" +
                    error;
                return false;
            }

            const uint64_t previous_bytes = segment_item.second.bytes;
            if (!refresh_entry_bytes(segment_item.second, error)) {
                return false;
            }
            resident_bytes_ = resident_bytes_ - previous_bytes + segment_item.second.bytes;
        }
    }
    return evict_to_budget(session_id, -1, error);
}

bool ResidentKVStore::keep_only_segment_live(
    const std::string & session_id,
    int32_t segment_id,
    std::string & error) {
    error.clear();
    ResidentSegmentKVMap * session_segments = find_resident_session_kv(resident_segments_, session_id);
    if (session_segments == nullptr) {
        return true;
    }

    for (auto & item : *session_segments) {
        if (item.first == segment_id) {
            continue;
        }
        if (!release_live_context_to_snapshot(item.second.handle, error)) {
            error =
                "failed to release resident live context: session=" +
                session_id +
                " segment=" +
                std::to_string(item.first) +
                " reason=" +
                error;
            return false;
        }

        const uint64_t previous_bytes = item.second.bytes;
        if (!refresh_entry_bytes(item.second, error)) {
            return false;
        }
        resident_bytes_ = resident_bytes_ - previous_bytes + item.second.bytes;
    }
    return evict_to_budget(session_id, segment_id, error);
}

bool ResidentKVStore::evict_to_budget(
    const std::string & protected_session_id,
    int32_t protected_segment_id,
    std::string & error) {
    error.clear();
    if (budget_bytes_ == 0) {
        return true;
    }

    while (resident_bytes_ > budget_bytes_) {
        ResidentSessionKVMap::iterator candidate_session = resident_segments_.end();
        ResidentSegmentKVMap::iterator candidate_segment;
        uint64_t oldest_access = std::numeric_limits<uint64_t>::max();

        for (auto session_it = resident_segments_.begin(); session_it != resident_segments_.end(); ++session_it) {
            for (auto segment_it = session_it->second.begin(); segment_it != session_it->second.end(); ++segment_it) {
                if (session_it->first == protected_session_id &&
                    (protected_segment_id < 0 || segment_it->first == protected_segment_id)) {
                    continue;
                }
                if (segment_it->second.last_access < oldest_access) {
                    oldest_access = segment_it->second.last_access;
                    candidate_session = session_it;
                    candidate_segment = segment_it;
                }
            }
        }

        if (candidate_session == resident_segments_.end()) {
            return true;
        }

        resident_bytes_ -= candidate_segment->second.bytes;
        candidate_session->second.erase(candidate_segment);
        if (candidate_session->second.empty()) {
            resident_segments_.erase(candidate_session);
        }
    }
    return true;
}

bool ResidentKVStore::refresh_entry_bytes(ResidentKVEntry & entry, std::string & error) {
    error.clear();
    entry.bytes = resident_snapshot_ram_bytes(entry.handle);
    return true;
}

void ResidentKVStore::touch(ResidentKVEntry & entry) {
    entry.last_access = ++access_clock_;
    if (access_clock_ == std::numeric_limits<uint64_t>::max()) {
        access_clock_ = 1;
    }
}

} // namespace attemory::context::kv
