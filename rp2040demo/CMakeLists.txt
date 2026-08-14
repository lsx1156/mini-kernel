cmake_minimum_required(VERSION 3.13)

# ================================================================
#  RP2040 Demo 独立工程 —— 使用 mini-kernel 作为内核静态库
#
#  目录设计（project/ 作为 git 单仓根）：
#    rp2040demo/        应用层（本工程）
#      ├ src/main.c      main()，调用 kernel_main() 起内核
#      └ build.bat       一键构建脚本
#    mini-kernel/       内核库（平级兄弟目录）
#      └ CMakeLists.txt 导出 kernel_core / hal_port / shell_module 等 target
#
#  PICO SDK 初始化必须在 project() 之前 — 这是 Pico SDK 官方强制顺序，
#  否则会丢掉 -mcpu=cortex-m0plus -mthumb 等关键 flags，最终导致
#  hardware/sync.h 中的 PRIMASK/cpsid 指令无法通过汇编。
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
# 1. mini-kernel 作为子目录引入（EXCLUDE_FROM_ALL：不构建其内置 demo 固件，
#    只构建静态库 target。我们 rp2040demo 可执行链接这些静态库）
# ================================================================
set(MINIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mini-kernel"
    CACHE PATH "mini-kernel root dir")
if(NOT EXISTS "${MINIK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "mini-kernel CMakeLists.txt missing at ${MINIK_ROOT}/CMakeLists.txt")
endif()
add_subdirectory(${MINIK_ROOT} ${CMAKE_BINARY_DIR}/minik-build EXCLUDE_FROM_ALL)

# ================================================================
# 2. 应用源文件
# ================================================================
set(RP2040DEMO_SRCS
    src/main.c
)

# demo_app.c 由 mini-kernel 侧的 OS_CFG_DEMO_APP 开关控制
#   mini-kernel 自身的 CMakeLists.txt 仅把它编译到 mini_kernel 可执行目标，
#   rp2040demo 作为独立工程需要自己判断 OS_CFG_DEMO_APP 变量（已被
#   mini-kernel 的 add_subdirectory 解析并设置到父作用域）。
if(OS_CFG_DEMO_APP)
    list(APPEND RP2040DEMO_SRCS
        ${MINIK_ROOT}/examples/builtin_demo/demo_app.c
    )
endif()

add_executable(${PROJECT_NAME} ${RP2040DEMO_SRCS})

# 【关键】为 rp2040demo 可执行目标添加 Cortex-M0+ Thumb 模式编译选项
#   SDK 源代码（如 hardware/sync.h 中的 PRIMASK/cpsid 指令）必须用 -mthumb
#   编译才能正确生成 Thumb 指令。mini-kernel 的各库 target 都已添加这些标志，
#   但 rp2040demo 是独立可执行目标，需显式添加。
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
target_compile_options(${PROJECT_NAME} PRIVATE ${DEMO_COMMON_FLAGS})

# 将 COMMON_FLAGS 转为空格分隔字符串添加到全局 C_FLAGS
# 防止 SDK OBJECT 库（如 pico_stdlib）在链接时因 CPU 模式不匹配报错
string(REPLACE ";" " " DEMO_FLAGS_STR "${DEMO_COMMON_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEMO_FLAGS_STR}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${DEMO_FLAGS_STR} -x assembler-with-cpp")

# 【关键修复】为 rp2040demo 目标添加 OS_CFG_* 编译宏定义
#   demo_app.c / shell.c / hal_port.c 等文件使用 #if OS_CFG_xxx 条件编译。
#   mini-kernel 通过 add_compile_definitions() 添加的宏只在 mini-kernel
#   目录及其子目录生效，rp2040demo 目标编译这些源文件时看不到它们。
#   必须显式把 OS_CFG_DEFINES 列表加到 rp2040demo 的 target_compile_definitions。
if(OS_CFG_DEFINES)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ${OS_CFG_DEFINES})
endif()

# USB CDC 关键定义：确保串口输出不阻塞、USB 枚举不要求 DTR
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # demo_app.c 需要内核头文件（task.h, sched.h, hal_interface.h 等）
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/port/rp2040
)

# ================================================================
# 3. 链接内核库（裁剪开关由 mini-kernel 侧的 OS_CFG_* 决定）
#    OS_CFG_* 变量已由 mini-kernel 的 CMakeLists.txt 通过 PARENT_SCOPE 传递
#    到父作用域，可直接用于条件判断。
# ================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    kernel_core
    hal_port
    # Pico SDK targets 只在最终可执行 link 一次，
    # 避免内核静态库嵌入 SDK OBJECT 副本。
    pico_stdlib
    hardware_uart
    hardware_timer
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_irq
)
# 直接使用 *_ENABLED 变量判断模块链接（由 mini-kernel 的 add_module_if_enabled 设置）
if(periph_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE periph_module)
endif()
if(shell_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell_module)
endif()
if(vfs_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE vfs_module)
endif()
if(OS_CFG_FATFS)
    target_link_libraries(${PROJECT_NAME} PRIVATE fatfs_module)
endif()

# Pico SDK USB/UART 双 stdio + UF2 输出
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_add_extra_outputs(${PROJECT_NAME})

# ================================================================
# 4. 体积检查（复用 mini-kernel 的 check_size.cmake）
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
