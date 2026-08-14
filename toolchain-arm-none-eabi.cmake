# CMake toolchain file for arm-none-eabi-gcc
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m0plus)

set(TOOLCHAIN_PREFIX arm-none-eabi-)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)

# 解析工具链全路径，避免 post-build 子进程（cmd.exe）PATH 中找不到 objcopy/objdump
find_program(_ARM_GCC_PATH ${TOOLCHAIN_PREFIX}gcc)
if(_ARM_GCC_PATH)
    get_filename_component(TOOLCHAIN_BIN_DIR ${_ARM_GCC_PATH} DIRECTORY)
    set(CMAKE_OBJCOPY ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objcopy${CMAKE_EXECUTABLE_SUFFIX} CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objdump${CMAKE_EXECUTABLE_SUFFIX} CACHE FILEPATH "" FORCE)
    set(CMAKE_SIZE    ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}size${CMAKE_EXECUTABLE_SUFFIX} CACHE FILEPATH "" FORCE)
else()
    set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump CACHE FILEPATH "" FORCE)
    set(CMAKE_SIZE    ${TOOLCHAIN_PREFIX}size CACHE FILEPATH "" FORCE)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ================================================================
# 裸机交叉编译：强制覆盖 WINDOWS 平台默认注入的 DLL 链接选项
#
# 现象：CMake 4.x 在 CMAKE_SYSTEM_NAME=Generic 但 host=Windows 时，
#       仍可能加载 Windows-GNU 平台规则，向所有 link 命令追加：
#         -Wl,--out-implib,libxxx.dll.a
#         -Wl,--major-image-version,0,--minor-image-version,0
#         -lkernel32 -luser32 -lgdi32 ...  (Windows host libs)
#       导致 arm-none-eabi-ld 报：unrecognized option '--major-image-version'
# 修复：显式覆盖 *_LINK_EXECUTABLE / *_STANDARD_LIBRARIES /
#       SHARED_LIBRARY_LINK_*_FLAGS，使用纯 Generic 规则。
# ================================================================
# (1) Clean executable link lines — 不要 implib / 版本号 / Win 系统库
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_ASM_LINK_EXECUTABLE
    "<CMAKE_ASM_COMPILER> <FLAGS> <CMAKE_ASM_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# (2) 移除 kernel32 / user32 等 Windows 主机库注入
set(CMAKE_C_STANDARD_LIBRARIES   "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

# (3) 移除 --major-image-version / --out-implib 等共享库专用后缀标志
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS   "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LIBRARY_LINK_ASM_FLAGS "" CACHE STRING "" FORCE)

# (4) 关闭 exports 标志（Windows 下触发 implib 生成）
set(CMAKE_EXE_EXPORTS_C_FLAG   "" CACHE STRING "" FORCE)
set(CMAKE_EXE_EXPORTS_CXX_FLAG "" CACHE STRING "" FORCE)

# (5) 可选：彻底防止 WIN32/MSYS/MinGW 平台检测分支运行（显式屏蔽 host 平台名变量）
unset(WIN32 CACHE)
unset(MSYS  CACHE)
unset(MINGW CACHE)
set(WIN32 FALSE CACHE INTERNAL "" FORCE)
set(MSYS  FALSE CACHE INTERNAL "" FORCE)
set(MINGW FALSE CACHE INTERNAL "" FORCE)
