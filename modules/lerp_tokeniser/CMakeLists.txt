cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_tokeniser)

add_library(lerp_tokeniser INTERFACE)

target_include_directories(lerp_tokeniser INTERFACE include)

target_sources(lerp_tokeniser INTERFACE
    CMakeLists.txt

    tokeniser.c
    include/lerp/tokeniser.h
)

target_compile_options(lerp_tokeniser INTERFACE
  -Wall 
)

target_link_libraries(lerp_tokeniser INTERFACE
    lerp_circ
)

endif()
