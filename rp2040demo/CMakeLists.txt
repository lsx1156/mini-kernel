cmake_minimum_required(VERSION 3.13)

# ================================================================
#  RP2040 Demo 独立工程（v2.5 两版本分离后）
#
#  目录设计（project/ 作为 git 单仓根）：
#    rp2040demo/            应用层（本工程）
#      ├ src/main.c         main()，调用 kernel_main() 起内核
#      ├ os_config.h        本工程专属裁剪配置（启用心全部 RP2040 功能）
#      ├ rp2040_port/       RP2040 移植层（hal_port / context_switch / msc /
#      │                      config_store / sysclk / FatFs diskio / bootscript /
#      │                      shell_fs / shell_mcore / shell_ovclk / demo_app）
#      └ build.bat          一键构建脚本
#    mini-kernel/           纯净内核库（平台无关，只导出 kernel_core / shell_module）
#
#  PICO SDK 初始化必须在 project() 之前 — 这是 Pico SDK 官方强制顺序。
# ================================================================

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "PICO_SDK_PATH invalid (missing pico_sdk_init.cmake): ${PICO_SDK_PATH}")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2040demo C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ================================================================
# 1. mini-kernel 作为子目录引入（只构建静态库，不构建其内置可执行）
#    先把 OS_CONFIG_HEADER 指向本工程专属 os_config.h，替换内核库默认配置。
# ================================================================
set(OS_CONFIG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/os_config.h" CACHE FILEPATH "rp2040demo os_config.h")
set(MINIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mini-kernel"
    CACHE PATH "mini-kernel root dir")
if(NOT EXISTS "${MINIK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "mini-kernel CMakeLists.txt missing at ${MINIK_ROOT}/CMakeLists.txt")
endif()
add_subdirectory(${MINIK_ROOT} ${CMAKE_BINARY_DIR}/minik-build EXCLUDE_FROM_ALL)

# ================================================================
# 2. 编译选项（Cortex-M0+ Thumb）
# ================================================================
set(DEMO_COMMON_FLAGS
    "-mcpu=cortex-m0plus"
    "-mthumb"
    "-mfloat-abi=soft"
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-common"
    "-fmessage-length=0"
    "-Wall"
    "-Wextra"
    "-Wpedantic"
)
string(REPLACE ";" " " DEMO_FLAGS_STR "${DEMO_COMMON_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEMO_FLAGS_STR}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${DEMO_FLAGS_STR} -x assembler-with-cpp")

# ================================================================
# 3. RP2040 移植层目录
# ================================================================
set(RP2040_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rp2040_port")

# ================================================================
# 4. RP2040 移植层库（HAL + 上下文切换 + MSC + 固化配置 + FatFs diskio +
#    bootscript + shell_fs/mcore/ovclk + demo_app）
# ================================================================
file(GLOB RP2040_PORT_SRCS
    ${RP2040_PORT_DIR}/hal_port.c
    ${RP2040_PORT_DIR}/context_switch.S
    ${RP2040_PORT_DIR}/msc_blockdev.c
    ${RP2040_PORT_DIR}/msc_usb.c
    ${RP2040_PORT_DIR}/kernel/config_store.c
    ${RP2040_PORT_DIR}/kernel/diskio.c
    ${RP2040_PORT_DIR}/shell/bootscript.c
    ${RP2040_PORT_DIR}/shell/shell_fs.c
    ${RP2040_PORT_DIR}/shell/shell_mcore.c
    ${RP2040_PORT_DIR}/shell/shell_ovclk.c
    ${RP2040_PORT_DIR}/shell/shell_plc.c
    ${RP2040_PORT_DIR}/ipc/shmem_ipc.c
    ${RP2040_PORT_DIR}/ipc/core1_worker.c
    ${RP2040_PORT_DIR}/ipc/shell_ipc.c
    ${RP2040_PORT_DIR}/shell/plc_core.c
)
# 【v2.5.1 关键修复】demo_app.c 不进静态库、直接编进可执行目标：
#   kernel.c 对 demo_app_init 只有 weak 引用 —— weak 引用不会从静态库拉取
#   归档成员，demo_app.o 会被链接器丢弃，连带 shell.o（仅 demo_app.o 强引用
#   shell_start）也被丢弃 → 固件里根本没有 shell 任务 → USB/TTL 全部无法输入。
#   编进 exe 后 demo_app_init 强定义存在，并强引用 shell_start 拉入 shell_module。
add_library(rp2040_port STATIC ${RP2040_PORT_SRCS})

# 移植层公共 include（内核头 + 移植层自身头 + SDK 头）
#   【关键】rp2040demo/os_config.h（端口配置）必须在 include path 最前，
#   否则 #include "os_config.h" 会命中 mini-kernel/include/os_config.h
#   （纯库配置：PERIPH_SERVICE=0/FATFS=0），导致 HAL_FLASH_SECTOR_SIZE 等未定义。
target_include_directories(rp2040_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/shell
    ${RP2040_PORT_DIR}
    ${RP2040_PORT_DIR}/include
    ${RP2040_PORT_DIR}/shell
    ${RP2040_PORT_DIR}/kernel
)
# 直接依赖内核库（继承其 PUBLIC include）与 Pico SDK（继承 include 路径）
target_link_libraries(rp2040_port PRIVATE
    pico_stdlib
    hardware_uart
    hardware_timer
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_irq
)
target_compile_options(rp2040_port PRIVATE ${DEMO_COMMON_FLAGS})
if(OS_CFG_DEFINES)
    target_compile_definitions(rp2040_port PRIVATE ${OS_CFG_DEFINES})
endif()
# MSC/USB 需要 TinyUSB 头文件（msc_usb.c 引用 tusb.h）
target_compile_definitions(rp2040_port PRIVATE
    CFG_TUD_MSC=1
    CFG_TUD_MSC_EP_BUFSIZE=512
)
target_include_directories(rp2040_port PRIVATE
    "${PICO_SDK_PATH}/lib/tinyusb/src"
    "${PICO_SDK_PATH}/lib/tinyusb/src/class/cdc"
    "${PICO_SDK_PATH}/lib/tinyusb/src/class/msc"
    "${PICO_SDK_PATH}/lib/tinyusb/src/common"
    "${PICO_SDK_PATH}/lib/tinyusb/src/device"
    "${PICO_SDK_PATH}/lib/tinyusb/src/osal"
    "${PICO_SDK_PATH}/src/rp2_common/pico_stdio_usb/include"
    "${PICO_SDK_PATH}/src/rp2_common/tinyusb"
    "${CMAKE_BINARY_DIR}/generated/pico-sdk"
)

# 让内核库（kernel_core / shell_module）在编译时也能找到移植层头
#   · kernel.c 在 OS_CFG_OVCLK=1 时 #include "config_store.h"/"sysclk.h"（在 rp2040_port/include）
#   · shell.c 在 OS_CFG_PORT_RP2040=1 时 #include "bootscript.h"（在 rp2040_port/shell）
target_include_directories(kernel_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RP2040_PORT_DIR}/include
)
target_include_directories(shell_module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RP2040_PORT_DIR}/shell
)
# shell_module 需要 rp2040_port（shell_ovclk/mcore 在其内）以实现链接
target_link_libraries(shell_module PUBLIC rp2040_port)

# ================================================================
# 5. FatFs 模块（Elm FatFs，来自 Pico SDK + 我们的 ffconf.h/diskio.h）
#    用 shim 目录技巧让 SDK 的 ff.c/#include "ffconf.h" 命中我们的配置
#    （FF_USE_MKFS=1，diskio.c 已在 rp2040_port 库中）。
# ================================================================
if(OS_CFG_FATFS)
    set(FATFS_SDK_SRC "${PICO_SDK_PATH}/lib/lib/fatfs/source")
    if(NOT (EXISTS "${FATFS_SDK_SRC}/ff.c" AND EXISTS "${FATFS_SDK_SRC}/ffunicode.c"))
        message(FATAL_ERROR "OS_CFG_FATFS=1 but Pico SDK FatFs source missing at ${FATFS_SDK_SRC}/ff.c")
    endif()
    set(FATFS_SHIM_DIR "${CMAKE_CURRENT_BINARY_DIR}/fatfs_shim_rp2040demo")
    file(MAKE_DIRECTORY "${FATFS_SHIM_DIR}")
    configure_file("${FATFS_SDK_SRC}/ff.h"         "${FATFS_SHIM_DIR}/ff.h"         COPYONLY)
    configure_file("${FATFS_SDK_SRC}/ff.c"         "${FATFS_SHIM_DIR}/ff.c"         COPYONLY)
    configure_file("${FATFS_SDK_SRC}/ffunicode.c"  "${FATFS_SHIM_DIR}/ffunicode.c"  COPYONLY)
    configure_file("${RP2040_PORT_DIR}/kernel/ffconf.h" "${FATFS_SHIM_DIR}/ffconf.h" COPYONLY)

    add_library(fatfs_module STATIC
        "${FATFS_SHIM_DIR}/ff.c"
        "${FATFS_SHIM_DIR}/ffunicode.c"
    )
    target_include_directories(fatfs_module PUBLIC
        "${FATFS_SHIM_DIR}"
        "${RP2040_PORT_DIR}/kernel"
        "${FATFS_SDK_SRC}"
        "${MINIK_ROOT}/include"
    )
    target_compile_options(fatfs_module PRIVATE ${DEMO_COMMON_FLAGS})
    if(OS_CFG_DEFINES)
        target_compile_definitions(fatfs_module PRIVATE ${OS_CFG_DEFINES})
    endif()
    # shell_fs.c（在 rp2040_port 里）需要 fatfs 头与符号
    target_link_libraries(fatfs_module PUBLIC rp2040_port)
    target_include_directories(rp2040_port PRIVATE
        "${FATFS_SHIM_DIR}"
        "${FATFS_SDK_SRC}"
    )
endif()

# ================================================================
# 6. 应用可执行目标
# ================================================================
add_executable(${PROJECT_NAME}
    src/main.c
    ${RP2040_PORT_DIR}/demo/demo_app.c   # v2.5.1：直接编进 exe（见上方 rp2040_port 库注释）
    ${RP2040_PORT_DIR}/demo/demo_show.c  # v2.7：展示/稳定性验证（OLED FPS + 呼吸 LED + GP15 PWM）
)
target_compile_options(${PROJECT_NAME} PRIVATE ${DEMO_COMMON_FLAGS})
if(OS_CFG_DEFINES)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ${OS_CFG_DEFINES})
endif()

