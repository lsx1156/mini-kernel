cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(render C CXX ASM)
pico_sdk_init()

if(NOT DEFINED MAX_MHZ)
    set(MAX_MHZ 150)
endif()
if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()

add_executable(render main.c ../coremark/port/oc.c)
target_include_directories(render PRIVATE ../coremark/port)
target_compile_definitions(render PRIVATE MAX_MHZ=${MAX_MHZ} VOLT_MV=${VOLT_MV})
if(NOT DEFINED OPT)
    set(OPT -O2)
endif()
target_compile_options(render PRIVATE ${OPT})
target_link_libraries(render pico_stdlib hardware_vreg hardware_adc)

pico_enable_stdio_usb(render 1)
pico_enable_stdio_uart(render 0)
pico_add_extra_outputs(render)

# --- RISC-V workaround -------------------------------------------------------
# binutils 2.47 rejects any linker-script INCLUDE, so flatten the memmap tree
# into one self-contained script and link against that (same as coremark/).
if(PICO_PLATFORM MATCHES "riscv")
    set(_mm "${PICO_SDK_PATH}/src/rp2350/pico_platform/memmap_default.ld")
    set(_flat "${CMAKE_BINARY_DIR}/memmap_flat.ld")
    find_program(_PY python)
    execute_process(
        COMMAND ${_PY} "${CMAKE_CURRENT_LIST_DIR}/../tools/flatten_ld.py" "${_mm}" "${_flat}"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform/script_include"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/script_include"
                "${CMAKE_BINARY_DIR}/generated/pico_standard_link/render"
                "${CMAKE_BINARY_DIR}/pico-sdk/src/rp2_common/pico_standard_link"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link"
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out)
    message(STATUS "flatten_ld: ${_out} (rc=${_rv})")
    pico_set_linker_script(render "${_flat}")
endif()
