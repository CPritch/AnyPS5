if(BUILD_TESTING)
    set_tests_properties(agc_driver_compute_prebuild_priority PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PREBUILD_THREADS=4" TIMEOUT 60)
    agc_add_env_test(agc_driver_compute_prebuild_priority_wide agc_driver_compute_prebuild_priority_tests
            ARGS wide ENV ANYPS5_NO_SHADER_CACHE=1 APS5_PREBUILD_THREADS=8 TIMEOUT 60)
endif()
