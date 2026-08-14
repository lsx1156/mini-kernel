# ================================================================
# Mini Kernel CMake 构建系统
# 目标：RP2040 (Cortex-M0+)，Pico SDK 集成
# 特性：分层编译、裁剪宏传递、体积红线检查、单测集成
# ================================================================
cmake_minimum_required(VERSION 3.13)

# mini-kernel 根目录锚点（使用 CURRENT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR，
# 便于其他工程 add_subdirectory(mini-kernel) 引用时路径仍正确）
set(MINIK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "mini-kernel root")

# ================================================================
# 0. Pico SDK 交叉工具链引入（必须在 project() 之前执行！）
#    pico_sdk_import.cmake 会提前把 CMAKE_TOOLCHAIN_FILE、
#    CMAKE_C_COMPILER / CMAKE_CXX_COMPILER / CMAKE_ASM_COMPILER
#    注入 cache，让 project() 的 LANGUAGES C CXX ASM 检查通过。
#    若放在 project() 之后，则清过 CMakeCache 后交叉编译器找不到。
# ================================================================
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")

# 父工程（如 rp2040demo）已经做过 pico_sdk_import / pico_sdk_init 时，
# 不再重复初始化 Pico SDK，避免 OBJECT 库源码被编译两遍 → 链接时符号重复。
if(NOT TARGET pico_stdlib)
    if(NOT PICO_SDK_PATH)
        message(FATAL_ERROR "PICO_SDK_PATH not set. Please export PICO_SDK_PATH or pass -DPICO_SDK_PATH=...")
    endif()
    if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
        message(FATAL_ERROR "PICO_SDK_PATH invalid (missing pico_sdk_import.cmake): ${PICO_SDK_PATH}")
    endif()
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()
set(_MINIK_PARENT_INITS_PICO_SDK_ TARGET pico_stdlib)

# 语言标准（放在 project 前也可；新 CMake 两者都接受）
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# project()：现在 SDK 已经把交叉工具链塞进 cache 了
project(mini_kernel
    VERSION 0.2.0
    LANGUAGES C CXX ASM
    DESCRIPTION "Lightweight time-sliced kernel for resource-constrained 32-bit MCUs"
)

# Pico SDK 初始化（只在 mini-kernel 作为顶层工程时自己执行；
#  rp2040demo 作为父工程已在其自己的 project() 之后执行过，跳过）
if(NOT TARGET pico_stdlib)
    pico_sdk_init()
endif()

# ================================================================
# 1. 编译选项与工具链路径推导（裸机 Cortex-M0+）
# ================================================================

# 从已解析的工具链路径推导 bin 目录，修正 objcopy/objdump 全路径
# （post-build 步骤在 cmd.exe 子进程中运行，PATH 里可能找不到裸名工具）
# 使用 CMAKE_C_COMPILER_AR（始终为全路径）推导，避免裸名编译器变量问题
if(CMAKE_C_COMPILER_AR)
    get_filename_component(_TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER_AR}" DIRECTORY)
