# ================================================================
# 单元测试 CMake 配置（Unity 框架）
# 目标：本地运行，不依赖硬件，模拟 HAL 接口
# ================================================================

# Unity 源码（内置或外部获取）
set(UNITY_DIR ${CMAKE_SOURCE_DIR}/external/unity)
if(NOT EXISTS ${UNITY_DIR}/src/unity.c)
    message(WARNING "Unity not found at ${UNITY_DIR}, tests will be skipped")
    return()
endif()

# 测试源文件
file(GLOB TEST_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_runner.c
)

# 被测模块源文件（链接内核核心，模拟 HAL）
file(GLOB KERNEL_SRCS
    ${CMAKE_SOURCE_DIR}/kernel/core/*.c
)

add_executable(unit_tests
    ${TEST_SRCS}
    ${KERNEL_SRCS}
    ${UNITY_DIR}/src/unity.c
    ${UNITY_DIR}/extras/fixture/src/unity_fixture.c
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${UNITY_DIR}/src
    ${UNITY_DIR}/extras/fixture/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/hal
    ${CMAKE_SOURCE_DIR}/kernel/core
    ${CMAKE_SOURCE_DIR}/kernel/syscall
)

target_compile_definitions(unit_tests PRIVATE
    TEST_BUILD=1
    OS_CFG_PERIPH_SERVICE=0
    OS_CFG_SHELL=0
    OS_CFG_VFS=0
    OS_CFG_FATFS=0
    OS_CFG_LOADER=0
    OS_CFG_KERNEL_HEAP=1
    OS_CFG_STACK_OVF_CHECK=1
    OS_CFG_DEBUG=1
    OS_CFG_ASSERT=1
    UNITY_INCLUDE_CONFIG_H
)

target_compile_options(unit_tests PRIVATE
    -std=gnu11
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
)

# 添加测试目标
add_test(NAME unit_tests COMMAND unit_tests)

# 可选：代码覆盖率
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unit_tests PRIVATE -fprofile-arcs -ftest-coverage)
    target_link_libraries(unit_tests PRIVATE -fprofile-arcs -ftest-coverage)
endif()