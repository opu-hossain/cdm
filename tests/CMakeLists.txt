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
    endif()
    target_link_libraries(${TEST_NAME}
        PRIVATE
        downloadmgr_core
        PkgConfig::CRITERION
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

if(UNIX AND NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_executable(test_curl_http_integration test_curl_http_integration.c)
        downloadmgr_enable_warnings(test_curl_http_integration)
        downloadmgr_enable_sanitizer(test_curl_http_integration)
        target_link_libraries(test_curl_http_integration
            PRIVATE downloadmgr_core PkgConfig::CRITERION)
        add_test(NAME test_curl_http_integration
                 COMMAND test_curl_http_integration)
    endif()
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
