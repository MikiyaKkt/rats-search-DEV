# Rats Search Test Suite
cmake_minimum_required(VERSION 3.16)

# Find Qt6 Test (other Qt modules come transitively from ratscore)
find_package(Qt6 REQUIRED COMPONENTS Test)

# Enable Qt testing
enable_testing()

# Optional engine regression suite reuses the exact library linked by the app.
# No implicit network downloads: the one-click script provisions a pinned source.
option(RATS_SEARCH_BUILD_ENGINE_TESTS "Build focused librats regression tests" OFF)
set(RATS_SEARCH_GTEST_SOURCE_DIR "" CACHE PATH "GoogleTest v1.17.0 source directory")
if(RATS_SEARCH_BUILD_ENGINE_TESTS)
    if(NOT EXISTS "${RATS_SEARCH_GTEST_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set RATS_SEARCH_GTEST_SOURCE_DIR to GoogleTest v1.17.0 sources")
    endif()
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory("${RATS_SEARCH_GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
    set(ENGINE_TEST_DIR "${CMAKE_SOURCE_DIR}/src/librats/tests")
    add_executable(test_download_engine
        ${ENGINE_TEST_DIR}/test_fs.cpp
        ${ENGINE_TEST_DIR}/test_bt_disk_io.cpp
        ${ENGINE_TEST_DIR}/test_bt_download.cpp
        ${ENGINE_TEST_DIR}/test_bt_pause_resume.cpp
        ${ENGINE_TEST_DIR}/test_bt_resume_data.cpp
        ${ENGINE_TEST_DIR}/test_bt_piece_picker.cpp
        ${ENGINE_TEST_DIR}/test_bt_stall_metadata.cpp
        ${ENGINE_TEST_DIR}/test_bt_store_buffer.cpp
        ${ENGINE_TEST_DIR}/test_bt_magnet_fetch.cpp
        ${ENGINE_TEST_DIR}/test_bt_subsystem.cpp
        ${ENGINE_TEST_DIR}/test_bt_utp_wire.cpp
        ${ENGINE_TEST_DIR}/test_bt_mse_wire.cpp)
    target_link_libraries(test_download_engine PRIVATE rats GTest::gtest_main)
    add_test(NAME test_download_engine COMMAND test_download_engine)
    set_tests_properties(test_download_engine PROPERTIES TIMEOUT 180)
endif()

# Helper function to add a test with minimal boilerplate.
#
# Tests link directly against ratscore (the new layered core library defined in
# the top-level CMakeLists). ratscore carries every rewritten class plus the
# src/ include directory, so no separate "testable" library is needed.
function(add_rats_test TEST_NAME)
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp ${ARGN})
    target_link_libraries(${TEST_NAME} PRIVATE Qt6::Test ratscore)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    # Qt's plain test logger writes to OutputDebugString instead of stdout when
    # Windows reports no attached console -- which is the case on a CI runner and
    # for anything reading the test through a pipe, ctest included. A failing test
    # then appears as a bare "***Failed" with no FAIL line naming the function.
    # Forcing the console assumption keeps the log on stdout, where ctest sees it.
    if(WIN32)
        set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_ASSUME_STDERR_HAS_CONSOLE=1")
    endif()
endfunction()

# Unit tests (pure, no external process required)
add_rats_test(test_domain)
add_rats_test(test_query)
add_rats_test(test_search_results ${CMAKE_SOURCE_DIR}/src/ui/searchresultmodel.cpp)
add_rats_test(test_infohash)
add_rats_test(test_utf8_download_paths)
add_rats_test(test_download_metrics)
add_rats_test(test_performance_metrics)
add_rats_test(test_crawler_metrics)
add_rats_test(test_crawler_pipeline)
add_rats_test(test_crawler_batch_indexing)
set_tests_properties(test_crawler_batch_indexing PROPERTIES RESOURCE_LOCK manticore_local)

