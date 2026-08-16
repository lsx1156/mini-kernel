# ================================================================
# Mini Kernel CMake 构建系统 —— 纯静态库版本（v2.5 两版本分离）
# 目标：跨平台内核库（不依赖具体 MCU / 不链接 Pico SDK）
# 特性：分层编译、裁剪宏传递、单测集成
#
# 说明：本文件只构建内核核心（kernel_core）与基础模块（shell 等）的
#       静态库，不包含任何 RP2040 专属内容（hal_port / context_switch /
#       msc_usb / config_store / sysclk / demo_app / FatFs 均已在
#       rp2040demo/rp2040_port 中，由目标工程编译）。
# ================================================================
cmake_minimum_required(VERSION 3.13)

# mini-kernel 根目录锚点（使用 CURRENT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR，
# 便于其他工程 add_subdirectory(mini-kernel) 引用时路径仍正确）
set(MINIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "mini-kernel root")

# ================================================================
# 0. 交叉工具链引入（仅在 mini-kernel 作为**顶层**工程、且未初始化 Pico SDK
#    时才引入。父工程（如 rp2040demo）已自行初始化工具链，这里跳过。
#    纯内核库本身不链接 Pico SDK，只需 arm-none-eabi 交叉编译器即可编译。）
# ================================================================
if(NOT TARGET pico_stdlib)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")
    if(PICO_SDK_PATH AND EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
        include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    endif()
endif()

# 语言标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

project(mini_kernel
    VERSION 0.2.0
    LANGUAGES C CXX ASM
    DESCRIPTION "Lightweight time-sliced, portable kernel for resource-constrained 32-bit MCUs"
)

# ================================================================
# 1. 编译选项（裸机 Cortex-M0+ 基线；移植到其它 MCU 时修改 mcpu/march）
# ================================================================
set(COMMON_FLAGS
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
    "-Wshadow"
    "-Wpointer-arith"
    "-Wcast-align"
    "-Wstrict-prototypes"
    "-Wmissing-prototypes"
    "-Werror=implicit-function-declaration"
)
string(REPLACE ";" " " COMMON_FLAGS_STR "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS "${COMMON_FLAGS_STR} -std=gnu11")

# ================================================================
# 2. 裁剪配置：从 os_config.h 读取宏并传递给编译器
# ================================================================
set(OS_CONFIG_HEADER "${MINIK_ROOT}/include/os_config.h" CACHE FILEPATH "Path to os_config.h")
if(NOT EXISTS ${OS_CONFIG_HEADER})
    message(FATAL_ERROR "os_config.h not found at ${OS_CONFIG_HEADER}")
endif()

# 解析 os_config.h 中的宏定义（简易 grep 方式）
file(READ ${OS_CONFIG_HEADER} _config_content)
string(REGEX MATCHALL "#define[ \t]+OS_CFG_[A-Z0-9_]+[ \t]+[0-9]+" _matches ${_config_content})
foreach(_m ${_matches})
    string(REGEX REPLACE "#define[ \t]+(OS_CFG_[A-Z0-9_]+)[ \t]+([0-9]+).*" "\\1" _name ${_m})
    string(REGEX REPLACE "#define[ \t]+(OS_CFG_[A-Z0-9_]+)[ \t]+([0-9]+).*" "\\2" _val  ${_m})
    list(APPEND OS_CFG_DEFINES "${_name}=${_val}")
    set(${_name} ${_val})           # 当前作用域（mini-kernel 内部使用）
    set(${_name} ${_val} PARENT_SCOPE)  # 父作用域（rp2040demo 使用）
endforeach()
set(OS_CFG_DEFINES "${OS_CFG_DEFINES}" PARENT_SCOPE)
add_compile_definitions(${OS_CFG_DEFINES})
message(STATUS "OS Config Defines: ${OS_CFG_DEFINES}")

# ================================================================
# 3. 目录结构（纯内核库：仅包含平台无关的核心与基础模块）
#    【v2.5.1 关键修复】OS_CONFIG_DIR 放在最前：
#    被 add_subdirectory 引用时（如 rp2040demo），OS_CONFIG_HEADER 指向目标工程
#    的 os_config.h；若不放在 ${MINIK_ROOT}/include 之前，kernel.c / shell.c 的
#    #include "os_config.h" 会命中本库自带的纯库配置（PORT_RP2040=0 等），
#    出现 "OS_CFG_xxx redefined" 警告 + RP2040 输入路径/命令被静默裁剪。
# ================================================================
get_filename_component(OS_CONFIG_DIR "${OS_CONFIG_HEADER}" DIRECTORY)
set(INCLUDE_DIRS
    ${OS_CONFIG_DIR}
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/shell
)

# ================================================================
# 4. 内核核心库（task / sched / mem / kernel / syscall）
# ================================================================
file(GLOB KERNEL_CORE_SRCS
    ${MINIK_ROOT}/kernel/core/*.c
    ${MINIK_ROOT}/kernel/syscall/*.c
)
add_library(kernel_core STATIC ${KERNEL_CORE_SRCS})
target_include_directories(kernel_core PUBLIC ${INCLUDE_DIRS})
target_compile_options(kernel_core PRIVATE ${COMMON_FLAGS})

# ================================================================
# 5. 扩展模块库（按配置裁剪；纯库里仅 shell 存在，periph/vfs/fatfs/loader
#    目录已不在本工程，add_module_if_enabled 检测到目录不存在即跳过）
# ================================================================
function(add_module_if_enabled name src_dir define_flag)
    if(${define_flag})
        if(NOT EXISTS "${src_dir}")
            set(${name}_ENABLED FALSE CACHE INTERNAL "${name} module disabled")
            return()
        endif()
        file(GLOB _srcs CONFIGURE_DEPENDS ${src_dir}/*.c)
        if(_srcs)
            add_library(${name} STATIC ${_srcs})
            target_include_directories(${name} PUBLIC ${INCLUDE_DIRS})
            target_compile_options(${name} PRIVATE ${COMMON_FLAGS})
            set(${name}_ENABLED TRUE CACHE INTERNAL "${name} module enabled")
        else()
            set(${name}_ENABLED FALSE CACHE INTERNAL "${name} module disabled")
        endif()
    else()
        set(${name}_ENABLED FALSE CACHE INTERNAL "${name} module disabled")
    endif()
endfunction()

add_module_if_enabled(periph_module ${MINIK_ROOT}/kernel/modules/periph OS_CFG_PERIPH_SERVICE)
add_module_if_enabled(shell_module ${MINIK_ROOT}/kernel/modules/shell OS_CFG_SHELL)
add_module_if_enabled(vfs_module ${MINIK_ROOT}/kernel/modules/vfs OS_CFG_VFS)
add_module_if_enabled(fatfs_module ${MINIK_ROOT}/kernel/modules/vfs/fatfs OS_CFG_FATFS)
add_module_if_enabled(loader_module ${MINIK_ROOT}/kernel/modules/loader OS_CFG_LOADER)

# shell 引用 kernel_core 的 syscall_contract 等符号，声明依赖以确保链接顺序
if(shell_module_ENABLED)
    target_link_libraries(shell_module PUBLIC kernel_core)
endif()

# ================================================================
# 6. 单元测试目标（Unity + 本地运行；Unity 缺失时自动跳过）
# ================================================================
enable_testing()
if(EXISTS "${MINIK_ROOT}/tests/unit/CMakeLists.txt")
    add_subdirectory(${MINIK_ROOT}/tests/unit)
endif()

# ================================================================
# 7. 安装与打包
# ================================================================
install(TARGETS kernel_core
    ARCHIVE DESTINATION lib
)
if(shell_module_ENABLED)
    install(TARGETS shell_module ARCHIVE DESTINATION lib)
endif()
install(FILES ${OS_CONFIG_HEADER} DESTINATION include)
install(DIRECTORY include/ DESTINATION include)