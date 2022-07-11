cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(pico-debug_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(FAMILY rp2040)
set(BOARD pico_sdk)
set(TINYUSB_FAMILY_PROJECT_NAME_PREFIX "tinyusb_dev_")

pico_sdk_init()

add_executable(pico-debug
    main.c

    swd.c swd.h swd.pio
    adi.c adi.h

    id_usb.c
    tusb_config.h

    flash.c flash.h
    io.c io.h

    gdb.c gdb.h
    breakpoint.c breakpoint.h

    utils.c utils.h
    
    filedata.c filedata.h

    files/rp2040_features.xml
    files/rp2040_memory_map.xml
    files/rp2040_threads.xml
)

#
# Now make sure we can generate the file data
#
file (GLOB files ${CMAKE_CURRENT_LIST_DIR}/files/* )

add_custom_target(fgen DEPENDS filedata.c filedata.h)
add_custom_command(OUTPUT filedata.c filedata.h
  DEPENDS ${files}
  COMMAND ${CMAKE_CURRENT_LIST_DIR}/filegen ${CMAKE_CURRENT_LIST_DIR}/files filedata.c filedata.h
)
set_source_files_properties(filedata.c filedata.h PROPERTIES
  GENERATED TRUE)
add_dependencies(pico-debug fgen)


pico_generate_pio_header(pico-debug ${CMAKE_CURRENT_LIST_DIR}/swd.pio)

#
# Make sure TinyUSB can find tusb_config.h
#
target_include_directories(pico-debug PUBLIC
        ${CMAKE_CURRENT_LIST_DIR})


# Add all of our modules
add_subdirectory(modules/lerp_task)
add_subdirectory(modules/lerp_circ)
add_subdirectory(modules/lerp_debug)

target_compile_definitions(pico-debug PRIVATE
  # We don't want the default alarm pool (and the IRQ's associated!)
  PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1

  # Main pins for the SWD connections
  PIN_SWDCLK=2
  PIN_SWDIO=3

  # Pins for the UART connection
  PIN_UART_TX=4
  PIN_UART_RX=5
  UART_INTERFACE=uart0
  UART_BAUD=115200

  # Debug on CDC2
  DEBUG_CDC2=1
)

target_compile_options(pico-debug PUBLIC
  -Wall
)


#pico_enable_stdio_usb(pico-debug 0)
#pico_enable_stdio_uart(pico-debug 0)
pico_add_extra_outputs(pico-debug)

#pico_generate_pio_header(pico-debug ${CMAKE_CURRENT_LIST_DIR}/gpib/gpib.pio)
#pico_generate_pio_header(pico-debug ${CMAKE_CURRENT_LIST_DIR}/monitor.pio)


target_link_libraries(pico-debug 
  pico_stdlib 
  tinyusb_device 
  tinyusb_board
  pico_unique_id
  hardware_pio

  lerp_task
  lerp_circ
  lerp_debug
)
