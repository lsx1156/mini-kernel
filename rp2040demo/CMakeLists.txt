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
add_executable(${PROJECT_NAME}
    src/main.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ================================================================
# 3. 链接内核库（裁剪开关由 mini-kernel 侧的 OS_CFG_* 决定）
#    shell_module 等的启用变量 periph_module_ENABLED / shell_module_ENABLED
#    等，在 add_subdirectory 后会带到父作用域
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
if(periph_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE periph_module)
endif()
if(shell_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell_module)
    target_link_libraries(shell_module PUBLIC kernel_core)
endif()
if(vfs_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE vfs_module)
endif()
if(fatfs_module_ENABLED)
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
