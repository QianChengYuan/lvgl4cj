/*
 * error.c —— 统一错误记录（设计文档 §3.12）
 *
 * 设计要点：
 *   · 单一出口 lvglcj_record_error()，所有错误都从这里出去，
 *     便于统一日志格式、统一计数、统一速率限制
 *   · 错误回调在**释放锁之后**调用：回调里若再触发记录不会自锁死
 *   · CALLBACK_THREW / WRONG_THREAD 做速率限制（§实现注意事项）：
 *     这两类在长时间运行 + 高频事件下会刷屏，但计数不丢
 */
#include "lvglcj_error.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 错误码槽位：负数码取 -code 作为下标（1..17） */
#define LVGLCJ_ERR_SLOTS 32
#define LVGLCJ_LAST_MSG_MAX 192

/* 速率限制（仅对可预期的噪声型错误生效） */
#define LVGLCJ_RATE_WINDOW_MS 1000u
#define LVGLCJ_RATE_MAX_PER_WINDOW 10u

typedef struct {
    uint32_t window_start_ms;
    uint32_t in_window;
} lvglcj_rate_t;

static pthread_mutex_t g_err_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t         g_err_counts[LVGLCJ_ERR_SLOTS];
static uint32_t         g_err_total;
static lvglcj_error_cb_t g_err_cb;
static lvglcj_rate_t    g_err_rate[LVGLCJ_ERR_SLOTS];

static lvglcj_error_t   g_last_err;
static char             g_last_msg[LVGLCJ_LAST_MSG_MAX];

static uint32_t lvglcj_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

static int lvglcj_err_slot(int32_t code)
{
    if (code >= 0) {
        return 0; /* 非错误码，归入总计数 */
    }
    int slot = -code;
    if (slot >= LVGLCJ_ERR_SLOTS) {
        return 0;
    }
    return slot;
}

/* 这两类错误在稳态运行中会出现（回调抛异常被吞、误跨线程），需要限流 */
static int lvglcj_err_is_noisy(int32_t code)
{
    return (code == LVGLCJ_ERR_CALLBACK_THREW) || (code == LVGLCJ_ERR_WRONG_THREAD);
}

static int lvglcj_err_should_emit(int slot, int32_t code)
{
    if (!lvglcj_err_is_noisy(code)) {
        return 1;
    }
    uint32_t now = lvglcj_now_ms();
    lvglcj_rate_t *r = &g_err_rate[slot];
    if (r->window_start_ms == 0u || (now - r->window_start_ms) >= LVGLCJ_RATE_WINDOW_MS) {
        r->window_start_ms = now;
        r->in_window = 0;
    }
    r->in_window++;
    return r->in_window <= LVGLCJ_RATE_MAX_PER_WINDOW;
}

int32_t lvglcj_error_set_cb(lvglcj_error_cb_t cb)
{
    pthread_mutex_lock(&g_err_lock);
    g_err_cb = cb;
    pthread_mutex_unlock(&g_err_lock);
    return LVGLCJ_OK;
}

void lvglcj_record_error(int32_t code, int64_t handle, int32_t closure_id,
                         const char *func, const char *msg)
{
    if (code == 0) {
        return; /* 0 不是错误，静默忽略 */
    }

    int slot = lvglcj_err_slot(code);
    char local_msg[LVGLCJ_LAST_MSG_MAX];
    lvglcj_error_cb_t cb = NULL;
    lvglcj_error_t snapshot;
    int emit = 0;

    local_msg[0] = '\0';
    if (msg != NULL) {
        /* 只做截断拷贝，绝不分配；错误路径必须无失败点 */
        strncpy(local_msg, msg, sizeof(local_msg) - 1);
        local_msg[sizeof(local_msg) - 1] = '\0';
    }

    pthread_mutex_lock(&g_err_lock);

    if (slot != 0) {
        g_err_counts[slot]++;
    }
    g_err_total++;

    emit = lvglcj_err_should_emit(slot, code);

    snapshot.code = code;
    snapshot.handle = handle;
    snapshot.closure_id = closure_id;
    /* func 由调用方传入 __func__，生命周期为静态字符串，可直接持有 */
    snapshot.func = func;
    snapshot.msg = (local_msg[0] != '\0') ? local_msg : NULL;

    g_last_err = snapshot;
    if (local_msg[0] != '\0') {
        memcpy(g_last_msg, local_msg, sizeof(g_last_msg));
        g_last_msg[sizeof(g_last_msg) - 1] = '\0';
        g_last_err.msg = g_last_msg;
    } else {
        g_last_msg[0] = '\0';
        g_last_err.msg = NULL;
    }

    cb = g_err_cb;

    pthread_mutex_unlock(&g_err_lock);

    /* ★ 锁外回调：避免回调内再次记录错误导致自锁 */
    if (emit && cb != NULL) {
        cb(&snapshot);
    }
}

