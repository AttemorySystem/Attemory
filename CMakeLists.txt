cmake_minimum_required(VERSION 3.18)

project(attemory LANGUAGES C CXX)

enable_testing()
option(ATTEMORY_BUILD_TESTS "Build attemory C++ tests" ON)
option(ATTEMORY_BUILD_MODEL_TESTS "Build C++ tests that require a model at runtime" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
    message(STATUS "attemory Linux libstdc++ C++11 ABI: 0")
endif()

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

set(ATTEMORY_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if (NOT EXISTS "${ATTEMORY_VERSION_FILE}")
    message(FATAL_ERROR "Missing attemory version file: ${ATTEMORY_VERSION_FILE}")
endif()
file(STRINGS "${ATTEMORY_VERSION_FILE}" ATTEMORY_VERSION LIMIT_COUNT 1)
string(STRIP "${ATTEMORY_VERSION}" ATTEMORY_VERSION)
if (NOT ATTEMORY_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([a-zA-Z0-9.+_-]*)?$")
    message(FATAL_ERROR "Invalid attemory version in ${ATTEMORY_VERSION_FILE}: ${ATTEMORY_VERSION}")
endif()
message(STATUS "attemory version: ${ATTEMORY_VERSION}")

set(ATMCORE_SDK "" CACHE PATH "attemory-core SDK root")
set(ATTEMORY_REQUIRED_ATMCORE_API_VERSION 1)

if (NOT ATMCORE_SDK)
    message(FATAL_ERROR "Set ATMCORE_SDK=/path/to/attemory-core-sdk")
endif()

get_filename_component(ATMCORE_SDK "${ATMCORE_SDK}" ABSOLUTE)
set(ATMCORE_INCLUDE_DIR "${ATMCORE_SDK}/include")
set(ATMCORE_LIBRARY_DIR "${ATMCORE_SDK}/lib")
set(ATMCORE_RUNTIME_DIR "${ATMCORE_SDK}/lib;${ATMCORE_SDK}/bin")
set(ATTEMORY_EXTRA_RUNTIME_DIRS "" CACHE STRING "Additional native runtime library directories required by the attemory-core SDK")
option(ATTEMORY_ALLOW_SDK_SHLIB_UNDEFINED "Allow unresolved symbols in shared libraries pulled in by the attemory-core SDK" OFF)
set(ATTEMORY_NATIVE_RUNTIME_DIRS ${ATMCORE_RUNTIME_DIR})
if (ATTEMORY_EXTRA_RUNTIME_DIRS)
    list(APPEND ATTEMORY_NATIVE_RUNTIME_DIRS ${ATTEMORY_EXTRA_RUNTIME_DIRS})
endif()
list(REMOVE_DUPLICATES ATTEMORY_NATIVE_RUNTIME_DIRS)

if (NOT EXISTS "${ATMCORE_INCLUDE_DIR}/attemory-core/attemory-core.h")
    message(FATAL_ERROR "Missing attemory-core SDK header: ${ATMCORE_INCLUDE_DIR}/attemory-core/attemory-core.h")
endif()
if (NOT IS_DIRECTORY "${ATMCORE_LIBRARY_DIR}")
    message(FATAL_ERROR "Missing attemory-core SDK library directory: ${ATMCORE_LIBRARY_DIR}")
endif()

set(ATMCORE_VERSION_HEADER "${ATMCORE_INCLUDE_DIR}/attemory-core/version.h")
if (NOT EXISTS "${ATMCORE_VERSION_HEADER}")
    message(FATAL_ERROR "Missing attemory-core SDK version header: ${ATMCORE_VERSION_HEADER}")
endif()

file(STRINGS "${ATMCORE_VERSION_HEADER}" ATMCORE_API_VERSION_LINE REGEX "^#define +ATMCORE_API_VERSION +[0-9]+")
if (NOT ATMCORE_API_VERSION_LINE)
    message(FATAL_ERROR "${ATMCORE_VERSION_HEADER} does not define ATMCORE_API_VERSION")
endif()
string(REGEX REPLACE "^#define +ATMCORE_API_VERSION +([0-9]+).*" "\\1" ATMCORE_API_VERSION "${ATMCORE_API_VERSION_LINE}")
if (ATMCORE_API_VERSION LESS ATTEMORY_REQUIRED_ATMCORE_API_VERSION)
    message(FATAL_ERROR "attemory-core API version ${ATMCORE_API_VERSION} is too old; attemory requires >= ${ATTEMORY_REQUIRED_ATMCORE_API_VERSION}")
endif()

file(STRINGS "${ATMCORE_VERSION_HEADER}" ATMCORE_VERSION_STRING_LINE REGEX "^#define +ATMCORE_VERSION_STRING +\"[^\"]+\"")
if (ATMCORE_VERSION_STRING_LINE)
    string(REGEX REPLACE "^#define +ATMCORE_VERSION_STRING +\"([^\"]+)\".*" "\\1" ATMCORE_VERSION_STRING "${ATMCORE_VERSION_STRING_LINE}")
else()
    set(ATMCORE_VERSION_STRING "unknown")
endif()
message(STATUS "attemory-core version: ${ATMCORE_VERSION_STRING}; API version: ${ATMCORE_API_VERSION}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/attemory/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/attemory/version.h"
    @ONLY
)

set(ATMCORE_LIBRARY "${ATMCORE_LIBRARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}attemory-core${CMAKE_SHARED_LIBRARY_SUFFIX}")
if (NOT EXISTS "${ATMCORE_LIBRARY}")
    message(FATAL_ERROR "Missing required attemory-core library: ${ATMCORE_LIBRARY}")
endif()
message(STATUS "attemory-core library: ${ATMCORE_LIBRARY}")

add_library(attemory-core SHARED IMPORTED GLOBAL)
set_target_properties(attemory-core PROPERTIES
    IMPORTED_LOCATION ${ATMCORE_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES "${ATMCORE_INCLUDE_DIR}"
)

function(attemory_configure_native_executable target)
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${ATTEMORY_NATIVE_RUNTIME_DIRS}"
    )
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach (runtime_dir IN LISTS ATTEMORY_NATIVE_RUNTIME_DIRS)
            if (runtime_dir)
                target_link_options(${target} PRIVATE "LINKER:-rpath-link,${runtime_dir}")
            endif()
        endforeach()
        if (ATTEMORY_ALLOW_SDK_SHLIB_UNDEFINED)
            target_link_options(${target} PRIVATE "LINKER:--allow-shlib-undefined")
        endif()
    endif()
endfunction()

add_library(attemory_httplib INTERFACE)
target_include_directories(attemory_httplib SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/cpp-httplib
)
target_compile_definitions(attemory_httplib INTERFACE
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576
    CPPHTTPLIB_LISTEN_BACKLOG=512
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=32768
    CPPHTTPLIB_TCP_NODELAY=1
)
target_link_libraries(attemory_httplib INTERFACE Threads::Threads)

add_library(attemory_nlohmann_json INTERFACE)
target_include_directories(attemory_nlohmann_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/json
)

add_library(attemory_server_3rd INTERFACE)
target_link_libraries(attemory_server_3rd INTERFACE
    attemory_httplib
    attemory_nlohmann_json
)

set(ATTEMORY_PERSISTENT_SOURCES
    src/persistent/binary_io.cpp
    src/persistent/kv_cache_persistence.cpp
    src/persistent/kv_cache_state.cpp
    src/persistent/persistent.cpp
    src/persistent/session_persistence.cpp
    src/persistent/storage_paths.cpp
)

add_library(attemory_persistent STATIC ${ATTEMORY_PERSISTENT_SOURCES})
target_include_directories(attemory_persistent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ATMCORE_INCLUDE_DIR}
)
target_link_libraries(attemory_persistent PUBLIC attemory-core)

set(ATTEMORY_CONTEXT_SOURCES
    src/context/kv/segment_kv_cache_commands.cpp
    src/context/kv/segment_kv_metadata.cpp
    src/context/command/command_result.cpp
    src/context/command/command_scope.cpp
    src/context/context.cpp
    src/context/kv/resident_kv_store.cpp
    src/context/kv/segment_kv_manager.cpp
    src/context/search/search_results.cpp
    src/context/search/search_runner.cpp
    src/context/search/search_service.cpp
    src/context/session/segment_planner.cpp
    src/context/session/session_lookup.cpp
    src/context/session/session_manager.cpp
    src/context/session/session_summary.cpp
    src/context/session/session_status.cpp
    src/context/storage/model_cache_key.cpp
    src/context/storage/storage_layout.cpp
)

add_library(attemory_context STATIC ${ATTEMORY_CONTEXT_SOURCES})
target_include_directories(attemory_context PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ATMCORE_INCLUDE_DIR}
)
target_link_libraries(attemory_context PUBLIC
    attemory-core
    attemory_persistent
)

add_library(attemory_server_http STATIC
    src/server/http_common.cpp
    src/server/request_parser.cpp
    src/server/response_builder.cpp
    src/server/routes.cpp
)
target_include_directories(attemory_server_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ATMCORE_INCLUDE_DIR}
)
target_link_libraries(attemory_server_http PUBLIC
    attemory_context
    attemory_server_3rd
)

