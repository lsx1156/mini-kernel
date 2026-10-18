cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(dmabench C CXX ASM)
pico_sdk_init()

add_executable(dmabench main.c oc.c)
target_link_libraries(dmabench
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_irq
    hardware_clocks
    hardware_vreg
    hardware_adc
)
target_compile_options(dmabench PRIVATE -O2)

if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()
target_compile_definitions(dmabench PRIVATE VOLT_MV=${VOLT_MV})
if(DEFINED MAX_MHZ)
    target_compile_definitions(dmabench PRIVATE MAX_MHZ=${MAX_MHZ})
endif()

pico_enable_stdio_usb(dmabench 1)
pico_enable_stdio_uart(dmabench 0)
pico_add_extra_outputs(dmabench)

# --- RISC-V workaround -------------------------------------------------------
# binutils 2.47 rejects linker-script INCLUDE; flatten the memmap tree instead.
if(PICO_PLATFORM MATCHES "riscv")
    set(_mm "${PICO_SDK_PATH}/src/rp2350/pico_platform/memmap_default.ld")
    set(_flat "${CMAKE_BINARY_DIR}/memmap_flat.ld")
    find_program(_PY python)
    execute_process(
        COMMAND ${_PY} "${CMAKE_CURRENT_LIST_DIR}/../tools/flatten_ld.py" "${_mm}" "${_flat}"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform/script_include"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/script_include"
                "${CMAKE_BINARY_DIR}/generated/pico_standard_link/dmabench"
                "${CMAKE_BINARY_DIR}/pico-sdk/src/rp2_common/pico_standard_link"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link"
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out)
    message(STATUS "flatten_ld: ${_out} (rc=${_rv})")
    pico_set_linker_script(dmabench "${_flat}")
endif()
