/*
 * runtime.c —— LVGL 运行时初始化 / 反初始化
 *
 * 职责边界：
 *   · 只做「进程级一次性」的事：lv_init / 日志钩子 / tick 源 / 线程基准登记
 *     / 桥接层内部子系统的创建与销毁
 *   · 不做对象、显示、输入的管理（那些在各自子系统里）
 *
 * §3.5.3 的配置校验**不在这里**：
 *   C 侧只提供 lvglcj_conf_hash()，比对动作在仓颉侧（src/runtime.cj），
 *   因为「构建期常量」只有仓颉侧才有（由 scripts/gen_conf_const.py 生成）。
 *
 * 初始化顺序有依赖，不可随意调整：
 *   句柄表 → 注册表 → 延迟队列 → 线程基准 → 日志/tick
 *   反初始化则逆序，且必须先停主循环（否则清理会与 LVGL 线程并发）。
 */
#include "lvglcj_internal.h"

#include <stdatomic.h>

static _Atomic int g_initialized = 0;

int32_t lvglcj_is_initialized(void)
{
    return atomic_load(&g_initialized) ? 1 : 0;
}

int32_t lvglcj_require_initialized(const char *func)
{
    if (!atomic_load(&g_initialized)) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, func,
                            "请先调用 lvglcj_init()（或 LvglRuntime.start()）");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_init(void)
{
    if (atomic_load(&g_initialized)) {
        return LVGLCJ_OK; /* 幂等 */
    }

    /* 幂等保护：即使外部已经调过 lv_init，也不要重复初始化 */
    if (!lv_is_initialized()) {
        lv_init();
    }

    /* 日志钩子必须在 lv_init 之后安装，否则漏掉初始化期的日志 */
    lvglcj_log_init();

    /* tick 由 C 侧接管（CLOCK_MONOTONIC），不依赖仓颉时钟 */
    lvglcj_set_tick_cb();

    if (lvglcj_handle_table_init() != LVGLCJ_OK) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "句柄表初始化失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    if (lvglcj_deferred_init() != LVGLCJ_OK) {
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    /* 任务队列（§3.9）：有界 1024，默认 fail-fast */
    if (lvglcj_queue_init() != LVGLCJ_OK) {
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    /*
     * 线程基准登记（§3.6.5）：
     *   方案 C 下这就是「创建 runtime 的线程」，断言逻辑与方案 A 完全一致。
     *   方案 A 下 lvglcj_start_thread() 会由主循环线程重新登记覆盖它。
     */
    lvglcj_thread_bind_current();

    atomic_store(&g_initialized, 1);
    lvglcj_log(LVGLCJ_LOG_INFO, __func__, "lvgl4cj 初始化完成");
    return LVGLCJ_OK;
}

int32_t lvglcj_deinit(void)
{
    if (!atomic_load(&g_initialized)) {
        return LVGLCJ_OK;
    }

    /* 先停主循环，保证此后的清理不会与 LVGL 线程并发访问 LVGL */
    lvglcj_stop_thread(LVGLCJ_SHUTDOWN_DRAIN);

    lv_deinit();

    /*
     * 泄漏自检（§11.1「句柄泄漏」门禁）：
     *   正常退出时不应残留 ALIVE 句柄；残留说明有对象没被 close()。
     *   这里只记录错误而不阻塞退出 —— 报告问题，但不制造新的崩溃点。
     */
    int32_t leaked = lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE);
    if (leaked > 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, 0, 0, __func__,
                            "退出时仍有 ALIVE 句柄残留，可能存在对象泄漏");
    }

    /*
     * 逆序销毁：延迟队列持有的是句柄，必须在句柄表之前释放。
     * 任务队列同理 —— 它持有的是 closure_id，虽然 cid 表在仓颉侧，
     * 但队列里的等待者必须在句柄表拆除前被唤醒并返回，
     * 否则可能出现「仓颉侧已清理、C 侧还在等任务」的窗口。
     */
    lvglcj_deferred_destroy();
    lvglcj_queue_destroy();
    lvglcj_registry_destroy();
    lvglcj_handle_table_destroy();

    atomic_store(&g_initialized, 0);
    return LVGLCJ_OK;
}
