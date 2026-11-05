if(BUILD_TESTING AND UNIX AND NOT APPLE)
    set(agcInputFetchBenchHeaders)
    foreach(agcInputFetchStage IN ITEMS vert frag)
        string(TOUPPER "${agcInputFetchStage}" agcInputFetchSymbol)
        set(agcInputFetchSpv ${agcTestShaderGeneratedRoot}/InputFetchBench_${agcInputFetchStage}.spv)
        set(agcInputFetchHeader ${agcTestShaderGeneratedRoot}/InputFetchBench_${agcInputFetchStage}_spv.h)
        add_custom_command(
                OUTPUT "${agcInputFetchHeader}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
                COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcInputFetchSpv}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/InputFetchBench.${agcInputFetchStage}"
                COMMAND ${CMAKE_COMMAND} -DINPUT=${agcInputFetchSpv} -DOUTPUT=${agcInputFetchHeader}
                -DSYMBOL=INPUT_FETCH_BENCH_${agcInputFetchSymbol}_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/InputFetchBench.${agcInputFetchStage}" glslang-standalone
                VERBATIM
        )
        list(APPEND agcInputFetchBenchHeaders ${agcInputFetchHeader})
    endforeach()
    add_executable(agc_input_fetch_bench EXCLUDE_FROM_ALL tests/InputFetchBench.cpp ${agcInputFetchBenchHeaders})
    target_include_directories(agc_input_fetch_bench PRIVATE ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR} ${agcTestShaderGeneratedRoot})
    target_compile_definitions(agc_input_fetch_bench PRIVATE VK_NO_PROTOTYPES)
    target_link_libraries(agc_input_fetch_bench PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(agc_input_fetch_bench PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
endif()
