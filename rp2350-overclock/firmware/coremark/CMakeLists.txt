cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

# binutils 2.47 (MSYS2 riscv32-unknown-elf) rejects the linker script being passed
# twice; memmap_*.ld already INCLUDEs default_locations.ld.
set(PICO_SKIP_DEFAULT_LOCATIONS_SCRIPT 1)

project(cm C CXX ASM)
pico_sdk_init()

if(NOT DEFINED MAX_MHZ)
    set(MAX_MHZ 150)
endif()
if(NOT DEFINED VOLT_MV)
    set(VOLT_MV 1100)
endif()
if(NOT DEFINED ITERATIONS)
    set(ITERATIONS 5000)
endif()

set(CM ${CMAKE_CURRENT_LIST_DIR}/../coremark-main)

add_executable(cm
    port/main_wrap.c
    port/core_portme.c
    port/oc.c
    ${CM}/core_list_join.c
    ${CM}/core_main.c
    ${CM}/core_matrix.c
    ${CM}/core_state.c
    ${CM}/core_util.c
)

target_include_directories(cm PRIVATE ${CM} ${CMAKE_CURRENT_LIST_DIR}/port)
target_compile_definitions(cm PRIVATE
    PERFORMANCE_RUN=1
    ITERATIONS=${ITERATIONS}
    FLAGS_STR="-O2"
    MAX_MHZ=${MAX_MHZ}
    VOLT_MV=${VOLT_MV}
)
target_compile_options(cm PRIVATE -O2)
target_link_libraries(cm pico_stdlib hardware_vreg hardware_adc)

# core_main.c owns the real main(); rename it so our launcher can drive 3 passes.
set_source_files_properties(${CM}/core_main.c PROPERTIES COMPILE_DEFINITIONS "main=coremark_main")

pico_enable_stdio_usb(cm 1)
pico_enable_stdio_uart(cm 0)
pico_add_extra_outputs(cm)

# --- RISC-V workaround -------------------------------------------------------
# binutils 2.47 (msys2 riscv32-unknown-elf) rejects any linker script INCLUDE
# ("file 'x (y)' appears multiple times"), even a two-file chain. Flatten the
# memmap include tree into one self-contained script and link against that.
if(PICO_PLATFORM MATCHES "riscv")
    set(_mm "${PICO_SDK_PATH}/src/rp2350/pico_platform/memmap_default.ld")
    set(_flat "${CMAKE_BINARY_DIR}/memmap_flat.ld")
    find_program(_PY python)
    execute_process(
        COMMAND ${_PY} "${CMAKE_CURRENT_LIST_DIR}/../tools/flatten_ld.py" "${_mm}" "${_flat}"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform"
                "${PICO_SDK_PATH}/src/rp2350/pico_platform/script_include"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/script_include"
                "${CMAKE_BINARY_DIR}/generated/pico_standard_link/cm"
                "${CMAKE_BINARY_DIR}/pico-sdk/src/rp2_common/pico_standard_link"
                "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link"
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out)
    message(STATUS "flatten_ld: ${_out} (rc=${_rv})")
    pico_set_linker_script(cm "${_flat}")
endif()
