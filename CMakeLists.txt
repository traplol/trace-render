cmake_minimum_required(VERSION 3.22)
project(trace_render LANGUAGES C CXX)
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
if(NOT EMSCRIPTEN)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.8
    GIT_SHALLOW    TRUE
  )
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON  CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
endif()

# --- Dear ImGui (docking branch) ---
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        docking
  GIT_SHALLOW    TRUE
)

# --- SQLite (amalgamation) ---
FetchContent_Declare(sqlite3
  URL        https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
)

if(EMSCRIPTEN)
  FetchContent_MakeAvailable(json imgui sqlite3)
else()
  FetchContent_MakeAvailable(json SDL3 imgui sqlite3)
endif()

# Build SQLite as a static library
add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3_lib PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION)

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

if(EMSCRIPTEN)
  # SDL3 is provided via Emscripten port
  target_compile_options(imgui_lib PUBLIC -sUSE_SDL=3)
  target_link_options(imgui_lib PUBLIC -sUSE_SDL=3)
else()
  target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static)
  find_package(OpenGL REQUIRED)
endif()

# --- Application ---
file(GLOB_RECURSE APP_SOURCES src/*.cpp)
# Exclude the wrong platform's source files
if(EMSCRIPTEN)
  list(FILTER APP_SOURCES EXCLUDE REGEX "platform_desktop\\.cpp$|file_loader_desktop\\.cpp$")
else()
  list(FILTER APP_SOURCES EXCLUDE REGEX "platform_wasm\\.cpp$|file_loader_wasm\\.cpp$")
endif()
add_executable(trace_render ${APP_SOURCES})
target_include_directories(trace_render PRIVATE src)

if(EMSCRIPTEN)
  target_link_libraries(trace_render PRIVATE
    imgui_lib
    nlohmann_json::nlohmann_json
    sqlite3_lib
  )
  # Emscripten linker flags
  target_link_options(trace_render PRIVATE
    -sUSE_SDL=3
    -sMAX_WEBGL_VERSION=2
    -sMIN_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=134217728
    -sEXPORTED_FUNCTIONS=_main,_wasm_file_loaded,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,lengthBytesUTF8,stringToUTF8
    --shell-file=${CMAKE_SOURCE_DIR}/web/shell.html
  )
  set_target_properties(trace_render PROPERTIES
    SUFFIX ".html"
    OUTPUT_NAME "index"
  )
else()
  target_link_libraries(trace_render PRIVATE
    imgui_lib
    nlohmann_json::nlohmann_json
    sqlite3_lib
    SDL3::SDL3-static
    OpenGL::GL
  )
endif()

# --- Tests (desktop only) ---
if(NOT EMSCRIPTEN)
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
    list(FILTER LIB_SOURCES EXCLUDE REGEX "platform_wasm\\.cpp$|file_loader_wasm\\.cpp$")

    file(GLOB TEST_SOURCES tests/*.cpp)
    add_executable(trace_render_tests ${TEST_SOURCES} ${LIB_SOURCES})
    target_include_directories(trace_render_tests PRIVATE src)
    target_link_libraries(trace_render_tests PRIVATE
      GTest::gtest_main
      imgui_lib
      nlohmann_json::nlohmann_json
      sqlite3_lib
      SDL3::SDL3-static
      OpenGL::GL
    )

    include(GoogleTest)
    gtest_discover_tests(trace_render_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endif()
