if(BUILD_TESTING)
    add_test(NAME agc_driver_wave64_span_read_lanes_unoptimized COMMAND agc_driver_wave64_span_read_lanes_tests)
    set_tests_properties(agc_driver_wave64_span_read_lanes_unoptimized PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1;APS5_UNOPTIMIZED_COMPUTE_WORDS=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FAIL_REGULAR_EXPRESSION "SYNC-HAZARD-|VUID-|was not found but was requested by env var VK_INSTANCE_LAYERS")
    if(WIN32)
        set_tests_properties(agc_driver_wave64_span_read_lanes_unoptimized PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
