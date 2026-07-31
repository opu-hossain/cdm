cmake_minimum_required(VERSION 3.20)
project(downloadmgr LANGUAGES C)

# ----- Global settings -----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Less noise during FetchContent
option(FETCHCONTENT_QUIET "Silence FetchContent output" ON)
set(FETCHCONTENT_QUIET ${FETCHCONTENT_QUIET})

include(FetchContent)

# ----- System package option (e.g. for packagers) -----
option(USE_SYSTEM_RAYLIB "Use system-installed raylib" OFF)

# ============================================================
# System dependencies (pkg-config)
# ============================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)

# ============================================================
# libcurl – always fetched (can be changed to system later)
# ============================================================
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTMP ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_21_0
)
FetchContent_MakeAvailable(curl)

# ============================================================
# SQLite3 – fetched amalgamation
# ============================================================
FetchContent_Declare(
    sqlite3
    URL https://sqlite.org/2024/sqlite-amalgamation-3460000.zip
)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})

# ============================================================
# raylib – optionally system, otherwise fetch
# ============================================================
if(USE_SYSTEM_RAYLIB)
    find_package(raylib REQUIRED)
else()
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 6.0
    )
    FetchContent_MakeAvailable(raylib)
endif()

# ============================================================
# raygui (header‑only) – fetched
# ============================================================
FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 5.0
)
FetchContent_MakeAvailable(raygui)

# ============================================================
# Vendor (third‑party) static library
# ============================================================
add_library(downloadmgr_vendor STATIC
    src/vendor/cJSON.c
    src/vendor/tinyfiledialogs.c
    src/vendor/tomlc17.c
)
target_include_directories(downloadmgr_vendor PUBLIC src/vendor)

# Platform‑specific defines if any (none needed for these libs currently)

# ============================================================
# Project source groups
# ============================================================
set(PLATFORM_SOURCES
    src/platform/bandwidth.c
    src/platform/curl_client.c
    src/platform/file_io.c
    src/platform/ipc_socket.c
    src/platform/sha256.c
    src/platform/spawn.c
    src/platform/thread.c
)

set(UTILS_SOURCES
    src/utils/config.c
    src/utils/log.c
    src/utils/notify.c
)

set(ENGINE_SOURCES
    src/engine/engine_runner.c
    src/engine/segmenter.c
    src/engine/worker_pool.c
    src/engine/finalize.c
)

set(CORE_SOURCES
    src/core/queue_manager.c
    src/core/scheduler.c
)

set(PERSISTENCE_SOURCES
    src/persistence/db.c
)

# ============================================================
# Core library – shared code (no main)
# ============================================================
add_library(downloadmgr_core STATIC
    ${PLATFORM_SOURCES}
    ${UTILS_SOURCES}
    ${ENGINE_SOURCES}
    ${CORE_SOURCES}
    ${PERSISTENCE_SOURCES}
)

target_include_directories(downloadmgr_core PRIVATE src)
target_link_libraries(downloadmgr_core PUBLIC
    downloadmgr_vendor
    libcurl
    sqlite3
    PkgConfig::LIBNOTIFY
)

if(WIN32)
    target_link_libraries(downloadmgr_core PUBLIC ws2_32 comdlg32 ole32)
elseif(UNIX)
    target_link_libraries(downloadmgr_core PUBLIC pthread)
endif()

# ============================================================
# Main executable (CLI / GUI / daemon)
# ============================================================
add_executable(downloadmgr
    src/main.c
    src/cli/cli.c
    src/gui/gui.c
    src/daemon/daemon.c
)

target_include_directories(downloadmgr PRIVATE
    src
    ${raygui_SOURCE_DIR}
    ${raygui_SOURCE_DIR}/src
)

target_link_libraries(downloadmgr PRIVATE
    downloadmgr_core
    raylib
)

# ============================================================
# Browser native host executable
# ============================================================
add_executable(downloadmgr_native_host
    src/native_host/main_host.c
)

target_link_libraries(downloadmgr_native_host PRIVATE downloadmgr_core)

# ============================================================
# Tests (disabled by default – enable with -DBUILD_TESTS=ON)
# ============================================================
# option(BUILD_TESTS "Build unit tests" OFF)
# 
# if(BUILD_TESTS)
#     enable_testing()
#     # Assuming you use Criterion (example)
#     # find_package(Criterion REQUIRED)   # or your test framework
#     add_subdirectory(tests)
# endif()
