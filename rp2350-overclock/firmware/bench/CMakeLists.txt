cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(bench C CXX ASM)
pico_sdk_init()

# ALLOW_OV=1 lets the ladder go past the DVDD 1.16 V spec ceiling (1.20/1.25 V).
if(NOT DEFINED ALLOW_OV)
    set(ALLOW_OV 0)
endif()

add_executable(bench main.c)
target_link_libraries(bench pico_stdlib hardware_vreg hardware_adc)
target_compile_definitions(bench PRIVATE ALLOW_OV=${ALLOW_OV})

# FRESH=1 wipes the watchdog scratch registers on boot. They survive a UF2 flash,
# so a stale HANG flag from the previous firmware would otherwise skip the point
# we actually want to measure. Never pass FRESH through CMAKE_C_FLAGS: that
# replaces CMAKE_C_FLAGS_INIT and silently drops -mcpu=cortex-m33.
if(NOT DEFINED FRESH)
    set(FRESH 0)
endif()
if(FRESH)
    target_compile_definitions(bench PRIVATE FRESH=1)
endif()

if(NOT DEFINED SOAK_S)
    set(SOAK_S 30)
endif()
target_compile_definitions(bench PRIVATE SOAK_S=${SOAK_S})

pico_enable_stdio_usb(bench 1)
pico_enable_stdio_uart(bench 0)
pico_add_extra_outputs(bench)