add_executable(attemory_server
    src/server/server.cpp
    src/server/server_args.cpp
)
target_include_directories(attemory_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${ATMCORE_INCLUDE_DIR}
)
target_link_libraries(attemory_server PRIVATE
    attemory_server_http
)
attemory_configure_native_executable(attemory_server)
set_target_properties(attemory_server PROPERTIES
    INSTALL_RPATH "$ORIGIN;${ATMCORE_RUNTIME_DIR};${ATMCORE_LIBRARY_DIR}"
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(attemory_server PRIVATE "LINKER:--as-needed")
endif()

function(attemory_add_cpp_test target)
    set(options)
    set(one_value_args)
    set(multi_value_args SOURCES LIBS LABELS)
    cmake_parse_arguments(TEST "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if (NOT TEST_SOURCES)
        message(FATAL_ERROR "attemory_add_cpp_test(${target}) requires SOURCES")
    endif()

    add_executable(${target} ${TEST_SOURCES})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ATMCORE_INCLUDE_DIR}
    )
    if (TEST_LIBS)
        target_link_libraries(${target} PRIVATE ${TEST_LIBS})
    endif()
    attemory_configure_native_executable(${target})
    add_test(NAME ${target} COMMAND ${target})
    if (TEST_LABELS)
        set_tests_properties(${target} PROPERTIES LABELS "${TEST_LABELS}")
    endif()