# USB CDC 关键定义
# 【v2.7.1 修复】PICO_STDIO_USB_STDOUT_TIMEOUT_US 从 1000000 改回 0：
#   值为 1s 半阻塞时，USB 未打开（无 DTR、主机不读）→ TinyUSB CDC TX FIFO 满 →
#   putchar_raw 在 hal_console_putc 的 PRIMASK 关中断下忙等 1s → 期间禁掉
#   USBCTRL_IRQ/systick 全部中断 → USB 无法推进 → 系统卡死（"不打开 USB 就崩溃"）。
#   0 = 满则丢不阻塞；完整开机画面由 bootlog 缓存回放（hal_bootlog_*）兜底。
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
    PICO_TIME_DEFAULT_ALARM_POOL_HARDWARE_ALARM_NUM=3
)

# USB Composite (CDC+MSC)：打开 TinyUSB MSC class
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
    ${CMAKE_CURRENT_SOURCE_DIR}          # v2.5.1：本工程 os_config.h 必须最前（防被纯库配置遮蔽）
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/shell
    ${RP2040_PORT_DIR}
    ${RP2040_PORT_DIR}/include
    ${RP2040_PORT_DIR}/shell
    ${RP2040_PORT_DIR}/kernel
)

# ================================================================
# 7. 链接内核库 + 移植层 + SDK
#    【v2.5.1-fix】四个静态库相互循环引用（shell→port→fatfs→port、
#    port→kernel_core→port），单遍扫描必断链。用 LINK_GROUP RESCAN
#    （-Wl,--start-group ... --end-group）让链接器循环重扫到收敛：
#      shell.c → shell_fs_register(rp2040_port) → f_opendir(fatfs_module)
#      kernel.c → config_apply(rp2040_port) → sysclk(config_store)
#      hal_port.c → kmalloc(kernel_core)
# ================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    "-Wl,--start-group"
    rp2040_port
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
    pico_multicore
    hardware_vreg
    hardware_pll
    hardware_clocks
)
# FatFs：demo_app.c（exe 编译单元）需要 ff.h（shim 目录）
if(TARGET fatfs_module)
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${FATFS_SHIM_DIR}"
        "${FATFS_SDK_SRC}"
    )
