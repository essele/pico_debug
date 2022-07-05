cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_circ)

add_library(lerp_circ INTERFACE)

target_include_directories(lerp_circ INTERFACE include)

target_sources(lerp_circ INTERFACE
    CMakeLists.txt

    circ.c
    include/lerp/circ.h
)

target_compile_options(lerp_circ INTERFACE
  -Wall 
)

endif()
