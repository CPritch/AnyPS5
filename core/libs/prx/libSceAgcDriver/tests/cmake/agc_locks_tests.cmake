add_test_executable(agc_locks_tests EXCLUDE_FROM_ALL tests/Locks.cpp)
target_include_directories(agc_locks_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_locks_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_locks_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_locks_tests)

if(BUILD_TESTING)
    agc_add_default_test(agc_locks_tests)
    add_test(NAME agc_locks_no_handoff COMMAND agc_locks_tests)
    set_tests_properties(agc_locks_no_handoff PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_GPU_MUTEX_HANDOFF_MS=0" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_locks_no_handoff PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
