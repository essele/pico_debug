cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_io)

add_library(lerp_io INTERFACE)

target_include_directories(lerp_io INTERFACE include)

target_sources(lerp_io INTERFACE
    CMakeLists.txt

    io.c
    include/lerp/io.h
)

target_compile_options(lerp_io INTERFACE
  -Wall 
)

target_link_libraries(lerp_io INTERFACE
    lerp_circ
    lerp_task
)

endif()
