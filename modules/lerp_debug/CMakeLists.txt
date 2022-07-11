cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_debug)

add_library(lerp_debug INTERFACE)

target_include_directories(lerp_debug INTERFACE include)

target_sources(lerp_debug INTERFACE
    CMakeLists.txt

    debug.c
    include/lerp/debug.h
)

target_compile_options(lerp_debug INTERFACE
  -Wall 
)

endif()
