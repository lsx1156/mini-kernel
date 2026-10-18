cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(piobench C CXX ASM)
pico_sdk_init()

add_executable(piobench main.c oc.c)
target_link_libraries(piobench
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_gpio
    hardware_clocks
    hardware_vreg
    hardware_adc
)
target_compile_options(piobench PRIVATE -O2)

if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()
target_compile_definitions(piobench PRIVATE VOLT_MV=${VOLT_MV})

if(DEFINED MAX_MHZ)
    target_compile_definitions(piobench PRIVATE MAX_MHZ=${MAX_MHZ})
endif()

pico_enable_stdio_usb(piobench 1)
pico_enable_stdio_uart(piobench 0)
pico_add_extra_outputs(piobench)
