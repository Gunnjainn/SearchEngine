cmake_minimum_required(VERSION 3.18)
project(search_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The tokenizer's fold tables and its tests contain UTF-8 string literals.
# MSVC otherwise reinterprets them in the active code page.
if (MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# std::mutex in the query cache needs -pthread on gcc.
find_package(Threads REQUIRED)

# Crow — header-only HTTP framework
FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
)

# nlohmann/json — header-only JSON library
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# Crow depends on ASIO (standalone, bundled via Crow's own fetch).
# We tell Crow to use its bundled ASIO so we don't need a system install.
set(CROW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(crow json)

# ---------------------------------------------------------------------------
# Engine library
# ---------------------------------------------------------------------------
add_library(engine_lib STATIC
    src/engine.cpp
    src/bm25.cpp
    src/topk.cpp
    src/query.cpp
    src/query_cache.cpp
    src/tokenizer.cpp
    src/porter.cpp
    src/index_io.cpp
    src/doc_store.cpp
)
target_include_directories(engine_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(engine_lib PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(search_engine src/main.cpp)
target_link_libraries(search_engine PRIVATE engine_lib Crow::Crow nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(engine_tests tests/test_engine.cpp)
target_link_libraries(engine_tests PRIVATE engine_lib)

add_executable(tokenizer_tests tests/test_tokenizer.cpp)
target_link_libraries(tokenizer_tests PRIVATE engine_lib)

add_executable(index_tests tests/test_index.cpp)
target_link_libraries(index_tests PRIVATE engine_lib)

add_executable(bm25_tests tests/test_bm25.cpp)
target_link_libraries(bm25_tests PRIVATE engine_lib)

add_executable(topk_tests tests/test_topk.cpp)
target_link_libraries(topk_tests PRIVATE engine_lib)
add_executable(persist_tests tests/test_persist.cpp)
target_link_libraries(persist_tests PRIVATE engine_lib)

add_executable(docstore_tests tests/test_docstore.cpp)
target_link_libraries(docstore_tests PRIVATE engine_lib)

add_executable(query_tests tests/test_query.cpp)
target_link_libraries(query_tests PRIVATE engine_lib)

add_executable(cache_tests tests/test_cache.cpp)
target_link_libraries(cache_tests PRIVATE engine_lib)

# Latency benchmark. Deliberately not registered with ctest: it reports numbers
# rather than passing or failing, and timing assertions make flaky tests.
add_executable(bench_query tools/bench_query.cpp)
target_link_libraries(bench_query PRIVATE engine_lib)

enable_testing()
add_test(NAME engine_unit_tests    COMMAND engine_tests)
add_test(NAME tokenizer_unit_tests COMMAND tokenizer_tests)
add_test(NAME index_unit_tests     COMMAND index_tests)
add_test(NAME bm25_unit_tests      COMMAND bm25_tests)
add_test(NAME topk_unit_tests      COMMAND topk_tests)
add_test(NAME persist_unit_tests   COMMAND persist_tests)
add_test(NAME docstore_unit_tests  COMMAND docstore_tests)
add_test(NAME query_unit_tests     COMMAND query_tests)
add_test(NAME cache_unit_tests     COMMAND cache_tests)
