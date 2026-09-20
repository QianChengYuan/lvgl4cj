/*
 * log.c —— LVGL 日志转发到仓颉（设计文档 §3.12.3 / §7.3）
 *
 * ★ 与设计文档 §5.1 的一处有据偏离：
 *   原文为 lvglcj_set_log_cb(int32_t cid)，即日志也走闭包表。
 *   但闭包表的签名是 (Int64) -> Unit，**装不下日志字符串**；
 *   若强行把字符串塞进 int64 会引入悬空指针（LVGL 的 buf 在回调返回后失效）。
 *   因此日志改用与错误回调一致的**函数指针注册**：
 *     typedef void (*lvglcj_log_cb_t)(int32_t level, const char *msg);
 *   仓颉侧用一个顶层不捕获的 CFunc lambda 接收，格式化后打印，
 *   需要时再自行转发到闭包表。这样既规避悬空指针，也不依赖 @C 导出符号名。
 */
#include "lvglcj_error.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static lvglcj_log_cb_t g_log_cb;
static int             g_lvgl_log_hooked;

static const char *lvglcj_level_name(int32_t level)
{
    switch (level) {
        case LVGLCJ_LOG_TRACE: return "TRACE";
        case LVGLCJ_LOG_INFO:  return "INFO";
        case LVGLCJ_LOG_WARN:  return "WARN";
        case LVGLCJ_LOG_ERROR: return "ERROR";
        case LVGLCJ_LOG_USER:  return "USER";
        default:               return "NONE";
    }
}

void lvglcj_log(int32_t level, const char *func, const char *msg)
{
    lvglcj_log_cb_t cb;

    pthread_mutex_lock(&g_log_lock);
    cb = g_log_cb;
    pthread_mutex_unlock(&g_log_lock);

    if (cb != NULL) {
        /* 组装一条完整消息再交给仓颉侧，避免跨语言多次调用 */
        char buf[512];
        snprintf(buf, sizeof(buf), "[lvgl4cj][%s] %s: %s",
                 lvglcj_level_name(level),
                 (func != NULL) ? func : "?",
                 (msg != NULL) ? msg : "");
        cb(level, buf);
    } else {
        /* 未注册回调时退回 stderr，保证错误在开发期不静默 */
        if (level >= LVGLCJ_LOG_WARN) {
            fprintf(stderr, "[lvgl4cj][%s] %s: %s\n",
                    lvglcj_level_name(level),
                    (func != NULL) ? func : "?",
                    (msg != NULL) ? msg : "");
        }
    }
}

int32_t lvglcj_log_set_cb(lvglcj_log_cb_t cb)
{
    pthread_mutex_lock(&g_log_lock);
    g_log_cb = cb;
    pthread_mutex_unlock(&g_log_lock);
    return LVGLCJ_OK;
}

/* ------------------------------------------------- LVGL 原生日志 → 仓颉 */
/* lv_log_print_g_cb_t = void (*)(lv_log_level_t level, const char *buf) */
static void lvglcj_lvgl_log_hook(lv_log_level_t level, const char *buf)
{
    int32_t mapped;
    switch (level) {
        case LV_LOG_LEVEL_TRACE: mapped = LVGLCJ_LOG_TRACE; break;
        case LV_LOG_LEVEL_INFO:  mapped = LVGLCJ_LOG_INFO;  break;
        case LV_LOG_LEVEL_WARN:  mapped = LVGLCJ_LOG_WARN;  break;
        case LV_LOG_LEVEL_ERROR: mapped = LVGLCJ_LOG_ERROR; break;
        case LV_LOG_LEVEL_USER:  mapped = LVGLCJ_LOG_USER;  break;
        default:                 mapped = LVGLCJ_LOG_INFO;  break;
    }
    lvglcj_log(mapped, "lvgl", buf);
}

int32_t lvglcj_log_init(void)
{
    pthread_mutex_lock(&g_log_lock);
    int already = g_lvgl_log_hooked;
    g_lvgl_log_hooked = 1;
    pthread_mutex_unlock(&g_log_lock);

    if (already) {
        return LVGLCJ_OK;
    }
    lv_log_register_print_cb(lvglcj_lvgl_log_hook);
    return LVGLCJ_OK;
}
