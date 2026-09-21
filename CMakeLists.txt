cmake_minimum_required(VERSION 3.20)
# FIX: Must include both C and CXX since this is a C project integrating a C++ library (webview)
project(downloadmgr LANGUAGES C CXX)

include(GNUInstallDirs)

# ----- Global settings -----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DOWNLOADMGR_ENABLE_WARNINGS "Enable warnings for project targets" ON)
set(DOWNLOADMGR_SANITIZER "" CACHE STRING
    "Sanitizer to enable: address, undefined, or thread")
set_property(CACHE DOWNLOADMGR_SANITIZER PROPERTY STRINGS "" address undefined thread)

function(downloadmgr_enable_warnings target)
    if(NOT DOWNLOADMGR_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(downloadmgr_enable_sanitizer target)
    if(DOWNLOADMGR_SANITIZER STREQUAL "")
        return()
    endif()

    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
            "DOWNLOADMGR_SANITIZER requires GCC or Clang-compatible compilers")
    endif()

    if(DOWNLOADMGR_SANITIZER STREQUAL "address")
        set(flags -fsanitize=address -fno-omit-frame-pointer)
    elseif(DOWNLOADMGR_SANITIZER STREQUAL "undefined")
        set(flags -fsanitize=undefined -fno-omit-frame-pointer)
    elseif(DOWNLOADMGR_SANITIZER STREQUAL "thread")
        set(flags -fsanitize=thread -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "Unknown DOWNLOADMGR_SANITIZER='${DOWNLOADMGR_SANITIZER}'")
    endif()

    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
endfunction()

include(CTest)

# Make FetchContent verbose so you can see download progress and logs for debugging
set(FETCHCONTENT_QUIET OFF)

include(FetchContent)

# ============================================================
# System dependencies (pkg-config)
# ============================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)

if(BUILD_TESTING)
    pkg_check_modules(CRITERION REQUIRED IMPORTED_TARGET criterion)
endif()

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
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})

# ============================================================
# Webview – fetched
# ============================================================
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG cbbdee44afff22867de9fd88a9fc8350d9bdd399
)
FetchContent_MakeAvailable(webview)

# ============================================================
# Vendor (third‑party) static library
# ============================================================
add_library(downloadmgr_vendor STATIC
    src/vendor/cJSON.c
    src/vendor/tinyfiledialogs.c
    src/vendor/tomlc17.c
)
target_include_directories(downloadmgr_vendor PUBLIC src/vendor)

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
downloadmgr_enable_warnings(downloadmgr_core)
downloadmgr_enable_sanitizer(downloadmgr_core)

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
    src/gui/gui_client.c
    src/gui/gui_model.c
    src/gui/gui_worker.c
    src/daemon/daemon.c
)
downloadmgr_enable_warnings(downloadmgr)
downloadmgr_enable_sanitizer(downloadmgr)

target_include_directories(downloadmgr PRIVATE src)

target_link_libraries(downloadmgr PRIVATE
    downloadmgr_core
    webview::core_static
)

add_custom_command(TARGET downloadmgr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/src/gui/ui
    $<TARGET_FILE_DIR:downloadmgr>/src/gui/ui
)

# ============================================================
# Browser native host executable
# ============================================================
add_executable(downloadmgr_native_host
    src/native_host/main_host.c
)
downloadmgr_enable_warnings(downloadmgr_native_host)
downloadmgr_enable_sanitizer(downloadmgr_native_host)

target_link_libraries(downloadmgr_native_host PRIVATE downloadmgr_core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS downloadmgr
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY src/gui/ui/
    DESTINATION ${CMAKE_INSTALL_BINDIR}/ui
)
install(FILES resources/downloadmgr.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

set(CPACK_PACKAGE_NAME "downloadmgr")
set(CPACK_PACKAGE_VENDOR "Core Download Manager")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Core Download Manager")
set(CPACK_PACKAGE_CONTACT "Core Download Manager maintainers")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 1)
set(CPACK_PACKAGE_VERSION_PATCH 0)
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libnotify4, libgtk-3-0")
include(CPack)
