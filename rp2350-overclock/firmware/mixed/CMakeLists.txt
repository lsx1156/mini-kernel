cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(mixed C CXX ASM)
pico_sdk_init()

if(NOT DEFINED MAX_MHZ)
    set(MAX_MHZ 150)
endif()
if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()

add_executable(mixed main.c payload_blob.c ../coremark/port/oc.c)
target_include_directories(mixed PRIVATE ../coremark/port)
target_compile_definitions(mixed PRIVATE MAX_MHZ=${MAX_MHZ} VOLT_MV=${VOLT_MV})
target_compile_options(mixed PRIVATE -O2)
target_link_libraries(mixed pico_stdlib pico_multicore hardware_vreg hardware_adc)

pico_enable_stdio_usb(mixed 1)
pico_enable_stdio_uart(mixed 0)
pico_add_extra_outputs(mixed)