endif()

# Pico SDK USB/UART 双 stdio + UF2 输出
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_usb(${PROJECT_NAME} 1)

# 【v2.6】自定义 linker 脚本：RAM 条带区 256K→128K，bank1/2/3 别名区
#   划给 Core1 镜像（0x21019000）与 IPC 共享乒乓缓冲（0x21028000/0x21038000）。
#   见 memmap_ipc.ld 头注释与 rp2040_port/ipc/shmem_ipc.h。
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_ipc.ld)

# 强制 GCC 链接驱动选择 thumb/v6-m/nofp multilib 路径
target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -mfloat-abi=soft
    --specs=nosys.specs
    --specs=nano.specs
    -Wl,--gc-sections
)

# ================================================================
# 8. 移除 Pico SDK 默认 USB 描述符（仅 CDC），改为 msc_usb.c 的
#    Composite (CDC+MSC) 描述符，避免 multiple definition。
#    （v2.2 起原由 mini-kernel 处理，现移到本工程。）
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
        message(STATUS "[rp2040demo] Removed stdio_usb_descriptors.c from pico_stdio_usb (using msc_usb.c Composite descriptors)")
    endif()
endif()

pico_add_extra_outputs(${PROJECT_NAME})

# ================================================================
# 9. 体积检查（复用 mini-kernel 的 check_size.cmake）
# ================================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DCMAKE_SOURCE_DIR=${MINIK_ROOT}
        -DPROJECT_NAME=${PROJECT_NAME}
        -DMAX_FLASH_KB=20 -DMAX_RAM_KB=8
        -P ${MINIK_ROOT}/cmake/check_size.cmake
    COMMENT "Checking rp2040demo firmware size..."
    VERBATIM
)