# Controlled P0 baseline: real temporary Manticore, no DHT or user database.
add_executable(crawler_benchmark ../benchmarks/crawler/crawler_benchmark.cpp)
target_link_libraries(crawler_benchmark PRIVATE ratscore)
add_executable(crawler_batch_benchmark ../benchmarks/crawler/crawler_batch_benchmark.cpp)
target_link_libraries(crawler_batch_benchmark PRIVATE ratscore)
add_executable(crawler_live_probe ../benchmarks/crawler/crawler_live_probe.cpp)
target_link_libraries(crawler_live_probe PRIVATE ratscore)
add_executable(crawler_concurrency_probe ../benchmarks/crawler/crawler_concurrency_probe.cpp)
target_link_libraries(crawler_concurrency_probe PRIVATE ratscore)
add_executable(crawler_soak_probe ../benchmarks/crawler/crawler_soak_probe.cpp)
target_link_libraries(crawler_soak_probe PRIVATE ratscore)
if(WIN32)
    target_link_libraries(crawler_concurrency_probe PRIVATE psapi)
    target_link_libraries(crawler_soak_probe PRIVATE psapi)
    target_link_libraries(crawler_live_probe PRIVATE psapi)
endif()
add_rats_test(test_download_policy)
add_rats_test(test_download_s1)
add_rats_test(test_download_s3)
add_rats_test(test_download_ui
    ${CMAKE_SOURCE_DIR}/src/ui/torrentfileswidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/taskdownloaddialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/downloadadvancedwidgets.cpp)
