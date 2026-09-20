/*
 * probe_support.c —— 探针的 C 侧支持库（liblvgl4cj_probe）
 *
 * 提供两类能力，供仓颉侧探针工程调用：
 *   1. 「在 C 创建的独立 OS 线程里调用仓颉函数」—— V1 的核心机制
 *   2. ASan 有效性自证 —— V4 要求先人为制造问题，确认工具本身生效
 *
 * 设计要点：V1 的结果必须用**带超时的 join**取回。
 *   因为 V1 的失败模式之一是「仓颉代码在外部 OS 线程里卡死」——
 *   如果探针自己挂死，就拿不到这个结论了。所以宁可超时返回，不可无限等待。
 */
#include "lvglcj_probe.h"
#include "lvglcj_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ 工具 */
int64_t lvglcj_probe_os_tid(void)
{
    return lvglcj_current_os_tid();
}

void lvglcj_probe_report(const char *probe, const char *key, const char *value)
{
    printf("[PROBE] %-8s %-24s = %s\n", probe, key, value);
    fflush(stdout); /* 探针若崩溃，已打印的结论必须已经落到 stdout */
}

/* ==================================================== V1：外部 OS 线程执行仓颉 */
static int32_t (*g_cj_entry)(int32_t, int64_t) = NULL;
static _Atomic int32_t g_c_result = -999; /* -999 = 未完成 */
static _Atomic int32_t g_c_calls = 0;
static _Atomic int64_t g_c_tid = 0;
static pthread_t       g_c_thread;
static int             g_c_thread_started = 0;

static void *probe_v1_thread_body(void *arg)
{
    int32_t iterations = (int32_t)(intptr_t)arg;

    /* 记录本线程的 OS TID：仓颉侧会拿它和主线程 TID 比对，确认确实换了线程 */
    atomic_store(&g_c_tid, lvglcj_current_os_tid());

    int32_t (*fn)(int32_t, int64_t) = g_cj_entry;
    if (fn == NULL) {
        atomic_store(&g_c_result, -1);
        return NULL;
    }

    for (int32_t i = 0; i < iterations; i++) {
        int32_t rc = fn(i, lvglcj_current_os_tid());
        atomic_fetch_add(&g_c_calls, 1);
        if (rc != 0) {
            /* 仓颉回调返回非 0：说明它在外部线程里无法正常工作 */
            atomic_store(&g_c_result, rc);
            return NULL;
        }
    }
    atomic_store(&g_c_result, 0);
    return NULL;
}

int32_t lvglcj_probe_set_cangjie_entry(int32_t (*fn)(int32_t, int64_t))
{
    g_cj_entry = fn;
    return LVGLCJ_OK;
}

int32_t lvglcj_probe_run_c_thread(int32_t iterations)
{
    if (g_c_thread_started) {
        return LVGLCJ_OK; /* 幂等：一次探针只跑一条线程 */
    }
    if (iterations <= 0) {
        iterations = 1;
    }

    atomic_store(&g_c_result, -999);
    atomic_store(&g_c_calls, 0);
    atomic_store(&g_c_tid, 0);
    g_c_thread_started = 1;

    int err = pthread_create(&g_c_thread, NULL, probe_v1_thread_body,
                             (void *)(intptr_t)iterations);
    if (err != 0) {
        g_c_thread_started = 0;
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_probe_join_c_thread(int32_t timeout_ms)
{
    if (!g_c_thread_started) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }

    struct timespec tick;
    tick.tv_sec = 0;
    tick.tv_nsec = 2L * 1000L * 1000L; /* 2ms 轮询 */

    int32_t waited = 0;
    while (waited < timeout_ms) {
        if (atomic_load(&g_c_result) != -999) {
            pthread_join(g_c_thread, NULL);
            return LVGLCJ_OK;
        }
        nanosleep(&tick, NULL);
        waited += 2;
    }
    /* 超时：不 join（线程可能真的卡死了），把结论交给调用方 */
    return LVGLCJ_ERR_BACKEND_FAILURE;
}

int32_t lvglcj_probe_c_thread_result(void) { return atomic_load(&g_c_result); }
int32_t lvglcj_probe_c_thread_calls(void)  { return atomic_load(&g_c_calls); }
int64_t lvglcj_probe_c_thread_tid(void)    { return atomic_load(&g_c_tid); }

/* ==================================================== V4：ASan 有效性自证 */
int32_t lvglcj_probe_asan_trigger(int32_t kind)
{
    if (kind == 0) {
        /* 堆内存泄漏：LeakSanitizer 应在进程退出时报告 */
        void *leak = malloc(1234);
        if (leak == NULL) {
            return LVGLCJ_ERR_OUT_OF_MEMORY;
        }
        memset(leak, 0xAB, 1234);
        /* 故意不 free —— 这就是本函数的全部目的 */
        return LVGLCJ_OK;
    }

    if (kind == 1) {
        /* 堆缓冲区越界写：AddressSanitizer 应立即报告 */
        volatile uint8_t *buf = (volatile uint8_t *)malloc(8);
        if (buf == NULL) {
            return LVGLCJ_ERR_OUT_OF_MEMORY;
        }
        buf[12] = 1; /* 越界 */
        free((void *)buf);
        return LVGLCJ_OK;
    }

    return LVGLCJ_ERR_INVALID_ARGUMENT;
}