endfunction()

if (ATTEMORY_BUILD_TESTS)
    attemory_add_cpp_test(attemory_server_request_parser_test
        SOURCES tests/server_request_parser_test.cpp
        LIBS attemory_server_http
        LABELS unit
    )
    attemory_add_cpp_test(attemory_server_response_builder_test
        SOURCES tests/server_response_builder_test.cpp
        LIBS attemory_server_http
        LABELS unit
    )
    attemory_add_cpp_test(attemory_search_results_test
        SOURCES tests/search_results_test.cpp
        LIBS attemory_context
        LABELS unit
    )
    attemory_add_cpp_test(attemory_persistent_session_test
        SOURCES tests/persistent_session_test.cpp
        LIBS attemory_persistent
        LABELS unit
    )
    attemory_add_cpp_test(attemory_http_contract_test
        SOURCES tests/http_contract_test.cpp
        LIBS attemory_server_http
        LABELS integration
    )

    if (ATTEMORY_BUILD_MODEL_TESTS)
        attemory_add_cpp_test(attemory_incremental_index_test
            SOURCES tests/incremental_index_test.cpp
            LIBS attemory_context
            LABELS model
        )
        set_tests_properties(attemory_incremental_index_test PROPERTIES
            SKIP_REGULAR_EXPRESSION "^skip:"
        )
    endif()
endif()
