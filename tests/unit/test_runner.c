/**
 * @file    test_runner.c
 * @brief   Unity 测试运行器入口
 */
#include "unity_fixture.h"

static void run_all_tests(void) {
    RUN_TEST_GROUP(task_mgmt);
    RUN_TEST_GROUP(mem_mgmt);
    RUN_TEST_GROUP(sched);
}

int main(int argc, char *argv[]) {
    return UnityMain(argc, argv, run_all_tests);
}