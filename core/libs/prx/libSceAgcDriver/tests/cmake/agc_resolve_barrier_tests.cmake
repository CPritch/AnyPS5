if(BUILD_TESTING AND NOT WIN32)
    add_test_executable(agc_resolve_barrier_tests
            tests/ResolveBarrier.cpp
            Execution/src/GuestMemory.cpp
            Execution/src/PerformanceTraceThread.cpp
    )
    target_include_directories(agc_resolve_barrier_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_resolve_barrier_tests PRIVATE shader_recompiler_agc_driver libc)
    set_target_properties(agc_resolve_barrier_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME agc_resolve_barrier COMMAND agc_resolve_barrier_tests)
    set_tests_properties(agc_resolve_barrier PROPERTIES TIMEOUT 30)
endif()
