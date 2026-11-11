if(BUILD_TESTING)
    add_test(NAME agc_driver_dcc_keys_first_write_readback_waits COMMAND agc_driver_dcc_keys_first_write_tests)
    set_tests_properties(agc_driver_dcc_keys_first_write_readback_waits PROPERTIES ENVIRONMENT "APS5_NO_DRAW_READBACK_WAITS=0;ANYPS5_NO_SHADER_CACHE=1;APS5_FAST_F16=0;APS5_FAST_F32=0" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_dcc_keys_rewrite_after_memo COMMAND agc_driver_dcc_keys_first_write_tests --rewrite-after-memo)
    add_test(NAME agc_driver_dcc_keys_rewrite_after_refresh_proof COMMAND agc_driver_dcc_keys_first_write_tests --rewrite-after-refresh-proof)
    add_test(NAME agc_driver_dcc_keys_rewrite_after_epoch_proof COMMAND agc_driver_dcc_keys_first_write_tests --rewrite-after-epoch-proof)
    add_test(NAME agc_driver_dcc_keys_partial_rewrite_keeps_store COMMAND agc_driver_dcc_keys_first_write_tests --partial-rewrite-keeps-store)
    add_test(NAME agc_driver_dcc_keys_write_beside_keys_keeps_store COMMAND agc_driver_dcc_keys_first_write_tests --write-beside-keys-keeps-store)
    add_test(NAME agc_driver_dcc_keys_rewrite_to_register_clear COMMAND agc_driver_dcc_keys_first_write_tests --rewrite-to-register-clear)
    set_tests_properties(agc_driver_dcc_keys_rewrite_after_memo agc_driver_dcc_keys_rewrite_after_refresh_proof agc_driver_dcc_keys_partial_rewrite_keeps_store agc_driver_dcc_keys_write_beside_keys_keeps_store agc_driver_dcc_keys_rewrite_to_register_clear PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_HOST_IMPORT=1;APS5_NO_DRAW_PIPELINE=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_dcc_keys_rewrite_after_epoch_proof PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_HOST_IMPORT=1;APS5_DRAW_PIPELINE=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_dcc_keys_first_write_readback_waits agc_driver_dcc_keys_rewrite_after_memo agc_driver_dcc_keys_rewrite_after_refresh_proof agc_driver_dcc_keys_rewrite_after_epoch_proof agc_driver_dcc_keys_partial_rewrite_keeps_store agc_driver_dcc_keys_write_beside_keys_keeps_store agc_driver_dcc_keys_rewrite_to_register_clear PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
