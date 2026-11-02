if(BUILD_TESTING)
    add_test(NAME agc_driver_dcc_keys_first_write_readback_waits COMMAND agc_driver_dcc_keys_first_write_tests)
    set_tests_properties(agc_driver_dcc_keys_first_write_readback_waits PROPERTIES ENVIRONMENT "APS5_NO_DRAW_READBACK_WAITS=0;ANYPS5_NO_SHADER_CACHE=1;APS5_FAST_F16=0;APS5_FAST_F32=0" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_dcc_keys_first_write_readback_waits PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
