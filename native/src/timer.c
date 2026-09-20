/*
 * timer.c —— LVGL 定时器（设计文档 §5.8）
 *
 * 回调桥接（分支 B）：
 *   user_data 装 closure_id（显式经 intptr_t），trampoline 调 dispatch(cid, timer_handle)。
 *   timer_handle 是本定时器在四态句柄表中的 ID，让仓颉侧能对定时器本身操作
 *   （暂停/改周期/删除），而不是只能拿到一个不透明的闭包。
 *
 * 闭包生命周期说明：
 *   定时器只由显式调用创建与删除（不像事件描述符会被对象删除连带释放），
 *   因此不存在「LVGL 偷偷释放了载体而闭包表不知情」的路径。
 *   删除时由 C 侧主动通知仓颉注销（lvglcj_closure_unregister），
 *   保证 §11.3 的「闭包表大小在对象删除后归零」成立。
 */
#include "lvglcj_bridge.h"
#include "lvglcj_internal.h"

/* ================================================================= 回调 */
void lvglcj_timer_trampoline(lv_timer_t *t)
{
    int32_t cid = lvglcj_ptr_to_cid(lv_timer_get_user_data(t));

    /* 定时器句柄：正常情况下 create 时已登记；此处兜底，避免拿到 0 */
    int64_t th = lvglcj_handle_of(t);
    if (th == LVGLCJ_HANDLE_NULL) {
        th = lvglcj_handle_register(t, "lv_timer_t");
    }

    (void)lvglcj_call_closure(cid, th);
}

/* ================================================================= API */
int64_t lvglcj_timer_create(int32_t cid, int32_t period_ms)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (cid <= LVGLCJ_CID_NONE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, cid, __func__,
                            "closure_id 必须为正（0 保留给「无闭包」）");
        return LVGLCJ_HANDLE_NULL;
    }
    if (period_ms < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, cid, __func__,
                            "周期不能为负");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_timer_t *t = lv_timer_create(lvglcj_timer_trampoline, (uint32_t)period_ms,
                                   lvglcj_cid_to_ptr(cid));
    if (t == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, cid, __func__,
                            "lv_timer_create 失败");
        return LVGLCJ_HANDLE_NULL;
    }

    return lvglcj_handle_register(t, "lv_timer_t");
}

int32_t lvglcj_timer_delete(int64_t timer)
{
    LVGLCJ_HANDLE_GUARD(timer, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_timer_t *t = (lv_timer_t *)lvglcj_ptr_of(timer);
    if (t == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    int32_t cid = lvglcj_ptr_to_cid(lv_timer_get_user_data(t));

    lv_timer_delete(t);

    /* 载体已被 LVGL 释放 → 通知仓颉侧摘掉闭包，避免闭包表泄漏 */
    lvglcj_closure_unregister(cid);

    lvglcj_handle_invalidate(timer);
    lvglcj_handle_release(timer);
    return LVGLCJ_OK;
}

int32_t lvglcj_timer_pause(int64_t timer)
{
    LVGLCJ_HANDLE_GUARD(timer, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_timer_pause((lv_timer_t *)lvglcj_ptr_of(timer));
    return LVGLCJ_OK;
}

int32_t lvglcj_timer_resume(int64_t timer)
{
    LVGLCJ_HANDLE_GUARD(timer, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_timer_resume((lv_timer_t *)lvglcj_ptr_of(timer));
    return LVGLCJ_OK;
}

int32_t lvglcj_timer_set_period(int64_t timer, int32_t ms)
{
    LVGLCJ_HANDLE_GUARD(timer, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    if (ms <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, timer, 0, __func__,
                            "周期必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_timer_set_period((lv_timer_t *)lvglcj_ptr_of(timer), (uint32_t)ms);
    return LVGLCJ_OK;
}

int32_t lvglcj_timer_ready(int64_t timer)
{
    LVGLCJ_HANDLE_GUARD(timer, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_timer_ready((lv_timer_t *)lvglcj_ptr_of(timer));
    return LVGLCJ_OK;
}

/* 最近一次 handler 报告的下次触发间隔（ms），供自适应休眠使用 */
static int32_t g_timer_next_ms = 0;

int32_t lvglcj_timer_handler(void)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    /*
     * ★ 返回值语义修正（相对早期版本）：
     *   LVGL 的 lv_timer_handler() 返回「距下一个定时器到期还有多少 ms」——
     *   那是**数据**而不是错误码。直接透传会违反本层 ABI 的统一约定
     *   （0 成功 / 正数仅 +1 为特殊标记 / 负数错误）：
     *   例如返回 30 会被调用方理解成「某种特殊成功标记」，而返回 0 又会被
     *   误读为「成功」——两种解读都错，且不会报错，只在时序上表现异常。
     *
     *   改为：本函数只返回错误码；下次间隔改用 lvglcj_timer_next_ms() 显式查询。
     *   这样「错误码」与「时序数据」两条通道互不干扰。
     */
    /*
     * 计时包裹 lv_timer_handler()（§7.3）：
     *   它内部包含「刷新定时器」的处理，因此这段时间就是 refr_time_ms 的口径，
     *   同时也是「主循环忙碌比」的分子。放在这个唯一咽喉点，
     *   保证方案 A（C 侧线程）与方案 C（pump）两条路径的统计完全一致。
     */
    uint64_t t0 = lvglcj_now_us();
    g_timer_next_ms = (int32_t)lv_timer_handler();
    lvglcj_perf_on_timer_handler((uint32_t)(lvglcj_now_us() - t0));

    return LVGLCJ_OK;
}

int32_t lvglcj_timer_next_ms(void)
{
    /* 纯读取：不校验线程，因为它只是观测数据，多线程下最坏读到旧值 */
    return g_timer_next_ms;
}
