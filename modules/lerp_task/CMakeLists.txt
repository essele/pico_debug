cmake_minimum_required(VERSION 3.13)

if(NOT TARGET lerp_task)

add_library(lerp_task INTERFACE)

target_include_directories(lerp_task INTERFACE include)

target_sources(lerp_task INTERFACE
    CMakeLists.txt

    task.c
    include/lerp/task.h
    list.h
    plat_rp2040.c
)

target_compile_options(lerp_task INTERFACE
  -Wall 
)
target_link_libraries(lerp_task INTERFACE
  pico_stdlib
  hardware_pio
  hardware_i2c
  hardware_exception
)


endif()
