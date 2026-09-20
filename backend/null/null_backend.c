/*
 * null_backend.c —— headless 后端实现
 *
 * 设计理由见 lvglcj_backend_null.h。这里只补充实现层面的两点：
 *
 * 1) 为什么复用 lvglcj_display_create，而不是自己调 lv_display_create：
 *    缓冲分配、stride 计算、句柄登记、删除顺序这四件事必须与真实后端**完全一致**，
 *    否则「headless 测过」不能推出「SDL2 下也对」。复用同一入口是唯一能做到
 *    这两条路径不漂移的办法。
 *
 * 2) 为什么 pump 要自己 sleep：
 *    lv_timer_handler 只在时间真的过去后才会触发刷新（默认 30ms 周期）。
 *    若不带间隔地空转，被测代码会看到「调了 100 次 handler 却一帧都没刷」，
 *    从而写出错误的假设。这里的 sleep 让 headless 的时序贴近真实运行。
 */
#include "lvglcj_backend_null.h"
#include "lvglcj_internal.h"

#include <unistd.h>

static int64_t g_null_display = LVGLCJ_HANDLE_NULL;

int32_t lvglcj_null_init(int32_t w, int32_t h, int32_t color_format, int32_t buf_lines)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    if (g_null_display != LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_OK; /* 幂等：已初始化 */
    }

    int64_t d = lvglcj_display_create(w, h, color_format,
                                      LVGLCJ_BUF_PARTIAL, buf_lines);
    if (d == LVGLCJ_HANDLE_NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "headless display 创建失败");
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    g_null_display = d;
    lvglcj_log(LVGLCJ_LOG_INFO, __func__,
               "headless 后端就绪（无窗口；flush 由未注册 cid 的自动放行路径处理）");
    return LVGLCJ_OK;
}

int32_t lvglcj_null_deinit(void)
{
    if (g_null_display == LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_OK; /* 幂等 */
    }
    int32_t rc = lvglcj_display_delete(g_null_display);
    /*
     * 即使删除返回 INVALID_HANDLE（已被别处删除）也把本地记录清掉，
     * 避免 g_null_display 变成一个悬空的老句柄被反复使用。
     */
    g_null_display = LVGLCJ_HANDLE_NULL;
    return rc;
}

int64_t lvglcj_null_display(void)
{
    return g_null_display;
}

int32_t lvglcj_null_pump(int32_t frames)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    if (frames < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "frames 不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    for (int32_t i = 0; i < frames; ++i) {
        /*
         * 复用方案 C 的泵：帧内顺序（延迟删除 → 任务队列 → 定时器）
         * 与方案 A 主循环严格一致，因此两种驱动方式的时序语义可比。
         */
        rc = lvglcj_pump();
        if (rc != LVGLCJ_OK) {
            return rc;
        }
        /* 让刷新周期真正到期，否则 handler 空转不出帧 */
        usleep((useconds_t)LVGLCJ_LOOP_SLEEP_MS * 1000u);
    }
    return LVGLCJ_OK;
}
