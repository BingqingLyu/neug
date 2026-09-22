# Integrate the official MaxCompute data-plane C++ SDK
# (github.com/aliyun/aliyun-odps-sdk-cpp) as a third-party dependency for the
# `odps` extension.
#
# This mirrors the repo's third-party conventions:
#   * each dependency owns one `cmake/Build*AsThirdParty.cmake` file;
#   * the source lives under `third_party/<name>` as a git submodule and an
#     optional same-adjacent `<name>.patch` is applied idempotently (carquet /
#     cppjieba style);
#   * the target is added with EXCLUDE_FROM_ALL so it is only built when a
#     consumer (the odps extension) actually links it.
#
# The SDK is only relevant to the `odps` extension. All SDK/Arrow symbols stay
# confined to `libodps.neug_extension`, which is dlopen'd with RTLD_LOCAL, so
# libneug.so's core keeps zero Arrow / zero SDK dependencies.

if(DEFINED NEUG_BUILD_ODPS_SDK_DONE AND NEUG_BUILD_ODPS_SDK_DONE)
    return()
endif()

# Locate the SDK source tree. Defaults to the vendored submodule but can be
# pointed at an existing checkout (e.g. a sibling clone) via
# -DNEUG_ODPS_SDK_SOURCE_DIR=<path> during local development.
set(NEUG_ODPS_SDK_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/aliyun-odps-sdk-cpp"
    CACHE PATH "Source directory of aliyun-odps-sdk-cpp")

if(NOT EXISTS "${NEUG_ODPS_SDK_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "aliyun-odps-sdk-cpp source not found at '${NEUG_ODPS_SDK_SOURCE_DIR}'. "
        "Run: git submodule update --init --recursive third_party/aliyun-odps-sdk-cpp "
        "(or configure -DNEUG_ODPS_SDK_SOURCE_DIR=<path> to an existing checkout).")
endif()

# Apply an optional patch idempotently (git apply --check, then apply, else
# verify it is already applied in reverse). No patch is required for the SDK at
# the moment; the hook exists so build-local fixes follow the carquet pattern.
set(_odps_sdk_patch "${CMAKE_SOURCE_DIR}/third_party/odps-sdk.patch")
if(EXISTS "${_odps_sdk_patch}")
    execute_process(
        COMMAND git apply --check "${_odps_sdk_patch}"
        WORKING_DIRECTORY "${NEUG_ODPS_SDK_SOURCE_DIR}"
        RESULT_VARIABLE _patch_check OUTPUT_QUIET ERROR_QUIET)
    if(_patch_check EQUAL 0)
        execute_process(
            COMMAND git apply "${_odps_sdk_patch}"
            WORKING_DIRECTORY "${NEUG_ODPS_SDK_SOURCE_DIR}"
            RESULT_VARIABLE _patch_result)
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply odps-sdk.patch")
        endif()
    else()
        execute_process(
            COMMAND git apply --reverse --check "${_odps_sdk_patch}"
            WORKING_DIRECTORY "${NEUG_ODPS_SDK_SOURCE_DIR}"
            RESULT_VARIABLE _patch_applied OUTPUT_QUIET ERROR_QUIET)
        if(NOT _patch_applied EQUAL 0)
            message(FATAL_ERROR "odps-sdk.patch does not apply cleanly")
        endif()
    endif()
endif()

# Configure the SDK for embedding: enable Arrow (required by the max_storage_api
# read stream which returns arrow::RecordBatch) and disable everything the
# extension does not need (tests, examples, perf tool, upstream -Werror).
set(WITH_ARROW ON CACHE BOOL "" FORCE)
set(ODPS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ODPS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(ODPS_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory("${NEUG_ODPS_SDK_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/third_party/aliyun-odps-sdk-cpp" EXCLUDE_FROM_ALL)

# Stable alias targets so the extension links intent-first on the static
# variants (the SDK also produces SHARED copies we do not want to distribute).
# Concrete target names in aliyun-odps-sdk-cpp:
#   odps_sdk_core_static / odps_sdk_tunnel_static / max_storage_api_static
foreach(_pair "core:odps_sdk_core_static"
        "tunnel:odps_sdk_tunnel_static"
        "max_storage_api:max_storage_api_static")
    string(REPLACE ":" ";" _kv "${_pair}")
    list(GET _kv 0 _alias)
    list(GET _kv 1 _target)
    if(TARGET ${_target} AND NOT TARGET neug::odps_${_alias})
        add_library(neug::odps_${_alias} ALIAS ${_target})
    endif()
endforeach()

# Public include roots: headers are referenced as "include/xxx.h" and
# "max_storage_api/xxx.h" / "common/xxx.h" relative to the SDK root.
set(NEUG_ODPS_SDK_INCLUDE_DIRS
    "${NEUG_ODPS_SDK_SOURCE_DIR}"
    "${NEUG_ODPS_SDK_SOURCE_DIR}/include"
    CACHE INTERNAL "Include directories for aliyun-odps-sdk-cpp")

set(NEUG_BUILD_ODPS_SDK_DONE TRUE CACHE INTERNAL "")
message(STATUS "Integrated aliyun-odps-sdk-cpp from ${NEUG_ODPS_SDK_SOURCE_DIR} (WITH_ARROW=ON)")
