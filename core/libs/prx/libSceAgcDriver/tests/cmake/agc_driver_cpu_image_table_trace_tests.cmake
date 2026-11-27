if(BUILD_TESTING)
    agc_add_env_test(agc_driver_cpu_image_table_trace_gpu_heap agc_driver_cpu_image_table_trace_tests TIMEOUT 30
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1)
    agc_add_env_test(agc_driver_cpu_image_table_trace_gpu_path agc_driver_cpu_image_table_trace_tests TIMEOUT 30
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1 APS5_GPU_PATH=1)
    set_tests_properties(agc_driver_cpu_image_table_trace agc_driver_cpu_image_table_trace_gpu_heap agc_driver_cpu_image_table_trace_gpu_path PROPERTIES TIMEOUT 30
            FAIL_REGULAR_EXPRESSION "SYNC-HAZARD-|VUID-|was not found but was requested by env var VK_INSTANCE_LAYERS")
endif()
