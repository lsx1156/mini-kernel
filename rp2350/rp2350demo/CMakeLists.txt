cmake_minimum_required(VERSION 3.13)

# ================================================================
#  RP2350 适配工程 (v4.0 预告)
# 
# 目标：适配 RP2350 (Cortex-M33 + RISC-V Hazard3) / Pico 2
# 
# 依赖：Pico SDK 2.0+ (支持 RP2350)
# ================================================================

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "PICO_SDK_PATH invalid (missing pico_sdk_init.cmake): ${PICO_SDK_PATH}")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2350demo C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ================================================================
# 1. mini-kernel 作为子目录引入
# ================================================================
set(OS_CONFIG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/os_config.h" CACHE FILEPATH "rp2350demo os_config.h")
set(MINIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mini-kernel" CACHE PATH "mini-kernel root dir")
if(NOT EXISTS "${MINIK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "mini-kernel CMakeLists.txt missing at ${MINIK_ROOT}/CMakeLists.txt")
endif()
add_subdirectory(${MINIK_ROOT} ${CMAKE_BINARY_DIR}/minik-build EXCLUDE_FROM_ALL)

# ================================================================
# 2. 编译选项 (Cortex-M33 + RISC-V Hazard3)
# ================================================================
# Cortex-M33 编译选项
set(DEMO_COMMON_FLAGS_M33
    "-mcpu=cortex-m33"
    "-mthumb"
    "-mfloat-abi=hard"
    "-mfpu=fpv5-sp-d16"
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-common"
    "-fmessage-length=0"
    "-Wall"
    "-Wextra"
    "-Wpedantic"
)

# RISC-V Hazard3 编译选项 (可选，双核异构)
set(DEMO_COMMON_FLAGS_RISCV
    "-march=rv32imac_zicsr_zifencei"
    "-mabi=ilp32"
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-common"
)

# 主要用 Cortex-M33 编译
string(REPLACE ";" " " DEMO_FLAGS_STR "${DEMO_COMMON_FLAGS_M33}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEMO_FLAGS_STR}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${DEMO_FLAGS_STR} -x assembler-with-cpp")

# ================================================================
# 3. RP2350 移植层目录
# ================================================================
set(RP2350_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rp2350_port")

# ================================================================
# 4. RP2350 移植层库
# ================================================================
file(GLOB RP2350_PORT_SRCS
    ${RP2350_PORT_DIR}/hal_port.c
    ${RP2350_PORT_DIR}/context_switch.S
    ${RP2350_PORT_DIR}/startup_rp2350.S
    ${RP2350_PORT_DIR}/rp2350_clk.c
    ${RP2350_PORT_DIR}/rp2350_gpio.c
    ${RP2350_PORT_DIR}/rp2350_usb.c
    ${RP2350_PORT_DIR}/rp2350_flash.c
    ${RP2350_PORT_DIR}/kernel/config_store.c
    ${RP2350_PORT_DIR}/kernel/diskio.c
    ${RP2350_PORT_DIR}/shell/bootscript.c
    ${RP2350_PORT_DIR}/shell/shell_fs.c
    ${RP2350_PORT_DIR}/shell/shell_ovclk.c
    ${RP2350_PORT_DIR}/shell/shell_plc.c
    ${RP2350_PORT_DIR}/ipc/shmem_ipc.c
    ${RP2350_PORT_DIR}/ipc/core1_worker.c
    ${RP2350_PORT_DIR}/ipc/shell_ipc.c
)

add_library(rp2350_port STATIC ${RP2350_PORT_SRCS})

# 移植层公共 include
target_include_directories(rp2350_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/shell
    ${RP2350_PORT_DIR}
    ${RP2350_PORT_DIR}/include
    ${RP2350_PORT_DIR}/shell
    ${RP2350_PORT_DIR}/kernel
)

# 依赖 Pico SDK
target_link_libraries(rp2350_port PRIVATE
    pico_stdlib
    hardware_uart
    hardware_timer
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_irq
    hardware_pwm
    hardware_adc
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_pll
    hardware_structs
    hardware_resets
    hardware_watchdog
    pico_multicore
    pico_bootrom
    hardware_trustzone
)

target_compile_options(rp2350_port PRIVATE ${DEMO_COMMON_FLAGS_M33})

# TrustZone 支持
target_compile_definitions(rp2350_port PRIVATE
    RP2350_HAS_TRUSTZONE=1
    PICO_RP2350=1
)

# ================================================================
# 5. FatFs 模块
# ================================================================
if(OS_CFG_FATFS)
    set(FATFS_SDK_SRC "${PICO_SDK_PATH}/lib/lib/fatfs/source")
    if(NOT (EXISTS "${FATFS_SDK_SRC}/ff.c" AND EXISTS "${FATFS_SDK_SRC}/ffunicode.c"))
        message(FATAL_ERROR "OS_CFG_FATFS=1 but Pico SDK FatFs source missing at ${FATFS_SDK_SRC}/ff.c")
    endif()
    set(FATFS_SHIM_DIR "${CMAKE_CURRENT_BINARY_DIR}/fatfs_shim_rp2350demo")
    file(MAKE_DIRECTORY "${FATFS_SHIM_DIR}")
    configure_file("${FATFS_SDK_SRC}/ff.h"         "${FATFS_SHIM_DIR}/ff.h"         COPYONLY)
    configure_file("${FATFS_SDK_SRC}/ff.c"         "${FATFS_SHIM_DIR}/ff.c"         COPYONLY)
    configure_file("${FATFS_SDK_SRC}/ffunicode.c"  "${FATFS_SHIM_DIR}/ffunicode.c"  COPYONLY)
    configure_file("${RP2350_PORT_DIR}/kernel/ffconf.h" "${FATFS_SHIM_DIR}/ffconf.h" COPYONLY)

    add_library(fatfs_module STATIC
        "${FATFS_SHIM_DIR}/ff.c"
        "${FATFS_SHIM_DIR}/ffunicode.c"
    )
    target_include_directories(fatfs_module PUBLIC
        "${FATFS_SHIM_DIR}"
        "${RP2350_PORT_DIR}/kernel"
        "${FATFS_SDK_SRC}"
        "${MINIK_ROOT}/include"
    )
    target_compile_options(fatfs_module PRIVATE ${DEMO_COMMON_FLAGS_M33})
    target_link_libraries(fatfs_module PUBLIC rp2350_port)
    target_include_directories(rp2350_port PRIVATE
        "${FATFS_SHIM_DIR}"
        "${FATFS_SDK_SRC}"
    )
endif()

# ================================================================
# 6. 应用可执行目标
# ================================================================
add_executable(${PROJECT_NAME}
    src/main.c
    ${RP2350_PORT_DIR}/demo/demo_app.c
)

target_compile_options(${PROJECT_NAME} PRIVATE ${DEMO_COMMON_FLAGS_M33})
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
    PICO_TIME_DEFAULT_ALARM_POOL_HARDWARE_ALARM_NUM=3
    PICO_RP2350=1
)

# USB Composite (CDC+MSC)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    CFG_TUD_MSC=1
    CFG_TUD_MSC_EP_BUFSIZE=512
)
if(TARGET tinyusb_device_common)
    target_compile_definitions(tinyusb_device_common INTERFACE
        CFG_TUD_MSC=1
        CFG_TUD_MSC_EP_BUFSIZE=512
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/shell
    ${RP2350_PORT_DIR}
    ${RP2350_PORT_DIR}/include
    ${RP2350_PORT_DIR}/shell
    ${RP2350_PORT_DIR}/kernel
)

if(TARGET fatfs_module)
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${FATFS_SHIM_DIR}"
        "${FATFS_SDK_SRC}"
    )
endif()

# ================================================================
# 7. 链接
# ================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    "-Wl,--start-group"
    rp2350_port
    kernel_core
    shell_module
    fatfs_module
    "-Wl,--end-group"
    pico_stdlib
    hardware_uart
    hardware_timer
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_irq
    hardware_pwm
    hardware_adc
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_pll
    pico_multicore
    hardware_watchdog
    pico_bootrom
)

