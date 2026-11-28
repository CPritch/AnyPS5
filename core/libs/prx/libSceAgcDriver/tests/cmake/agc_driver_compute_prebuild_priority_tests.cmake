if(BUILD_TESTING)
    set_tests_properties(agc_driver_compute_prebuild_priority PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PREBUILD_THREADS=4" TIMEOUT 60)
endif()
