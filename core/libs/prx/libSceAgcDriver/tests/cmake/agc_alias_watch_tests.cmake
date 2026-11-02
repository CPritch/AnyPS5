if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test_executable(agc_alias_watch_tests
            tests/AliasWatch.cpp
            Execution/src/GuestMemory.cpp
            Execution/src/PerformanceTraceThread.cpp
    )
    target_include_directories(agc_alias_watch_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_alias_watch_tests PRIVATE shader_recompiler_agc_driver libkernel libc)
    set_target_properties(agc_alias_watch_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME agc_alias_watch COMMAND agc_alias_watch_tests)
    set_tests_properties(agc_alias_watch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_ALIAS_WATCH=1;APS5_ALIAS_WATCH_AUDIT=4")
endif()