const char *lvglcj_strerror(int32_t code)
{
    switch (code) {
        case LVGLCJ_OK:                        return "OK";
        case LVGLCJ_OK_DEFERRED:               return "OK_DEFERRED（成功但已延迟）";
        case LVGLCJ_ERR_INVALID_HANDLE:        return "INVALID_HANDLE（句柄失效）";
        case LVGLCJ_ERR_NOT_INITIALIZED:       return "NOT_INITIALIZED（未调用 lvglcj_init）";
        case LVGLCJ_ERR_INVALID_CONFIG:        return "INVALID_CONFIG（lv_conf 哈希不匹配）";
        case LVGLCJ_ERR_CALLBACK_THREW:        return "CALLBACK_THREW（仓颉回调抛异常，已吞掉）";
        case LVGLCJ_ERR_OUT_OF_MEMORY:         return "OUT_OF_MEMORY（内存不足）";
        case LVGLCJ_ERR_WRONG_THREAD:          return "WRONG_THREAD（跨线程调用 LVGL API）";
        case LVGLCJ_ERR_DEADLOCK_RISK:         return "DEADLOCK_RISK（存在等待环）";
        case LVGLCJ_ERR_QUEUE_FULL:            return "QUEUE_FULL（任务队列满）";
        case LVGLCJ_ERR_BACKEND_FAILURE:       return "BACKEND_FAILURE（后端失败）";
        case LVGLCJ_ERR_INVALID_ARGUMENT:      return "INVALID_ARGUMENT（参数非法）";
        case LVGLCJ_ERR_CLOSURE_EXHAUSTED:     return "CLOSURE_EXHAUSTED（闭包 ID 耗尽）";
        case LVGLCJ_ERR_PENDING_DELETE:        return "PENDING_DELETE（对象待删除）";
        case LVGLCJ_ERR_NOT_SUPPORTED:         return "NOT_SUPPORTED（当前配置不支持）";
        case LVGLCJ_ERR_VERSION_MISMATCH:      return "VERSION_MISMATCH（LVGL 版本不匹配）";
        case LVGLCJ_ERR_DEFERRED_LOOP:         return "DEFERRED_LOOP（延迟删除循环超限）";
        case LVGLCJ_ERR_PIN_EXHAUSTED:         return "PIN_EXHAUSTED（已归档：分支 A 未采用）";
        case LVGLCJ_ERR_ANIM_CTX_LOST:         return "ANIM_CTX_LOST（动画上下文丢失）";
        default:                               return "UNKNOWN（未知错误码）";
    }
}

int32_t lvglcj_error_count(int32_t code)
{
    if (code == 0) {
        pthread_mutex_lock(&g_err_lock);
        uint32_t t = g_err_total;
        pthread_mutex_unlock(&g_err_lock);
        return (int32_t)t;
    }
    int slot = lvglcj_err_slot(code);
    pthread_mutex_lock(&g_err_lock);
    uint32_t c = (slot != 0) ? g_err_counts[slot] : 0u;
    pthread_mutex_unlock(&g_err_lock);
    return (int32_t)c;
}

void lvglcj_error_reset_counts(void)
{
    pthread_mutex_lock(&g_err_lock);
    memset(g_err_counts, 0, sizeof(g_err_counts));
    memset(g_err_rate, 0, sizeof(g_err_rate));
    g_err_total = 0u;
    memset(&g_last_err, 0, sizeof(g_last_err));
    g_last_msg[0] = '\0';
    pthread_mutex_unlock(&g_err_lock);
}

int32_t lvglcj_last_error(lvglcj_error_t *out)
{
    if (out == NULL) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&g_err_lock);
    *out = g_last_err;
    if (g_last_msg[0] != '\0') {
        out->msg = g_last_msg; /* 指向内部静态缓冲，仅诊断用 */
    }
    int32_t code = g_last_err.code;
    pthread_mutex_unlock(&g_err_lock);
    return code;
}
