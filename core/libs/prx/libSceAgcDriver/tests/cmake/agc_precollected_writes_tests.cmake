if(BUILD_TESTING AND NOT WIN32)
    add_test_executable(agc_precollected_writes_tests
            tests/PrecollectedWrites.cpp
            Execution/src/GuestMemory.cpp
            Execution/src/PerformanceTraceThread.cpp
    )
    target_include_directories(agc_precollected_writes_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_precollected_writes_tests PRIVATE shader_recompiler_agc_driver libc)
    set_target_properties(agc_precollected_writes_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME agc_precollected_writes COMMAND agc_precollected_writes_tests)
    set_tests_properties(agc_precollected_writes PROPERTIES TIMEOUT 30)
endif()
