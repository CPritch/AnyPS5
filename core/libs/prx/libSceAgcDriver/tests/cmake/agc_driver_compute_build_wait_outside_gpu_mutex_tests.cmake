if(BUILD_TESTING)
    agc_add_env_test(agc_driver_compute_build_wait_outside_gpu_mutex_descriptor_heap agc_driver_compute_build_wait_outside_gpu_mutex_tests
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1)
    agc_add_env_test(agc_driver_compute_build_wait_outside_gpu_mutex_indirect agc_driver_compute_build_wait_outside_gpu_mutex_tests
            ARGS indirect ENV ANYPS5_NO_SHADER_CACHE=1)
    agc_add_env_test(agc_driver_compute_build_wait_outside_gpu_mutex_indirect_descriptor_heap agc_driver_compute_build_wait_outside_gpu_mutex_tests
            ARGS indirect ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1)
    agc_add_gpu_heap_test(agc_driver_compute_build_wait_outside_gpu_mutex)
endif()
