add_test_executable(agc_metadata_trust_tests EXCLUDE_FROM_ALL tests/MetadataTrust.cpp Graphics/src/MetadataTrust.cpp)
target_include_directories(agc_metadata_trust_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_metadata_trust_tests PRIVATE libc)
set_target_properties(agc_metadata_trust_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_metadata_trust_tests)

if(BUILD_TESTING)
    agc_add_default_test(agc_metadata_trust_tests)
endif()
