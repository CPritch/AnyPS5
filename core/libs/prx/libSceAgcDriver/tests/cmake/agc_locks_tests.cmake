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
    add_test(NAME agc_locks_holder_note COMMAND agc_locks_tests)
    set_tests_properties(agc_locks_holder_note PROPERTIES TIMEOUT 60 ENVIRONMENT "APS5_COMMIT_STATS=1")
    add_test(NAME agc_locks_holder_note_off COMMAND agc_locks_tests)
    set_tests_properties(agc_locks_holder_note_off PROPERTIES TIMEOUT 60 ENVIRONMENT "APS5_COMMIT_STATS=1;APS5_NO_EARLY_HOLDER_NOTE=1")
    if(WIN32)
        set_tests_properties(agc_locks_holder_note agc_locks_holder_note_off PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
