if(BUILD_TESTING)
    set_tests_properties(agc_driver_fast_float16 PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    add_test(NAME agc_driver_fast_float16_off COMMAND agc_driver_fast_float16_tests)
    set_tests_properties(agc_driver_fast_float16_off PROPERTIES ENVIRONMENT "APS5_FAST_F16=0;ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_fast_float16_off PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
