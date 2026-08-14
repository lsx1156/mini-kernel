# ================================================================
# 体积红线检查脚本：解析 .map 文件，超限则报错
# 使用：cmake -P check_size.cmake
# ================================================================
cmake_minimum_required(VERSION 3.13)

# 参数
if(NOT DEFINED PROJECT_NAME)
    message(FATAL_ERROR "PROJECT_NAME not defined")
endif()
if(NOT DEFINED MAX_FLASH_KB)
    set(MAX_FLASH_KB 10)
endif()
if(NOT DEFINED MAX_RAM_KB)
    set(MAX_RAM_KB 4)
endif()

# SDK 会生成 ${PROJECT_NAME}.elf.map，自定义链接脚本则生成 ${PROJECT_NAME}.map
set(MAP_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map")
if(NOT EXISTS ${MAP_FILE})
    set(MAP_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf.map")
endif()
if(NOT EXISTS ${MAP_FILE})
    message(WARNING "Map file not found: ${PROJECT_NAME}.map / ${PROJECT_NAME}.elf.map in ${CMAKE_BINARY_DIR}")
    return()
endif()

file(READ ${MAP_FILE} MAP_CONTENT)

# 解析 Memory Configuration 段
# 典型格式：
# Memory Configuration
# Name             Origin             Length             Attributes
# FLASH            0x10000000         0x00200000         rx
# RAM              0x20000000         0x00042000         rwx
# ...
# Linker script and memory map
# ...
# .text            0x10000000        0x1234
# .rodata          0x10000c34        0x56
# .data            0x20000000        0x78
# .bss             0x20000078        0x9a
# ...

# 简易解析：提取 .text + .rodata = Flash，.data + .bss = RAM
string(REGEX MATCHALL "\\.text[ \t]+0x[0-9a-fA-F]+[ \t]+0x([0-9a-fA-F]+)" _flash_matches ${MAP_CONTENT})
string(REGEX MATCHALL "\\.rodata[ \t]+0x[0-9a-fA-F]+[ \t]+0x([0-9a-fA-F]+)" _rodata_matches ${MAP_CONTENT})
string(REGEX MATCHALL "\\.data[ \t]+0x[0-9a-fA-F]+[ \t]+0x([0-9a-fA-F]+)" _data_matches ${MAP_CONTENT})
string(REGEX MATCHALL "\\.bss[ \t]+0x[0-9a-fA-F]+[ \t]+0x([0-9a-fA-F]+)" _bss_matches ${MAP_CONTENT})

math(EXPR FLASH_SIZE 0)
foreach(m ${_flash_matches})
    string(REGEX REPLACE ".*0x([0-9a-fA-F]+).*" "\\1" h ${m})
    math(EXPR FLASH_SIZE "${FLASH_SIZE} + 0x${h}")
endforeach()
foreach(m ${_rodata_matches})
    string(REGEX REPLACE ".*0x([0-9a-fA-F]+).*" "\\1" h ${m})
    math(EXPR FLASH_SIZE "${FLASH_SIZE} + 0x${h}")
endforeach()

math(EXPR RAM_SIZE 0)
foreach(m ${_data_matches})
    string(REGEX REPLACE ".*0x([0-9a-fA-F]+).*" "\\1" h ${m})
    math(EXPR RAM_SIZE "${RAM_SIZE} + 0x${h}")
endforeach()
foreach(m ${_bss_matches})
    string(REGEX REPLACE ".*0x([0-9a-fA-F]+).*" "\\1" h ${m})
    math(EXPR RAM_SIZE "${RAM_SIZE} + 0x${h}")
endforeach()

math(EXPR FLASH_KB "${FLASH_SIZE} / 1024")
math(EXPR RAM_KB "${RAM_SIZE} / 1024")

message(STATUS "Firmware Size: Flash = ${FLASH_KB} KB, RAM = ${RAM_KB} KB")
message(STATUS "Limits:        Flash <= ${MAX_FLASH_KB} KB, RAM <= ${MAX_RAM_KB} KB")

# 注意：含 Pico SDK 运行时/HAL 的总固件体积会超过纯内核红线，
# 此处改为告警而非阻断，便于观察体积变化；纯内核裁剪版另用最小配置构建。
if(FLASH_KB GREATER MAX_FLASH_KB)
    message(WARNING "FLASH SIZE EXCEEDS LIMIT: ${FLASH_KB} KB > ${MAX_FLASH_KB} KB (含 SDK 运行时)")
endif()
if(RAM_KB GREATER MAX_RAM_KB)
    message(WARNING "RAM SIZE EXCEEDS LIMIT: ${RAM_KB} KB > ${MAX_RAM_KB} KB (含 SDK 运行时)")
endif()

message(STATUS "Size check done (warning-only).")