if(BUILD_TESTING)
    add_test(NAME agc_driver_prepare_ahead_device_verify COMMAND agc_driver_prepare_ahead_device_tests)
    set_tests_properties(agc_driver_prepare_ahead_device_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_FAST_F16=0;APS5_FAST_F32=0;APS5_A2C_FALLBACK=0;APS5_VERIFY_PREPARE_AHEAD=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_prepare_ahead_device_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
