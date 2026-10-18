cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(sniffer C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sniffer main.c oc.c)
pico_generate_pio_header(sniffer ${CMAKE_CURRENT_LIST_DIR}/sampler.pio)
pico_generate_pio_header(sniffer ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
pico_generate_pio_header(sniffer ${CMAKE_CURRENT_LIST_DIR}/spi_tx.pio)
pico_generate_pio_header(sniffer ${CMAKE_CURRENT_LIST_DIR}/i2c_tx.pio)
target_link_libraries(sniffer pico_stdlib hardware_clocks hardware_vreg hardware_adc hardware_pio hardware_dma hardware_pwm pico_multicore)
if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()
target_compile_definitions(sniffer PRIVATE VOLT_MV=${VOLT_MV})
if(DEFINED MAX_MHZ)
    target_compile_definitions(sniffer PRIVATE MAX_MHZ=${MAX_MHZ})
endif()

pico_enable_stdio_usb(sniffer 1)
pico_enable_stdio_uart(sniffer 0)
pico_add_extra_outputs(sniffer)
