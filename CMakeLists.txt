cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)
##include(pico_extras_import.cmake)

project(pico-debug_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(FAMILY rp2040)
#set(BOARD pico_sdk)
set(PICO_BOARD pico_w)
set(TINYUSB_FAMILY_PROJECT_NAME_PREFIX "tinyusb_dev_")

pico_sdk_init()

add_executable(pico-debug
    main.c

    swd.c swd.h swd.pio
    adi.c adi.h

    id_usb.c
    tusb_config.h
    lwipopts.h

    flash.c flash.h
    uart.c uart.h
    wifi.c wifi.h

    gdb.c gdb.h
    breakpoint.c breakpoint.h

    prof.c

    cmdline.c cmdline.h
    utils.c utils.h

    config/config.h
    config/config.c
    
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
add_subdirectory(modules/lerp_io)
add_subdirectory(modules/lerp_interact)
add_subdirectory(modules/lerp_flash)
add_subdirectory(modules/lerp_tokeniser)

target_compile_definitions(pico-debug PRIVATE
  # We don't want the default alarm pool (and the IRQ's associated!)
  PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1

  # Main pins for the SWD connections
  PIN_SWDCLK=2
  PIN_SWDIO=3

  # Pins for the UART connection
  PIN_UART_TX=4
  PIN_UART_RX=5
  UART_INTERFACE=uart1
  UART_BAUD=115200

  # Which CDC port to use for GDB
  GDB_CDC=0
  GDB_TCP=3333

  # Which CDC port to use for UART
  UART_CDC=1
  UART_TCP=3334

  # Which CDC port to use for CMD interface
  CMD_CDC=2
  CMD_TCP=3335

  PROF_CDC=0
  PROF_TCP=0

  # Debug on CDC2
  DEBUG_CDC=2
  DEBUG_BUF=1
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

  # Support LWIP in polling mode...
  pico_cyw43_arch_lwip_poll

  lerp_task
  lerp_circ
  lerp_io
  lerp_debug
  lerp_interact
  lerp_flash
  lerp_tokeniser
)
