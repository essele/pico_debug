cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_interact)

add_library(lerp_interact INTERFACE)

target_include_directories(lerp_interact INTERFACE include)

target_sources(lerp_interact INTERFACE
    CMakeLists.txt

    interact.c
    include/lerp/interact.h
)

target_compile_options(lerp_interact INTERFACE
  -Wall 
)
target_link_libraries(lerp_interact INTERFACE
  lerp_circ
  lerp_io
  lerp_task
)


endif()
