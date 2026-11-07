option(ANYPS5_NO_SEMANTIC_INTERPOSITION "Compile the project's own targets with -fno-semantic-interposition on Linux" ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    if(ANYPS5_NO_SEMANTIC_INTERPOSITION)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fno-semantic-interposition>)
    endif()
endif()
