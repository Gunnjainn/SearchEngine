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
    src/tokenizer.cpp
    src/porter.cpp
)
target_include_directories(engine_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(engine_lib PUBLIC nlohmann_json::nlohmann_json)

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

enable_testing()
add_test(NAME engine_unit_tests    COMMAND engine_tests)
add_test(NAME tokenizer_unit_tests COMMAND tokenizer_tests)