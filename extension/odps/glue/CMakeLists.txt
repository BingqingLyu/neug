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

add_library(odps_sdk_glue SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/odps_sdk_glue.cc")

# Match the SDK exactly: gnu++14 + the pre-C++11 std::string ABI. These flags
# are PRIVATE to this TU — they must never leak into the ABI=1 outer extension.
# The glue is a SHARED library (not STATIC) so the SDK's ABI=0 Arrow 1.0.0 /
# protobuf 3.7.1 stay sealed inside it: the outer extension links only this .so
# and sees just the odps_glue_* C seam, never an SDK/Arrow symbol. A static glue
# would instead forward the raw SDK archives into the ABI=1 extension .so, where
# the bare deps resolve against the system Arrow/protobuf and leave undefined
# old-ABI symbols (the bug this design closes). See plan.md C3.
set_target_properties(odps_sdk_glue PROPERTIES
    CXX_STANDARD 14
    CXX_EXTENSIONS ON
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    # Co-locate with libodps.neug_extension so its $ORIGIN RUNPATH finds this
    # .so, and this .so's own $ORIGIN RUNPATH finds the bundled SDK deps below.
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:neug_odps_extension>"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
target_compile_definitions(odps_sdk_glue PRIVATE _GLIBCXX_USE_CXX11_ABI=0)

# Hide everything except the explicitly-default-visible odps_glue_* seam:
#  -fvisibility=hidden    hides this TU's own non-seam symbols;
#  --exclude-libs,ALL     hides all symbols pulled from the static SDK archives.
# The SDK's Arrow/protobuf are SHARED (NEEDED) deps, so their imports stay
# dynamic and are resolved at load from the bundled .so — a `local: *` version
# script would wrongly bind those UNDEF entries locally (see cmake/neug_exports.ld).
target_compile_options(odps_sdk_glue PRIVATE
    -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(odps_sdk_glue PRIVATE "LINKER:--exclude-libs,ALL")

# SDK/Arrow headers are PRIVATE: the outer extension includes only the SDK-free
# odps_sdk_glue.h (its own dir is added by the parent CMakeLists).
target_include_directories(odps_sdk_glue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${NEUG_ODPS_SDK_INCLUDE_DIRS})

# neug::odps_sdk is an INTERFACE IMPORTED target carrying the four SDK static
# libs (--start-group wrapped) plus the SDK's shared Arrow/protobuf referenced
# by FULL PATH. Linked PRIVATE into this .so so the SDK symbols are resolved
# here and never propagate into the ABI=1 outer extension.
target_link_libraries(odps_sdk_glue PRIVATE neug::odps_sdk)

# glue.cc transitively includes SDK headers that the SDK build generates back
# into its source tree (common/sdk_version.h), so compile only after the
# ExternalProject has built.
add_dependencies(odps_sdk_glue ${NEUG_ODPS_SDK_EXTERNAL_TARGET})

# Bundle the SDK's SHARED ABI=0 runtime deps (Arrow 1.0.0, protobuf 3.7.1) next
# to libodps_sdk_glue.so so its $ORIGIN RUNPATH resolves them at load. The SDK
# is Linux/GCC-only (plan.md C2), so the bash glob is safe here. `cp -a`
# preserves the SONAME symlink chains (libarrow.so.100 -> .100.0.0).
add_custom_command(TARGET odps_sdk_glue POST_BUILD
    COMMAND bash -c "cp -a '${NEUG_ODPS_SDK_DEPS_LIB_DIR}'/libarrow.so* '${NEUG_ODPS_SDK_DEPS_LIB_DIR}'/libprotobuf*.so* '$<TARGET_FILE_DIR:odps_sdk_glue>/'"
    COMMENT "Bundling SDK Arrow/protobuf runtime .so next to libodps_sdk_glue.so")
