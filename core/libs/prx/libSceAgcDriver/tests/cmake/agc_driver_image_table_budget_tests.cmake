if(BUILD_TESTING)
    agc_add_env_test(agc_driver_image_table_budget_gpu_heap agc_driver_image_table_budget_tests TIMEOUT 60
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1)
    agc_add_env_test(agc_driver_image_table_budget_deferred agc_driver_image_table_budget_tests TIMEOUT 60
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_GPU_HEAP=1 APS5_TABLE_IMAGE_BUDGET=1)
    set_tests_properties(agc_driver_image_table_budget agc_driver_image_table_budget_gpu_heap agc_driver_image_table_budget_deferred PROPERTIES TIMEOUT 60
            FAIL_REGULAR_EXPRESSION "SYNC-HAZARD-|VUID-|was not found but was requested by env var VK_INSTANCE_LAYERS")
endif()
