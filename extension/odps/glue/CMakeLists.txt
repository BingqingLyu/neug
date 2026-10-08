# Inner ABI=0 glue library for the `odps` extension.
#
# This is the ONLY target in NeuG that compiles against the ODPS SDK / Arrow
# headers. It is built with -D_GLIBCXX_USE_CXX11_ABI=0 and gnu++14 to match the
# SDK (which hard-codes that ABI for GCC>=5), while the outer
# `libodps.neug_extension` stays on NeuG's default ABI=1 / C++20. The two sides
# meet only at the pure C seam declared in odps_sdk_glue.h, so no std::string or
# other STL object ever crosses the ABI boundary. See plan.md C3.
#
# Only added when NEUG_WITH_ODPS_SDK=ON (the parent CMakeLists guards this), and
# build_odps_sdk_as_third_party() must have run first so that neug::odps_sdk and
# NEUG_ODPS_SDK_* are defined.

add_library(odps_sdk_glue STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/odps_sdk_glue.cc")

# Match the SDK exactly: gnu++14 + the pre-C++11 std::string ABI. These flags
# are PRIVATE to this TU — they must never leak into the ABI=1 outer extension.
set_target_properties(odps_sdk_glue PROPERTIES
    CXX_STANDARD 14
    CXX_EXTENSIONS ON
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(odps_sdk_glue PRIVATE _GLIBCXX_USE_CXX11_ABI=0)

# SDK/Arrow headers are PRIVATE: the outer extension includes only the SDK-free
# odps_sdk_glue.h (its own dir is added by the parent CMakeLists).
target_include_directories(odps_sdk_glue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${NEUG_ODPS_SDK_INCLUDE_DIRS})

# neug::odps_sdk is an INTERFACE IMPORTED target carrying the four SDK static
# libs (--start-group wrapped) plus the SDK's shared third-party deps. Linked
# PRIVATE: for a static library CMake propagates these as link-only to the final
# consumer (the outer .so) without exposing the SDK include dirs/defines.
target_link_libraries(odps_sdk_glue PRIVATE neug::odps_sdk)

# glue.cc transitively includes SDK headers that the SDK build generates back
# into its source tree (common/sdk_version.h), so compile only after the
# ExternalProject has built.
add_dependencies(odps_sdk_glue ${NEUG_ODPS_SDK_EXTERNAL_TARGET})