# ================================================================
# 8. Linker 脚本 (RP2350 内存映射不同)
# ================================================================
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/rp2350.ld)

# 编译选项
target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv5-sp-d16
    --specs=nosys.specs
    --specs=nano.specs
    -Wl,--gc-sections
)

# ================================================================
# 9. USB 描述符
# ================================================================
if(TARGET pico_stdio_usb)
    get_target_property(_STDIO_USB_IFACE_SRCS pico_stdio_usb INTERFACE_SOURCES)
    if(_STDIO_USB_IFACE_SRCS)
        set(_STDIO_USB_FILTERED "")
        foreach(_s ${_STDIO_USB_IFACE_SRCS})
            get_filename_component(_s_name "${_s}" NAME)
            if(NOT "${_s_name}" STREQUAL "stdio_usb_descriptors.c")
                list(APPEND _STDIO_USB_FILTERED "${_s}")
            endif()
        endforeach()
        set_target_properties(pico_stdio_usb PROPERTIES
            INTERFACE_SOURCES "${_STDIO_USB_FILTERED}"
        )
        message(STATUS "[rp2350demo] Removed stdio_usb_descriptors.c (using custom Composite descriptors)")
    endif()
endif()

pico_add_extra_outputs(${PROJECT_NAME})

# ================================================================
# 10. 体积检查
# ================================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DCMAKE_SOURCE_DIR=${MINIK_ROOT}
        -DPROJECT_NAME=${PROJECT_NAME}
        -DMAX_FLASH_KB=512  # RP2350 有 4MB Flash
        -DMAX_RAM_KB=520    # RP2350 有 520KB RAM
        -P ${MINIK_ROOT}/cmake/check_size.cmake
    COMMENT "Checking rp2350demo firmware size..."
    VERBATIM
)

# ================================================================
# 11. RISC-V Hazard3 核心支持 (可选)
# ================================================================
# 如果需要在 RISC-V 核心上运行部分代码
# add_library(rp2350_riscv_port STATIC ...)
# target_compile_options(rp2350_riscv_port PRIVATE ${DEMO_COMMON_FLAGS_RISCV})
# target_link_libraries(rp2350_riscv_port ...)
# pico_set_linker_script(rp2350_riscv_port ${CMAKE_CURRENT_SOURCE_DIR}/riscv.ld)