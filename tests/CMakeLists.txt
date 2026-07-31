# List all test source files
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
)

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
        downloadmgr_core
        ${CRITERION_LIBRARIES}
    )
    target_include_directories(${TEST_NAME} PRIVATE ${CRITERION_INCLUDE_DIRS})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
