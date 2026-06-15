#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace attemory::persistent::binary_io {

bool write_bytes(std::ostream & out, const void * data, size_t size);
bool read_bytes(std::istream & in, void * data, size_t size);

template <typename T>
bool write_pod(std::ostream & out, const T & value) {
    return write_bytes(out, &value, sizeof(value));
}

template <typename T>
bool read_pod(std::istream & in, T & value) {
    return read_bytes(in, &value, sizeof(value));
}

bool write_bool(std::ostream & out, bool value);
bool read_bool(std::istream & in, bool & value);

bool write_string(std::ostream & out, const std::string & value);
bool read_string(std::istream & in, std::string & value);

bool write_i64_vector(std::ostream & out, const std::vector<int64_t> & values);
bool read_i64_vector(std::istream & in, std::vector<int64_t> & values);

} // namespace attemory::persistent::binary_io
