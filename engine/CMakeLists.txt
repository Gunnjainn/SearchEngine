cmake_minimum_required(VERSION 3.14)
project(search_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies via FetchContent ─────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_Declare(
    doctest
    URL      https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
)

FetchContent_MakeAvailable(nlohmann_json doctest)

# ── Engine library ────────────────────────────────────────────────────────────

add_library(engine_lib STATIC src/engine.cpp)
target_include_directories(engine_lib PUBLIC include)
target_link_libraries(engine_lib PUBLIC nlohmann_json::nlohmann_json)

# ── CLI binary ────────────────────────────────────────────────────────────────

add_executable(engine_cli src/main.cpp)
target_link_libraries(engine_cli PRIVATE engine_lib)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE engine_lib doctest::doctest)

add_test(NAME engine_test COMMAND engine_test)
