#include "persistent/binary_io.h"

#include <limits>

namespace attemory::persistent::binary_io {
namespace {

constexpr uint64_t kMaxStringBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxVectorItems = 16ull * 1024ull * 1024ull;

} // namespace

bool write_bytes(std::ostream & out, const void * data, size_t size) {
    if (size == 0) {
        return true;
    }
    if (data == nullptr || size > (size_t) std::numeric_limits<std::streamsize>::max()) {
        return false;
    }

    out.write(reinterpret_cast<const char *>(data), (std::streamsize) size);
    return out.good();
}

bool read_bytes(std::istream & in, void * data, size_t size) {
    if (size == 0) {
        return true;
    }
    if (data == nullptr || size > (size_t) std::numeric_limits<std::streamsize>::max()) {
        return false;
    }

    in.read(reinterpret_cast<char *>(data), (std::streamsize) size);
    return in.good();
}

bool write_bool(std::ostream & out, bool value) {
    const uint8_t raw = value ? 1 : 0;
    return write_pod(out, raw);
}

bool read_bool(std::istream & in, bool & value) {
    uint8_t raw = 0;
    if (!read_pod(in, raw)) {
        return false;
    }
    value = raw != 0;
    return true;
}

bool write_string(std::ostream & out, const std::string & value) {
    const uint64_t size = (uint64_t) value.size();
    return write_pod(out, size) &&
           write_bytes(out, value.data(), (size_t) size);
}

bool read_string(std::istream & in, std::string & value) {
    uint64_t size = 0;
    if (!read_pod(in, size)) {
        return false;
    }
    if (size > kMaxStringBytes ||
        size > (uint64_t) std::numeric_limits<size_t>::max()) {
        return false;
    }

    value.resize((size_t) size);
    return read_bytes(in, value.data(), value.size());
}

bool write_i64_vector(std::ostream & out, const std::vector<int64_t> & values) {
    const uint64_t size = (uint64_t) values.size();
    if (!write_pod(out, size)) {
        return false;
    }
    for (int64_t value : values) {
        if (!write_pod(out, value)) {
            return false;
        }
    }
    return true;
}

bool read_i64_vector(std::istream & in, std::vector<int64_t> & values) {
    uint64_t size = 0;
    if (!read_pod(in, size)) {
        return false;
    }
    if (size > kMaxVectorItems) {
        return false;
    }

    values.resize((size_t) size);
    for (int64_t & value : values) {
        if (!read_pod(in, value)) {
            return false;
        }
    }
    return true;
}

} // namespace attemory::persistent::binary_io
