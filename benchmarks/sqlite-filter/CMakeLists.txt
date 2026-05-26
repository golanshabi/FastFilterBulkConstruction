cmake_minimum_required(VERSION 3.16)

project(ffbc_sqlite_filter_benchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

get_filename_component(FFBC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(
    SQLITE_PPF_SOURCE_DIR
    "${FFBC_ROOT}/sqlite-past-present-future"
    CACHE PATH
    "Path to the unmodified sqlite-past-present-future checkout"
)

set(SQLITE_AMALGAMATION_DIR "${SQLITE_PPF_SOURCE_DIR}/src/systems/sqlite")
if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
    message(
        FATAL_ERROR
        "SQLite source not found at ${SQLITE_AMALGAMATION_DIR}. "
        "Initialize the sqlite-past-present-future submodule or set "
        "SQLITE_PPF_SOURCE_DIR."
    )
endif()

include(FetchContent)
FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG v2.2.1
    GIT_SHALLOW TRUE
)
set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CXXOPTS_ENABLE_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cxxopts)

add_library(sqlite3_filter_benchmark STATIC
    "${SQLITE_AMALGAMATION_DIR}/sqlite3.c"
)
target_include_directories(sqlite3_filter_benchmark PUBLIC
    "${SQLITE_AMALGAMATION_DIR}"
)
target_compile_definitions(sqlite3_filter_benchmark PRIVATE
    SQLITE_DQS=0
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA
    SQLITE_OMIT_AUTOINIT
)

add_executable(filter_build_sqlite3
    filter_build_sqlite3.cpp
    "${FFBC_ROOT}/Tests/sort_bucket_keys.cpp"
    "${FFBC_ROOT}/third_party/highway/hwy/contrib/sort/vqsort.cc"
    "${FFBC_ROOT}/third_party/highway/hwy/contrib/sort/vqsort_u64a.cc"
    "${FFBC_ROOT}/third_party/highway/hwy/aligned_allocator.cc"
    "${FFBC_ROOT}/third_party/highway/hwy/targets.cc"
    "${FFBC_ROOT}/third_party/highway/hwy/per_target.cc"
    "${FFBC_ROOT}/third_party/highway/hwy/abort.cc"
)
target_include_directories(filter_build_sqlite3 PRIVATE
    "${FFBC_ROOT}"
    "${FFBC_ROOT}/Tests"
    "${FFBC_ROOT}/third_party/highway"
    "${FFBC_ROOT}/ips2ra/include"
)
target_link_libraries(filter_build_sqlite3 PRIVATE
    cxxopts
    sqlite3_filter_benchmark
)
target_compile_options(filter_build_sqlite3 PRIVATE
    -O3
    -DNDEBUG
    -march=native
    -mavx2
    -mbmi
    -mbmi2
    -mlzcnt
    -mpopcnt
    -mavx512f
    -mavx512cd
    -mavx512dq
    -mavx512vl
    -mavx512vbmi
)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/filter_build.sh"
    "${CMAKE_BINARY_DIR}/filter_build.sh"
    COPYONLY
)
