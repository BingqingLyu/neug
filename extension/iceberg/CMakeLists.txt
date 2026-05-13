# Collect source files
file(GLOB ICEBERG_EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/metadata/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/metadata/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/schema/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/schema/*.cpp")
set(ICEBERG_EXTENSION_OBJECT_FILES ${ICEBERG_EXTENSION_SOURCES})

# Use the extension build system
build_extension_lib("iceberg")

# Set include directories
target_include_directories(neug_iceberg_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR})

if(ARROW_INCLUDE_DIRS)
    target_include_directories(neug_iceberg_extension PRIVATE SYSTEM ${ARROW_INCLUDE_DIRS})
endif()

# Link Arrow components needed for Iceberg extension:
# - arrow_dataset: for DatasetFactory, Scanner, FileFormat
# - parquet: for reading Parquet data files within Iceberg tables
if(TARGET arrow_dataset_objlib)
    set(_EXT_ARROW_DATASET_LIB arrow_dataset_objlib)
else()
    set(_EXT_ARROW_DATASET_LIB ${ARROW_DATASET_LIB})
endif()
if(TARGET parquet_objlib)
    set(_EXT_ARROW_PARQUET_LIB parquet_objlib)
elseif(TARGET arrow_parquet_objlib)
    set(_EXT_ARROW_PARQUET_LIB arrow_parquet_objlib)
else()
    set(_EXT_ARROW_PARQUET_LIB ${ARROW_PARQUET_LIB})
endif()
target_link_libraries(neug_iceberg_extension PRIVATE
    neug
    ${ARROW_BASE_LIB}
    ${_EXT_ARROW_DATASET_LIB}
    ${_EXT_ARROW_PARQUET_LIB}
)

# Build test by compiling extension sources directly (no link to neug_iceberg_extension)
if(BUILD_TEST)
    if(TARGET arrow_dataset_objlib)
        set(_iceberg_test_dataset_lib arrow_dataset_objlib)
    elseif(ARROW_DATASET_LIB)
        set(_iceberg_test_dataset_lib ${ARROW_DATASET_LIB})
    else()
        set(_iceberg_test_dataset_lib "")
    endif()
    add_extension_test(NAME iceberg
        TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tests/iceberg_test.cpp
        EXTRA_LINK_LIBS ${_iceberg_test_dataset_lib} ${ARROW_PARQUET_LIB}
    )
endif()
