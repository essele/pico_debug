cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_flash)

add_library(lerp_flash INTERFACE)

target_include_directories(lerp_flash INTERFACE include)

target_sources(lerp_flash INTERFACE
    CMakeLists.txt

    flash.c
    include/lerp/flash.h
)

target_compile_options(lerp_flash INTERFACE
  -Wall 
)

target_link_libraries(lerp_flash INTERFACE
)

endif()
