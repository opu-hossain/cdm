# Test sources.
set(TEST_SOURCES
    test_segmenter.c
    test_queue_manager.c
    test_db.c
    test_file_io.c
    test_finalize.c
    test_ipc.c
    test_curl_client.c
    test_log.c
    test_thread.c
    test_spawn.c
    test_ipc_socket.c
    test_scheduler.c
    test_engine_runner.c
    test_worker_pool.c
    test_path.c
    test_gui_controller.c
)

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    downloadmgr_enable_warnings(${TEST_NAME})
    downloadmgr_enable_sanitizer(${TEST_NAME})
    if(TEST_NAME STREQUAL test_engine_runner)
        target_sources(${TEST_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/engine/engine_runner.c
        )
    elseif(TEST_NAME STREQUAL test_gui_controller)
        target_sources(${TEST_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/gui/gui_controller.c
            ${CMAKE_SOURCE_DIR}/src/gui/gui_client.c
        )
    endif()
    target_link_libraries(${TEST_NAME}
        PRIVATE
        downloadmgr_core
        PkgConfig::CRITERION
    )
    if(TEST_NAME STREQUAL test_scheduler)
        # Expose Criterion case names in verbose CTest output.
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME} --verbose)
    else()
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endif()
endforeach()

if(UNIX AND NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_native_host_protocol
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_native_host_protocol.py
                         $<TARGET_FILE:cdm_native_host>)
        add_test(NAME test_browser_install
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_browser_install.py
                         $<TARGET_FILE:cdm>)
        add_test(NAME test_browser_end_to_end
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_browser_end_to_end.py
                         $<TARGET_FILE:cdm> $<TARGET_FILE:cdm_native_host>)
        add_test(NAME test_browser_autostart
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_browser_autostart.py
                         $<TARGET_FILE:cdm_native_host>)
        add_test(NAME test_daemon_lifecycle
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_daemon_lifecycle.py
                         $<TARGET_FILE:cdm>)
        add_test(NAME test_cli_headers
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_headers.py
                         $<TARGET_FILE:cdm>)
        add_test(NAME test_cli_rejected_add
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_rejected_add.py
                         $<TARGET_FILE:cdm>)
        add_executable(test_curl_http_integration test_curl_http_integration.c)
        downloadmgr_enable_warnings(test_curl_http_integration)
        downloadmgr_enable_sanitizer(test_curl_http_integration)
        target_link_libraries(test_curl_http_integration
            PRIVATE downloadmgr_core PkgConfig::CRITERION)
        add_test(NAME test_curl_http_integration
                 COMMAND test_curl_http_integration)
    endif()
endif()

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME test_browser_extensions
             COMMAND ${NODE_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_browser_extensions.js
                     ${CMAKE_SOURCE_DIR}/browser/chromium/service_worker.js
                     ${CMAKE_SOURCE_DIR}/browser/firefox/background.js)
endif()

if(UNIX AND NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_executable(test_engine_http_integration test_engine_http_integration.c)
        downloadmgr_enable_warnings(test_engine_http_integration)
        downloadmgr_enable_sanitizer(test_engine_http_integration)
        target_link_libraries(test_engine_http_integration
            PRIVATE downloadmgr_core PkgConfig::CRITERION)
        add_test(NAME test_engine_http_integration
                 COMMAND test_engine_http_integration)
    endif()
endif()
