cmake_minimum_required(VERSION 3.22)
project(perfetto_imgui LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- nlohmann/json ---
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

# --- SDL3 ---
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.8
  GIT_SHALLOW    TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

# --- Dear ImGui (docking branch) ---
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        docking
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(json SDL3 imgui)

# Build ImGui as a static library
add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static)

find_package(OpenGL REQUIRED)

# --- Application ---
file(GLOB_RECURSE APP_SOURCES src/*.cpp)
add_executable(perfetto_imgui ${APP_SOURCES})
target_include_directories(perfetto_imgui PRIVATE src)
target_link_libraries(perfetto_imgui PRIVATE
  imgui_lib
  nlohmann_json::nlohmann_json
  SDL3::SDL3-static
  OpenGL::GL
)

# --- Tests ---
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  enable_testing()

  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
  )
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Collect non-main source files for linking with tests
  file(GLOB_RECURSE LIB_SOURCES src/*.cpp)
  list(FILTER LIB_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

  file(GLOB TEST_SOURCES tests/*.cpp)
  add_executable(perfetto_tests ${TEST_SOURCES} ${LIB_SOURCES})
  target_include_directories(perfetto_tests PRIVATE src)
  target_link_libraries(perfetto_tests PRIVATE
    GTest::gtest_main
    imgui_lib
    nlohmann_json::nlohmann_json
    SDL3::SDL3-static
    OpenGL::GL
  )

  include(GoogleTest)
  gtest_discover_tests(perfetto_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
