if(BUILD_TESTING)
    set_property(TEST agc_driver_draw_relocate_user_values APPEND PROPERTY ENVIRONMENT "APS5_POOL_RELOCATION=0")
endif()