else()
    get_filename_component(_TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
endif()
if(WIN32)
    set(_TOOLCHAIN_EXE_SUFFIX ".exe")
else()
    set(_TOOLCHAIN_EXE_SUFFIX "")
endif()
if(EXISTS "${_TOOLCHAIN_BIN_DIR}/arm-none-eabi-objcopy${_TOOLCHAIN_EXE_SUFFIX}")
    set(CMAKE_OBJCOPY "${_TOOLCHAIN_BIN_DIR}/arm-none-eabi-objcopy${_TOOLCHAIN_EXE_SUFFIX}" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP "${_TOOLCHAIN_BIN_DIR}/arm-none-eabi-objdump${_TOOLCHAIN_EXE_SUFFIX}" CACHE FILEPATH "" FORCE)
    set(CMAKE_SIZE    "${_TOOLCHAIN_BIN_DIR}/arm-none-eabi-size${_TOOLCHAIN_EXE_SUFFIX}"    CACHE FILEPATH "" FORCE)
    message(STATUS "Toolchain bin: ${_TOOLCHAIN_BIN_DIR}")
endif()

# 编译器标志（列表形式，供 target_compile_options 使用）
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

# 转为空格分隔字符串后赋给 CMAKE_*_FLAGS（避免列表被拼成分号串）
string(REPLACE ";" " " COMMON_FLAGS_STR "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS "${COMMON_FLAGS_STR} -std=gnu11")
set(CMAKE_ASM_FLAGS "${COMMON_FLAGS_STR} -x assembler-with-cpp")

# 链接器标志（同样转为字符串）
# 注意：Pico SDK 的 pico_standard_link 已自带 nosys.specs / nano.specs 与
# memmap_default.ld，此处不再重复指定 specs 和链接脚本，避免 spec 重定义冲突。
#
# 【绝对禁止】加 "-nostartfiles"：它会让 GCC 跳过 crt0.o / crt1.o 链接，
# 导致 SDK 标准启动链路（Reset → runtime_init → main）完全消失，
# UF2 变成死代码，永远进不了 main() —— 表现就是"单片机完全没反应"。
set(LINK_FLAGS_LIST
    "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft"
    "-Wl,--gc-sections"
    "-Wl,--print-memory-usage"
    "-Wl,-Map=${PROJECT_NAME}.map"
)
string(REPLACE ";" " " CMAKE_EXE_LINKER_FLAGS "${LINK_FLAGS_LIST}")

# ================================================================
# 2. 裁剪配置：从 os_config.h 读取宏并传递给编译器
# ================================================================
# 由用户提供的配置头文件路径
# NOTE: 使用 MINIK_ROOT 而非 CMAKE_SOURCE_DIR，
#       以便 mini-kernel 被其他工程 add_subdirectory 引用时仍能定位自身的 include/os_config.h。
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
    # 同步设置为 CMake 变量，让后面的 if(OS_CFG_SHELL) / add_module_if_enabled 能真·判断开关
    # 同时设置当前作用域（供 mini-kernel 内部使用）和父作用域（供 rp2040demo 使用）
    set(${_name} ${_val})           # 当前作用域（mini-kernel 内部使用）
    set(${_name} ${_val} PARENT_SCOPE)  # 父作用域（rp2040demo 使用）
endforeach()

# 把 OS_CFG_DEFINES 列表也传递到父工程，使 rp2040demo 等独立应用能把它们加到自己的 target_compile_definitions
set(OS_CFG_DEFINES "${OS_CFG_DEFINES}" PARENT_SCOPE)

add_compile_definitions(${OS_CFG_DEFINES})

# PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1:
#   使 stdio_usb_connected() 仅检查 tud_ready()（USB 已枚举），
#   不再要求主机设置 DTR 信号。部分串口助手（如某些版本 PuTTY/
#   串口调试器）不主动拉 DTR/RTS，导致 tud_cdc_connected() 返回 false，
#   stdio_usb_out_chars 直接丢弃所有输出 → COM 口无任何数据。
#   设为 1 后，只要 USB 完成枚举即可输出，兼容所有终端工具。
add_compile_definitions(PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1)

# PICO_STDIO_USB_STDOUT_TIMEOUT_US=0:
#   CDC FIFO 满时（主机未读）stdio_usb_out_chars 的等待超时。
#   SDK 默认 60 秒 → demo_app_init 打印几百字节后 FIFO 满 → 阻塞 60s/字符
#   → sched_start 永远不执行 → LED 不亮。
#   设为 0 → FIFO 满时立即 break 丢弃（不阻塞），系统快速启动。
#   主机打开终端后 FIFO 被持续读取，后续输出正常不丢。
add_compile_definitions(PICO_STDIO_USB_STDOUT_TIMEOUT_US=0)

message(STATUS "OS Config Defines: ${OS_CFG_DEFINES}")

# ================================================================
# 3. 目录结构
# ================================================================
set(INCLUDE_DIRS
    ${MINIK_ROOT}/include
    ${MINIK_ROOT}/include/hal
    ${MINIK_ROOT}/kernel/core
    ${MINIK_ROOT}/kernel/syscall
    ${MINIK_ROOT}/kernel/modules/periph
    ${MINIK_ROOT}/kernel/modules/shell
    ${MINIK_ROOT}/kernel/modules/vfs
    ${MINIK_ROOT}/kernel/modules/loader
    ${MINIK_ROOT}/port/rp2040
)

# ================================================================
# 4. 内核核心库
# ================================================================
file(GLOB KERNEL_CORE_SRCS
    ${MINIK_ROOT}/kernel/core/*.c
    ${MINIK_ROOT}/kernel/core/*.S
    ${MINIK_ROOT}/kernel/syscall/*.c
)

add_library(kernel_core STATIC ${KERNEL_CORE_SRCS})
target_include_directories(kernel_core PUBLIC ${INCLUDE_DIRS})
# 只取 Pico SDK + hardware_* 的头文件路径（generator 表达式），
# 不把 OBJECT 库的对象归档进 .a，避免最终链接时 multiple definition。
# SDK target 统一在可执行（mini_kernel/rp2040demo）上 PRIVATE link 一次。
target_include_directories(kernel_core PRIVATE
    $<TARGET_PROPERTY:pico_stdlib,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_uart,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_timer,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_gpio,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_spi,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_i2c,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_irq,INTERFACE_INCLUDE_DIRECTORIES>
)
# INTERFACE 下游（如可执行）仍能拿到 Pico SDK：由最终 CMakeLists 直接 link
target_compile_options(kernel_core PRIVATE ${COMMON_FLAGS})

# ================================================================
# 5. HAL 移植层库
# ================================================================
# 注意：startup_rp2040.S 提供的 .weak 向量表/Reset_Handler 与 SDK crt0.S
#       的启动流程冲突，必须 **EXCLUDE**。仅保留 context_switch.S。
file(GLOB HAL_SRCS
    ${MINIK_ROOT}/port/rp2040/*.c
    ${MINIK_ROOT}/port/rp2040/context_switch.S
)
# 也可显式列出而非 GLOB，避免新增文件时误链接：
# list(FILTER HAL_SRCS EXCLUDE REGEX "startup_rp2040\\.S$")

add_library(hal_port STATIC ${HAL_SRCS})
target_include_directories(hal_port PUBLIC ${INCLUDE_DIRS})
target_include_directories(hal_port PRIVATE
    $<TARGET_PROPERTY:pico_stdlib,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_uart,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_timer,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_gpio,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_spi,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_i2c,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:hardware_irq,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_options(hal_port PRIVATE ${COMMON_FLAGS})

# ================================================================
# 6. 扩展模块库（按配置裁剪）
# ================================================================
function(add_module_if_enabled name src_dir define_flag)
    message(STATUS "add_module_if_enabled: name=${name}, src_dir=${src_dir}, define_flag=${define_flag}")
    message(STATUS "add_module_if_enabled: ${define_flag}=${${define_flag}}")
    
    if(${define_flag})
        # 不用 file(GLOB) 避免 CMake cache 不自动发现新增的 bootscript.c 这类文件；
        # 显式 CONFIGURE_DEPENDS 让重新 cmake --build 时 glob 会更新。
        file(GLOB _srcs CONFIGURE_DEPENDS ${src_dir}/*.c)
        message(STATUS "add_module_if_enabled: ${name} - found ${_srcs} source files")
        if(_srcs)
            add_library(${name} STATIC ${_srcs})
            target_include_directories(${name} PUBLIC ${INCLUDE_DIRS})
            target_compile_options(${name} PRIVATE ${COMMON_FLAGS})
            set(${name}_ENABLED TRUE CACHE INTERNAL "${name} module enabled")
            message(STATUS "add_module_if_enabled: ${name} - created library, ${name}_ENABLED=TRUE")
        else()
            set(${name}_ENABLED FALSE CACHE INTERNAL "${name} module disabled")
            message(STATUS "add_module_if_enabled: ${name} - no source files found, ${name}_ENABLED=FALSE")
        endif()
    else()
        set(${name}_ENABLED FALSE CACHE INTERNAL "${name} module disabled")
        message(STATUS "add_module_if_enabled: ${name} - ${define_flag} is OFF, ${name}_ENABLED=FALSE")
    endif()
endfunction()

add_module_if_enabled(periph_module ${MINIK_ROOT}/kernel/modules/periph OS_CFG_PERIPH_SERVICE)
add_module_if_enabled(shell_module ${MINIK_ROOT}/kernel/modules/shell OS_CFG_SHELL)
add_module_if_enabled(vfs_module ${MINIK_ROOT}/kernel/modules/vfs OS_CFG_VFS)
add_module_if_enabled(fatfs_module ${MINIK_ROOT}/kernel/modules/vfs/fatfs OS_CFG_FATFS)
add_module_if_enabled(loader_module ${MINIK_ROOT}/kernel/modules/loader OS_CFG_LOADER)

# ================================================================
# 7. 最终固件目标
# ================================================================
# startup_rp2040.S 已废弃：SDK crt0.S 提供正确的向量表（isr_xxx 命名）。
# 若保留并参与链接，它提供的 .weak PendSV_Handler 等与 context_switch.S
# 导出的 isr_pendsv 别名可能产生未定义行为。完全不编译。
add_executable(${PROJECT_NAME})
if(OS_CFG_DEMO_APP)
    target_sources(${PROJECT_NAME} PRIVATE ${MINIK_ROOT}/examples/builtin_demo/demo_app.c)
else()
    # OS_CFG_DEMO_APP=0：不构建 demo_app.c，提供空桩让 kernel.c 链接通过。
    # 由目标工程（如 rp2040demo）在自己的 main.c 里自行注册应用任务。
    target_compile_definitions(kernel_core PUBLIC OS_CFG_DEMO_APP_STUB=1)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
    kernel_core
    hal_port
    # 所有 Pico SDK target 在最终可执行处 link 一次，
    # 避免 kernel_core / hal_port 静态库各自嵌入 SDK OBJECT 副本
    # 导致 multiple definition。
    pico_stdlib
    hardware_uart
    hardware_timer
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_irq
)
# 按模块裁剪开关按需链接（模块未启用时 target 不存在，不能直接写进上面的列表）
# 依赖顺序：模块 → kernel_core → hal_port（模块引用内核核心的符号）
if(periph_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE periph_module)
endif()
if(shell_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell_module)
    # shell 引用 kernel_core 的 syscall_contract 等符号，声明依赖以确保链接顺序
    target_link_libraries(shell_module PUBLIC kernel_core)
endif()
if(vfs_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE vfs_module)
endif()
if(fatfs_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE fatfs_module)
endif()
if(loader_module_ENABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE loader_module)
endif()

# 使用 Pico SDK 默认链接脚本（memmap_default.ld），由 SDK crt0 负责启动流程：
# boot2 -> _entry_point -> runtime_init(时钟/外设) -> main()
# 我们的 startup_rp2040.S 不参与启动（被 --gc-sections 移除），仅保留文件备用。

# Pico SDK 启动支持
# 双通道同时启用：物理 UART0(GPIO0/1) + USB CDC(USB线直连电脑虚拟串口)
# 这样一根 USB 线既能烧录也能交互，不用再接 USB-UART 线。
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_add_extra_outputs(${PROJECT_NAME})

# ================================================================
# 8. 体积红线检查（构建后自动解析 .map 文件）
# ================================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DCMAKE_SOURCE_DIR=${MINIK_ROOT}
        -DPROJECT_NAME=${PROJECT_NAME}
        -DMAX_FLASH_KB=10 -DMAX_RAM_KB=4
        -P ${MINIK_ROOT}/cmake/check_size.cmake
    COMMENT "Checking firmware size against volume limits..."
    VERBATIM
)

# ================================================================
# 9. 诊断固件：最小 SDK 闪灯（不链接 Mini Kernel，纯 SDK API）
#    用途：分流"硬件/烧录通路问题" vs "Mini Kernel 内部问题"
#    → 若本程序烧录后也不亮：硬件/板型/刷写操作问题
#    → 若本程序正常闪：硬件通路 OK，问题在内核代码
# ================================================================
if(EXISTS ${MINIK_ROOT}/tests/minimal_led_test.c)
    add_executable(minimal_led_test)
    target_sources(minimal_led_test PRIVATE ${MINIK_ROOT}/tests/minimal_led_test.c)
    target_link_libraries(minimal_led_test PRIVATE pico_stdlib)
    set_target_properties(minimal_led_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    target_compile_options(minimal_led_test PRIVATE ${COMMON_FLAGS})
    pico_enable_stdio_uart(minimal_led_test 0)
    pico_enable_stdio_usb(minimal_led_test 0)
    pico_add_extra_outputs(minimal_led_test)
    message(STATUS "Diagnostic target: minimal_led_test (pure SDK LED blink)")
endif()

# 诊断固件：最小 USB CDC 打印测试（零内核依赖）
#    用途：验证 USB 枚举 + 发送通路本身是否可行
if(EXISTS ${MINIK_ROOT}/tests/usb_print_test.c)
    add_executable(usb_print_test)
    target_sources(usb_print_test PRIVATE ${MINIK_ROOT}/tests/usb_print_test.c)
    target_link_libraries(usb_print_test PRIVATE pico_stdlib)
    set_target_properties(usb_print_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    target_compile_options(usb_print_test PRIVATE ${COMMON_FLAGS})
    pico_enable_stdio_uart(usb_print_test 0)
    pico_enable_stdio_usb(usb_print_test 1)
    pico_add_extra_outputs(usb_print_test)
    message(STATUS "Diagnostic target: usb_print_test (pure SDK USB CDC print)")
endif()

# ================================================================
# 10. 单元测试目标（Unity + 本地运行）
# ================================================================
enable_testing()

# Unity 测试框架
add_subdirectory(${MINIK_ROOT}/tests/unit)

# ================================================================
# 11. 安装与打包
# ================================================================
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
install(FILES ${OS_CONFIG_HEADER} DESTINATION include)
install(DIRECTORY include/ DESTINATION include)