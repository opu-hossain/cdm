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
# libcurl – prefer system, fetch as fallback
#
# Rationale:
#   * Every Linux distro ships libcurl with a stable ABI (libcurl.so.4).
#   * Vendoring forces us to also ship openssl/zlib/brotli/nghttp2/zstd,
#     which bloats the package and leaks curl's own install rules.
#   * Set -DDOWNLOADMGR_USE_SYSTEM_CURL=OFF only for offline or pinned builds.
# ============================================================
option(DOWNLOADMGR_USE_SYSTEM_CURL
       "Use the system libcurl instead of fetching and building curl" ON)

if(DOWNLOADMGR_USE_SYSTEM_CURL)
    find_package(CURL 7.60 REQUIRED)
    message(STATUS "Using system curl: ${CURL_VERSION_STRING}")
    message(STATUS "  include: ${CURL_INCLUDE_DIRS}")
    message(STATUS "  library: ${CURL_LIBRARIES}")

    add_library(downloadmgr_curl INTERFACE)
    target_include_directories(downloadmgr_curl INTERFACE ${CURL_INCLUDE_DIRS})
    target_link_libraries(downloadmgr_curl INTERFACE ${CURL_LIBRARIES})
    # System curl on Linux is always shared, so CURL_STATICLIB must be off.
    target_compile_definitions(downloadmgr_curl INTERFACE CURL_STATICLIB=0)
else()
    message(STATUS "Fetching curl from source (DOWNLOADMGR_USE_SYSTEM_CURL=OFF)")

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
    # EXCLUDE_FROM_ALL keeps curl's install() rules out of our CPack payload.
    FetchContent_GetProperties(curl)
    if(NOT curl_POPULATED)
        FetchContent_Populate(curl)
        add_subdirectory(${curl_SOURCE_DIR} ${curl_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

    add_library(downloadmgr_curl INTERFACE)
    target_link_libraries(downloadmgr_curl INTERFACE libcurl)
    target_compile_definitions(downloadmgr_curl INTERFACE CURL_STATICLIB=1)
endif()

# ============================================================
# SQLite3 – system library (required)
# ============================================================
find_package(SQLite3 3.24 REQUIRED)
message(STATUS "Using system sqlite3: ${SQLite3_VERSION}")
message(STATUS "  include: ${SQLite3_INCLUDE_DIRS}")
message(STATUS "  library: ${SQLite3_LIBRARIES}")

add_library(downloadmgr_sqlite INTERFACE)
target_include_directories(downloadmgr_sqlite INTERFACE ${SQLite3_INCLUDE_DIRS})
target_link_libraries(downloadmgr_sqlite INTERFACE ${SQLite3_LIBRARIES})

# ============================================================
# Webview – fetched
#
# EXCLUDE_FROM_ALL so webview does not inject its own install rules
# (headers, cmake configs) into our CPack payload.
# ============================================================
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG cbbdee44afff22867de9fd88a9fc8350d9bdd399
)
FetchContent_GetProperties(webview)
if(NOT webview_POPULATED)
    FetchContent_Populate(webview)
    add_subdirectory(${webview_SOURCE_DIR} ${webview_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

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
    downloadmgr_curl
    downloadmgr_sqlite      # was: sqlite3
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

# ============================================================
# CPack – DEB + RPM
# ============================================================
set(CPACK_PACKAGE_NAME "downloadmgr")
set(CPACK_PACKAGE_VENDOR "Core Download Manager")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Core Download Manager")
set(CPACK_PACKAGE_DESCRIPTION
    "Core Download Manager (cdm) is a segmented download manager with a CLI, a GTK/WebView GUI, and a resident daemon.")
set(CPACK_PACKAGE_CONTACT "Core Download Manager maintainers")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/opu-hossain/cdm")

# ============================================================
# Release version – read from .release.toml
#
#   * "~" is the correct separator for pre-releases in both dpkg and RPM:
#     0.1.0~rc1 sorts BELOW 0.1.0, so the final release supersedes the RC.
#   * CPACK_PACKAGE_FILE_NAME uses "-" because "~" in filenames is awkward.
# ============================================================
set(_channel "rc")
set(_version_base "0.1.0")
set(_rc "1")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.release.toml")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.release.toml" _release_lines)
    foreach(_line IN LISTS _release_lines)
        if(_line MATCHES "^[ \t]*channel[ \t]*=[ \t]*[\"']?([A-Za-z0-9_-]+)")
            set(_channel "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^[ \t]*version[ \t]*=[ \t]*[\"']?([0-9]+\\.[0-9]+\\.[0-9]+)")
            set(_version_base "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^[ \t]*rc[ \t]*=[ \t]*([0-9]+)")
            set(_rc "${CMAKE_MATCH_1}")
        endif()
    endforeach()
else()
    message(WARNING ".release.toml not found; using defaults (rc 0.1.0~rc1)")
endif()

if(_channel STREQUAL "rc")
    set(DOWNLOADMGR_VERSION "${_version_base}~rc${_rc}")
else()
    set(DOWNLOADMGR_VERSION "${_version_base}")
endif()

# Split base version into MAJOR/MINOR/PATCH for CPack (some generators need them).
string(REPLACE "." ";" _ver_parts "${_version_base}")
list(LENGTH _ver_parts _ver_count)
if(_ver_count EQUAL 3)
    list(GET _ver_parts 0 CPACK_PACKAGE_VERSION_MAJOR)
    list(GET _ver_parts 1 CPACK_PACKAGE_VERSION_MINOR)
    list(GET _ver_parts 2 CPACK_PACKAGE_VERSION_PATCH)
else()
    set(CPACK_PACKAGE_VERSION_MAJOR "${_version_base}")
endif()

set(CPACK_PACKAGE_VERSION "${DOWNLOADMGR_VERSION}")

# Filename uses "-" instead of "~" for shell-friendliness.
string(REPLACE "~" "-" _version_dashed "${DOWNLOADMGR_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${_version_dashed}-Linux")

message(STATUS "Release channel: ${_channel}")
message(STATUS "Release version: ${DOWNLOADMGR_VERSION}")
message(STATUS "Package version: ${CPACK_PACKAGE_VERSION}")
message(STATUS "Package file name: ${CPACK_PACKAGE_FILE_NAME}")

# Generators are selectable via -DCPACK_GENERATOR=DEB / RPM
set(CPACK_GENERATOR "DEB;RPM")

# ---------- Debian ----------
# libcurl4t64 on Ubuntu 24.04+; libcurl4 on older. SHLIBDEPS picks the right
# one automatically, so we only list the GUI libs here as a floor.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libnotify4, libgtk-3-0, libgtk-3-0t64, libwebkit2gtk-4.1-0")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/opu-hossain/cdm")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")

# ---------- RPM ----------
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_REQUIRES "libnotify, gtk3, webkit2gtk4.1")
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_SUMMARY "Core Download Manager")

include(CPack)
