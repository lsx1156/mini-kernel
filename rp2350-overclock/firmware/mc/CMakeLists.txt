cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(mc C CXX ASM)
pico_sdk_init()

if(NOT DEFINED MAX_MHZ)
    set(MAX_MHZ 150)
endif()
if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()
if(NOT DEFINED CORES)
    set(CORES 2)
endif()

add_executable(mc main.c ../coremark/port/oc.c)
target_include_directories(mc PRIVATE ../coremark/port)
target_compile_definitions(mc PRIVATE
    MAX_MHZ=${MAX_MHZ} VOLT_MV=${VOLT_MV} CORES=${CORES})
target_compile_options(mc PRIVATE -O2)
if(KERNEL_IN_RAM)
    target_compile_definitions(mc PRIVATE KERNEL_IN_RAM=1)
endif()
target_link_libraries(mc pico_stdlib pico_multicore hardware_vreg hardware_adc)

pico_enable_stdio_usb(mc 1)
pico_enable_stdio_uart(mc 0)
pico_add_extra_outputs(mc)

# See coremark/CMakeLists.txt: binutils 2.47 rejects linker-script INCLUDE.
if(PICO_PLATFORM MATCHES "riscv")
    set(_mm "${PICO_SDK_PATH}/src/rp2350/pico_platform/memmap_default.ld")
    set(_flat "${CMAKE_BINARY_DIR}/memmap_flat.ld")
    find_program(_PY python)
    execute_process(
        COMMAND ${_PY} "${CMAKE_CURRENT_LIST_DIR}/../tools/flatten_ld.py" "${_mm}" "${_flat}"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform/script_include"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/script_include"
                "${CMAKE_BINARY_DIR}/generated/pico_standard_link/mc"
                "${CMAKE_BINARY_DIR}/pico-sdk/src/rp2_common/pico_standard_link"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link"
        RESULT_VARIABLE _rv OUTPUT_QUIET)
    pico_set_linker_script(mc "${_flat}")
endif()