target_link_libraries(test_download_ui PRIVATE Qt6::Widgets)
target_compile_definitions(test_download_ui PRIVATE RATS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" RATS_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_custom_target(download_test_translations DEPENDS ${QM_FILES})
add_dependencies(test_download_ui download_test_translations)
set_tests_properties(test_download_ui PROPERTIES ENVIRONMENT "QT_ASSUME_STDERR_HAS_CONSOLE=1;QT_QPA_PLATFORM=offscreen")
if(WIN32)
    add_executable(download_package_smoke download_package_smoke.cpp)
    target_link_libraries(download_package_smoke PRIVATE Qt6::Test ratscore)
endif()
set_tests_properties(test_download_s1 PROPERTIES TIMEOUT 240)
add_rats_test(test_download_creation)
add_rats_test(test_download_errors)
add_rats_test(test_row_id)
add_rats_test(test_update_service)
add_rats_test(test_config_store)
add_rats_test(test_search_history)
add_rats_test(test_filter_policy)
add_rats_test(test_manticore_maintenance)
add_rats_test(test_database_dump)
add_rats_test(test_database_snapshot)
add_rats_test(test_database_transfer_resume)
add_rats_test(test_voting_record)
add_rats_test(test_scrape_queue)

# The single-instance guard lives in src/bootstrap (executable-only, not in
# ratscore), so its source is compiled straight into the test.
add_rats_test(test_single_instance ${CMAKE_SOURCE_DIR}/src/bootstrap/single_instance.cpp)

# The theme lives in src/ui (executable-only, not in ratscore) and reads its
# stylesheet template and token tables out of the Qt resources, so both the
# source and the qrc are compiled straight into this test.
add_rats_test(test_theme
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

# Content classifier needs the :/content/*.json Qt resources. Those live in the
# main resources.qrc (they are not compiled into ratscore), so compile the qrc
# straight into this test executable (CMAKE_AUTORCC is ON globally).
add_rats_test(test_content_classifier ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

# Integration test: starts a real Manticore instance and drives the new
# data layer (Manticore + Database + TorrentRepository).
add_rats_test(test_manticore_queries)
set_tests_properties(test_manticore_queries PROPERTIES RESOURCE_LOCK manticore_local)
add_rats_test(test_peer_search ${CMAKE_SOURCE_DIR}/src/ui/searchresultmodel.cpp)
set_tests_properties(test_peer_search PROPERTIES RESOURCE_LOCK manticore_local TIMEOUT 90)

# Copy libmysql.dll next to test executables so the QMYSQL driver can load.
#
# It has to be the libmysql the QMYSQL plugin was *built against*. Manticore
# ships its own copy in imports/ for its own tools (8.3 at the time of writing)
# while the prebuilt Qt driver comes with 8.4, and a DLL next to the executable
# wins the search over Qt's bin directory -- so copying Manticore's here made
# the plugin read MYSQL_FIELD at the wrong offsets. It then reports string
# columns with an integer type: `SELECT * FROM files` handed back `hash` as
# uint 0, every hash verification rejected the row as somebody else's, and file
# lists came back empty with no error anywhere. Measured on this repo: with
# Manticore's 8.3 next to the binary test_manticore_queries failed 5 runs out of
# 12, with the driver's 8.4 zero out of 45.
#
# So take Qt's copy and never substitute Manticore's: no library at all fails
# loudly ("can not load requested driver 'QMYSQL'"), the wrong one fails
# silently and intermittently.
if(WIN32)
    set(LIBMYSQL_CANDIDATES "")
    if(DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_BINS)
        list(APPEND LIBMYSQL_CANDIDATES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}/libmysql.dll")
    endif()
    if(TARGET Qt6::Core)
        get_target_property(QT_CORE_LOCATION Qt6::Core LOCATION)
        if(QT_CORE_LOCATION)
            get_filename_component(QT_BIN_DIR "${QT_CORE_LOCATION}" DIRECTORY)
            list(APPEND LIBMYSQL_CANDIDATES "${QT_BIN_DIR}/libmysql.dll")
        endif()
    endif()

    set(LIBMYSQL_PATH "")
    foreach(candidate IN LISTS LIBMYSQL_CANDIDATES)
        if(EXISTS "${candidate}")
            set(LIBMYSQL_PATH "${candidate}")
            break()
        endif()
    endforeach()

    if(LIBMYSQL_PATH)
        message(STATUS "Integration tests: libmysql.dll from ${LIBMYSQL_PATH}")
        set(INTEGRATION_TEST_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        if(CMAKE_CONFIGURATION_TYPES)
            string(APPEND INTEGRATION_TEST_BIN_DIR "/$<CONFIG>")
        endif()
        add_custom_target(integration_test_mysql
            COMMAND ${CMAKE_COMMAND} -E make_directory "${INTEGRATION_TEST_BIN_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBMYSQL_PATH}" "${INTEGRATION_TEST_BIN_DIR}"
            COMMENT "Copying libmysql.dll for integration tests...")
        add_dependencies(test_manticore_queries integration_test_mysql)
        add_dependencies(test_peer_search integration_test_mysql)
    else()
        message(STATUS "Integration tests: no libmysql.dll next to Qt. Copy the one that ships with the "
                       "QMYSQL driver next to the test binary by hand -- NOT the one in imports/, which "
                       "belongs to Manticore and makes the plugin misread result metadata.")
    endif()
endif()

# Custom target to run all tests
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_domain test_query test_search_results test_infohash test_utf8_download_paths test_download_metrics test_performance_metrics test_crawler_metrics test_download_policy test_download_s1 test_download_s3 test_download_ui test_download_creation test_download_errors test_update_service
            test_config_store test_filter_policy test_content_classifier test_manticore_queries test_peer_search
            test_manticore_maintenance
            test_database_dump
            test_voting_record
            test_single_instance
            test_theme
    COMMENT "Running all tests..."
)

message(STATUS "")
message(STATUS "=== Rats Search Tests ===")
message(STATUS "Tests enabled: ON")
if(TARGET test_download_engine)
    add_dependencies(check test_download_engine)
endif()
message(STATUS "Qt6 Test version: ${Qt6Test_VERSION}")
message(STATUS "=========================")
message(STATUS "")
