#pragma once

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace attemory::test {

inline int failures = 0;

inline void fail(const char * expr, const char * file, int line) {
    std::cerr << file << ":" << line << ": expectation failed: " << expr << "\n";
    ++failures;
}

inline void fail_equal(
    const char * actual,
    const char * expected,
    const char * file,
    int line) {
    std::cerr << file << ":" << line
              << ": expectation failed: " << actual << " == " << expected << "\n";
    ++failures;
}

inline void fail_contains(
    const char * actual,
    const char * expected,
    const char * file,
    int line) {
    std::cerr << file << ":" << line
              << ": expectation failed: " << actual << " contains " << expected << "\n";
    ++failures;
}

inline bool contains(const std::string & value, const std::string & needle) {
    return value.find(needle) != std::string::npos;
}

inline int test_main_result(const char * suite_name) {
    if (failures != 0) {
        std::cerr << failures << " " << suite_name << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}

class TempDir {
public:
    explicit TempDir(const std::string & name) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                (name + "-" + std::to_string(static_cast<long long>(now)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    TempDir(const TempDir &) = delete;
    TempDir & operator=(const TempDir &) = delete;

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path & path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace attemory::test

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            ::attemory::test::fail(#expr, __FILE__, __LINE__); \
        } \
    } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(actual, expected) \
    do { \
        const auto actual_value = (actual); \
        const auto expected_value = (expected); \
        if (!(actual_value == expected_value)) { \
            ::attemory::test::fail_equal(#actual, #expected, __FILE__, __LINE__); \
        } \
    } while (false)

#define EXPECT_CONTAINS(actual, expected) \
    do { \
        const std::string actual_value = (actual); \
        const std::string expected_value = (expected); \
        if (!::attemory::test::contains(actual_value, expected_value)) { \
            ::attemory::test::fail_contains(#actual, #expected, __FILE__, __LINE__); \
        } \
    } while (false)
