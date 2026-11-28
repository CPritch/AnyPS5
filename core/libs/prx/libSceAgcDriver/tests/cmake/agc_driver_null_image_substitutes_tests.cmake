if(BUILD_TESTING)
    set_tests_properties(agc_driver_null_image_substitutes PROPERTIES TIMEOUT 30)
    agc_add_env_test(agc_driver_null_image_substitutes_placeholder agc_driver_null_image_substitutes_tests
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_NO_NULL_DESCRIPTOR=1 TIMEOUT 30)
    agc_add_gpu_heap_test(agc_driver_null_image_substitutes)
    agc_add_env_test(agc_driver_null_image_substitutes_gpu_heap_placeholder agc_driver_null_image_substitutes_tests
            ENV ANYPS5_NO_SHADER_CACHE=1 APS5_NO_NULL_DESCRIPTOR=1 APS5_GPU_HEAP=1 APS5_VERIFY_GPU_HEAP=1 APS5_DRAW_PIPELINE=1 APS5_PIPELINED_DRAWS=32 APS5_PIPELINE_DISPATCH=1 TIMEOUT 30)
endif()
