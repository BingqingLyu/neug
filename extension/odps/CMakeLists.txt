# NeuG `odps` extension: read MaxCompute (ODPS) tables as an input data source
# via the official data-plane C++ SDK (aliyun-odps-sdk-cpp, Storage API).
#
# This mirrors extension/parquet and extension/fts. All SDK/Arrow dependencies
# are confined to this extension, which is dlopen'd with RTLD_LOCAL, so the
# libneug.so core keeps zero Arrow / zero SDK dependencies.

# Collect source files.
file(GLOB ODPS_EXTENSION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")
set(ODPS_EXTENSION_OBJECT_FILES ${ODPS_EXTENSION_SOURCES})

# The Storage API reader (module 1, tasks T105/T106) needs the ODPS C++ SDK and
# its Arrow support. Until then the extension builds and loads as a skeleton
# (ODPS_SCAN registered, callbacks fail loudly), so the SDK link is opt-in and
# OFF by default to keep the extension buildable without the vendored SDK.
option(NEUG_WITH_ODPS_SDK
    "Link aliyun-odps-sdk-cpp into the odps extension (enable with the Storage API reader)"
    OFF)

# Use the extension build system. Must be called after setting
# ODPS_EXTENSION_OBJECT_FILES.
build_extension_lib("odps")

target_include_directories(neug_odps_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# neug (shared library) is linked last so the extension resolves core symbols
# against libneug while keeping its own static dependencies self-contained.
# Protobuf follows neug: the schema converter (T105) builds `::common::DataType`
# protobuf messages (mutable_string()->mutable_var_char()), whose Arena
# allocator lives in the protobuf runtime that libneug does not re-export under
# RTLD_LOCAL. Mirrors extension/parquet.
target_link_libraries(neug_odps_extension PRIVATE neug ${Protobuf_LIBRARIES})

if(NEUG_WITH_ODPS_SDK)
    # Builds the SDK as an isolated ExternalProject (it is not
    # subdirectory-safe) and defines the IMPORTED aggregate target
    # neug::odps_sdk plus NEUG_ODPS_SDK_INCLUDE_DIRS / _EXTERNAL_TARGET.
    # See plan.md C1.
    include(${CMAKE_SOURCE_DIR}/cmake/BuildOdpsSdkAsThirdParty.cmake)
    build_odps_sdk_as_third_party()

    # Inner ABI=0 glue: the only TU that sees SDK/Arrow headers, built with
    # -D_GLIBCXX_USE_CXX11_ABI=0 to match the SDK. The outer extension stays on
    # NeuG's ABI=1 / C++20 and talks to it through the pure C seam. See plan.md C3.
    add_subdirectory(glue)

    # The outer extension includes only the SDK-free glue header and links the
    # glue SHARED lib. Because the glue is a .so that seals the SDK's ABI=0
    # Arrow/protobuf behind -fvisibility=hidden + --exclude-libs,ALL, this .so
    # depends only on libodps_sdk_glue.so and its odps_glue_* C seam; no SDK or
    # Arrow symbol is pulled into the ABI=1 extension or leaks into the process.
    target_include_directories(neug_odps_extension PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/glue)
    target_link_libraries(neug_odps_extension PRIVATE odps_sdk_glue)
    target_compile_definitions(neug_odps_extension PRIVATE ODPS_SDK_ENABLE_ARROW)

    # Bug #8: the SDK-ON extension statically links NeuG's protobuf (to build
    # ::common::DataType schema messages) and, per build_extension_lib, does NOT
    # hide those exports. It therefore publishes ~586 google::protobuf symbols
    # (incl. MessageLite::ParseFromArray). At `LOAD odps;` time the loader
    # dlopen's this .so into a group that already contains the SDK's ABI=0
    # libprotobuf.so.3.7.1.0 (via the glue); the SDK protobuf's static-init
    # AddDescriptors -> ParseFromArray then binds to THIS extension's ABI=1 copy
    # instead of its own, and the cross-ABI call segfaults before any query.
    # Hide the extension's protobuf/arrow/absl exports (neug_exports.ld keeps
    # `Init` and all UNDEF imports global) so each protobuf runtime binds to
    # itself. Verified safe here: the extension has ZERO UNDEF symbols matching
    # any local: pattern (its arrow/SDK deps are sealed inside the glue .so, and
    # the only UNDEF protobuf-adjacent refs are common::* -> _ZN6common...,
    # which the script does not touch).
    neug_apply_symbol_visibility(neug_odps_extension)
endif()

# Unit tests for the pure-logic modules (T103 odps_options, T104
# odps_connection credential resolution). No SDK/network required, so the
# default (NEUG_WITH_ODPS_SDK=OFF) skeleton build runs them green.
if(BUILD_TEST)
    if(NEUG_WITH_ODPS_SDK)
        # add_extension_test compiles the extension src/*.cc directly into the
        # test exe, so under SDK-ON it needs the same glue header, glue lib and
        # ODPS_SDK_ENABLE_ARROW define as the extension .so.
        add_extension_test(
            NAME odps
            TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/tests/odps_test.cc"
            EXTRA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/glue"
            EXTRA_LINK_LIBS odps_sdk_glue)
        target_compile_definitions(odps_extension_test PRIVATE
            ODPS_SDK_ENABLE_ARROW)
        # The test exe links libneug (NeuG protobuf, ABI=1) AND loads the glue
        # .so, which pulls the SDK's libprotobuf.so.3.7.1.0 (ABI=0) into the same
        # process. A normal executable exports its statically-linked protobuf
        # symbols into the GLOBAL scope, where they interpose the SDK protobuf's
        # internal calls (ParseFromArray -> GetTypeName) and segfault during
        # library init. Hide this exe's arrow/protobuf/absl symbols exactly like
        # libneug.so does (neug_exports.ld) so each protobuf runtime binds to
        # itself. The shipped extension avoids this because it is dlopen'd with
        # RTLD_LOCAL, but the test links the glue as a normal global dependency.
        neug_apply_symbol_visibility(odps_extension_test)
    else()
        add_extension_test(
            NAME odps
            TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/tests/odps_test.cc")
    endif()
    add_dependencies(odps_extension_test neug_odps_extension)
endif()
