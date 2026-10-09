# Integrate the official MaxCompute data-plane C++ SDK
# (github.com/aliyun/aliyun-odps-sdk-cpp) as a third-party dependency for the
# `odps` extension.
#
# This mirrors the repo's third-party conventions (see
# cmake/BuildZVecAsThirdParty.cmake):
#   * each dependency owns one `cmake/Build*AsThirdParty.cmake` file;
#   * the source lives under `third_party/<name>` as a git submodule and an
#     optional same-adjacent `<name>.patch` is applied idempotently;
#   * the dependency is built in an ISOLATED CMake project via
#     ExternalProject_Add and consumed through IMPORTED targets.
#
# Why ExternalProject_Add and NOT add_subdirectory (validated in Linux docker):
# the SDK's root CMakeLists.txt refers to ${CMAKE_SOURCE_DIR} throughout and
# writes generated headers (build_sdk_version, common/sdk_version.h) back into
# its own source tree. Embedded as a subdirectory those paths resolve to the
# NeuG root and configure fails. Building it as a stand-alone ExternalProject
# keeps ${CMAKE_SOURCE_DIR} pointing at the SDK. See plan.md C1.
#
# The SDK is only relevant to the `odps` extension. All SDK/Arrow symbols stay
# confined to the inner ABI=0 glue library and `libodps.neug_extension`, which
# is dlopen'd with RTLD_LOCAL, so libneug.so's core keeps zero Arrow / zero SDK
# dependencies. See plan.md C3 for the ABI seam.

include_guard(GLOBAL)
include(ExternalProject)

# Apply a patch idempotently: try git apply (submodule), else fall back to
# `patch -p1`. If neither applies forward, verify it is already applied.
function(_neug_odps_apply_patch source_dir patch_file patch_name)
    execute_process(
        COMMAND git rev-parse --show-toplevel
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _git_check
        OUTPUT_VARIABLE _git_root
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(REAL_PATH "${source_dir}" _source_real)
    if(_git_check EQUAL 0)
        file(REAL_PATH "${_git_root}" _git_real)
    endif()

    if(_git_check EQUAL 0 AND _git_real STREQUAL _source_real)
        set(_check git apply --check "${patch_file}")
        set(_apply git apply "${patch_file}")
        set(_reverse git apply --reverse --check "${patch_file}")
    else()
        find_program(_patch_exe patch REQUIRED)
        set(_check "${_patch_exe}" -p1 -f --dry-run -i "${patch_file}")
        set(_apply "${_patch_exe}" -p1 -f -i "${patch_file}")
        set(_reverse "${_patch_exe}" -p1 -f -R --dry-run -i "${patch_file}")
    endif()

    execute_process(COMMAND ${_check} WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _ok ERROR_VARIABLE _err)
    if(_ok EQUAL 0)
        execute_process(COMMAND ${_apply} WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE _applied ERROR_VARIABLE _err)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_name}: ${_err}")
        endif()
        message(STATUS "Applied ${patch_name}.")
        return()
    endif()

    execute_process(COMMAND ${_reverse} WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _already ERROR_QUIET)
    if(_already EQUAL 0)
        message(STATUS "${patch_name} is already applied.")
    else()
        message(FATAL_ERROR
            "${patch_name} neither applies nor appears already applied: ${_err}")
    endif()
endfunction()

function(build_odps_sdk_as_third_party)
    # Idempotence guard scoped to a SINGLE configure run — it must NOT be a
    # CACHE variable. The IMPORTED (neug::odps_sdk) and ExternalProject
    # (odps_sdk_external) targets created below live only in the in-memory CMake
    # model and are recreated on every configure; they are never persisted to
    # the cache. A CACHE-internal guard survives re-configuration, makes this
    # function return early, and the generate step then fails with "target
    # neug::odps_sdk ... was not found" because the consumers in
    # extension/odps/{,glue/}CMakeLists.txt still reference it. A GLOBAL
    # property is reset at the start of each cmake run, so it guards against
    # double-invocation within one configure without breaking incremental
    # re-configures (see plan.md C1).
    get_property(_neug_odps_sdk_done GLOBAL PROPERTY NEUG_BUILD_ODPS_SDK_DONE)
    if(_neug_odps_sdk_done)
        return()
    endif()

    # The SDK hard-codes GNU-only flags (--std=gnu++14, -march for the host
    # arch, x86 inline asm in util/crc32c.cpp) and only builds on Linux/GCC.
    # See plan.md C2. Fail loudly with guidance instead of a cryptic asm error.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "NEUG_WITH_ODPS_SDK=ON requires Linux (found '${CMAKE_SYSTEM_NAME}'): "
            "the aliyun-odps-sdk-cpp data plane only builds there. Configure with "
            "-DNEUG_WITH_ODPS_SDK=OFF on this platform (the odps extension then "
            "builds as a skeleton), or build on Linux x86_64.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "NEUG_WITH_ODPS_SDK=ON requires GCC (found '${CMAKE_CXX_COMPILER_ID}'): "
            "the aliyun-odps-sdk-cpp data plane pins GNU-only flags.")
    endif()

    # Locate the SDK source tree. Defaults to the vendored submodule but can be
    # pointed at an existing checkout via -DNEUG_ODPS_SDK_SOURCE_DIR=<path>.
    set(NEUG_ODPS_SDK_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/third_party/aliyun-odps-sdk-cpp"
        CACHE PATH "Source directory of aliyun-odps-sdk-cpp")
    if(NOT EXISTS "${NEUG_ODPS_SDK_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "aliyun-odps-sdk-cpp source not found at '${NEUG_ODPS_SDK_SOURCE_DIR}'. "
            "Run: git submodule update --init --recursive "
            "third_party/aliyun-odps-sdk-cpp (or -DNEUG_ODPS_SDK_SOURCE_DIR=<path>).")
    endif()

    # Optional patch (e.g. the crc32c.cpp arch guard that unblocks arm64 dev
    # builds; harmless on x86_64). Applied idempotently, carquet/zvec style.
    set(_odps_patch "${CMAKE_SOURCE_DIR}/third_party/odps-sdk.patch")
    if(EXISTS "${_odps_patch}")
        _neug_odps_apply_patch("${NEUG_ODPS_SDK_SOURCE_DIR}" "${_odps_patch}"
            "odps-sdk.patch")
    endif()

    # Parallelism: cap to physical cores (never unlimited -j on this codebase).
    if(APPLE)
        execute_process(COMMAND sysctl -n hw.physicalcpu
            OUTPUT_VARIABLE _odps_jobs OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        cmake_host_system_information(RESULT _odps_jobs
            QUERY NUMBER_OF_PHYSICAL_CORES)
    endif()
    if(NOT _odps_jobs OR _odps_jobs LESS 1)
        set(_odps_jobs 1)
    endif()

    set(_odps_binary_dir
        "${CMAKE_BINARY_DIR}/third_party/aliyun-odps-sdk-cpp-build")
    set(_odps_lib_dir "${_odps_binary_dir}/lib")
    # The SDK's own OdpsDeps.cmake installs fetched deps under
    # ${CMAKE_BINARY_DIR}/deps_install (its binary dir == _odps_binary_dir).
    set(_odps_deps_prefix "${_odps_binary_dir}/deps_install")

    # Static libs produced by the SDK module CMakeLists (OUTPUT_NAMEs).
    set(_odps_common_lib "${_odps_lib_dir}/libodps_sdk_common_static.a")
    set(_odps_core_lib "${_odps_lib_dir}/libodps_sdk_core_static.a")
    set(_odps_tunnel_lib "${_odps_lib_dir}/libodps_sdk_tunnel_static.a")
    set(_odps_msa_lib "${_odps_lib_dir}/libmax_storage_api.a")

    ExternalProject_Add(odps_sdk_external
        SOURCE_DIR "${NEUG_ODPS_SDK_SOURCE_DIR}"
        BINARY_DIR "${_odps_binary_dir}"
        INSTALL_COMMAND ""
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE=Release"
            "-DWITH_ARROW=ON"
            "-DODPS_BUILD_TESTS=OFF"
            "-DODPS_BUILD_EXAMPLES=OFF"
            "-DWITH_PERF_TOOL=OFF"
            "-DODPS_WERROR=OFF"
        BUILD_COMMAND
            "${CMAKE_COMMAND}" --build <BINARY_DIR>
            --target odps_sdk_common_static odps_sdk_core_static
                     odps_sdk_tunnel_static max_storage_api_static
            --parallel ${_odps_jobs})

    # Public include roots: SDK headers are referenced as "xxx.h" (from
    # include/), "common/xxx.h" / "max_storage_api/xxx.h" (from the root) and
    # vendored <nlohmann/json.hpp> etc. (from thirdparty/), plus the fetched
    # deps' headers (Arrow 1.0.0, protobuf, ...).
    set(_odps_include_dirs
        "${NEUG_ODPS_SDK_SOURCE_DIR}"
        "${NEUG_ODPS_SDK_SOURCE_DIR}/include"
        "${NEUG_ODPS_SDK_SOURCE_DIR}/thirdparty"
        "${_odps_deps_prefix}/include")

    # CMake validates an IMPORTED target's INTERFACE_INCLUDE_DIRECTORIES at
    # generate time, but the fetched deps' headers only appear once the
    # ExternalProject builds. Pre-create the generated dirs (empty for now) so
    # configure succeeds; the build populates them before anything includes them.
    file(MAKE_DIRECTORY "${_odps_deps_prefix}/include")
    file(MAKE_DIRECTORY "${_odps_deps_prefix}/lib")

    # The SDK's fetched third-party deps are SHARED-only (no static .a for
    # Arrow/protobuf) and live under deps_install/lib. They MUST be referenced
    # by FULL PATH, never by bare name: INTERFACE_LINK_DIRECTORIES does not
    # survive propagation across the glue's link interface into the outer
    # extension .so, so a bare `-larrow`/`-lprotobuf` there binds to the SYSTEM
    # libs (Arrow 22 / protobuf 3.21, both ABI=1) instead of the SDK's ABI=0
    # Arrow 1.0.0 / protobuf 3.7.1, leaving the extension .so with undefined
    # `Ss`/`EPSs` (old-ABI std::string) symbols that fail at dlopen. Full paths
    # pin the exact SDK copies regardless of -L propagation.
    set(_odps_deps_libdir "${_odps_deps_prefix}/lib")
    set(_odps_arrow_lib "${_odps_deps_libdir}/libarrow.so")
    set(_odps_protobuf_lib "${_odps_deps_libdir}/libprotobuf.so")
    set(_odps_protobuf_lite_lib "${_odps_deps_libdir}/libprotobuf-lite.so")

    # IMPORTED aggregate target carrying the full link interface: the four SDK
    # static libs plus the SDK's shared third-party deps. The SDK libs have
    # circular references, so they are wrapped in --start-group/--end-group.
    add_library(neug::odps_sdk INTERFACE IMPORTED GLOBAL)
    set_target_properties(neug::odps_sdk PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_odps_include_dirs}"
        INTERFACE_LINK_DIRECTORIES "${_odps_deps_libdir}"
        INTERFACE_LINK_LIBRARIES
        "-Wl,--start-group;${_odps_msa_lib};${_odps_tunnel_lib};${_odps_core_lib};${_odps_common_lib};-Wl,--end-group;${_odps_arrow_lib};${_odps_protobuf_lib};${_odps_protobuf_lite_lib};curl;ssl;crypto;zstd;lz4;z;pthread;dl;rt")
    add_dependencies(neug::odps_sdk odps_sdk_external)

    # Exported for the inner glue library's CMakeLists.
    set(NEUG_ODPS_SDK_INCLUDE_DIRS "${_odps_include_dirs}"
        CACHE INTERNAL "Include dirs for aliyun-odps-sdk-cpp")
    set(NEUG_ODPS_SDK_EXTERNAL_TARGET "odps_sdk_external"
        CACHE INTERNAL "ExternalProject target that builds the ODPS SDK")
    set(NEUG_ODPS_SDK_DEPS_LIB_DIR "${_odps_deps_libdir}"
        CACHE INTERNAL "Directory holding the ODPS SDK's shared third-party deps (Arrow 1.0.0, protobuf 3.7.1) to bundle next to the glue .so")
    set_property(GLOBAL PROPERTY NEUG_BUILD_ODPS_SDK_DONE TRUE)

    message(STATUS
        "Integrating aliyun-odps-sdk-cpp from ${NEUG_ODPS_SDK_SOURCE_DIR} "
        "via ExternalProject (WITH_ARROW=ON); IMPORTED target neug::odps_sdk")
endfunction()
