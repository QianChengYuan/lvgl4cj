/*
 * callback.c —— trampoline 实现（设计文档 §3.2.5，分支 B）
 *
 * trampoline 的职责边界（刻意做得很窄）：
 *   1. 从 user_data 取回 closure_id（显式经 intptr_t）
 *   2. 把「LVGL 回调的载荷」翻译成一个 int64 的 arg（句柄或标量）
 *   3. 交给分发器（g_dispatch → 仓颉闭包表）
 *   4. **吞掉一切异常，转成错误码** —— 绝不允许异常跨越 C 边界（§3.8.1）
 *
 * arg 的统一语义（约定，两侧必须一致）：
 *   event      → 事件对象句柄（lv_event_t 在句柄表中的 ID）
 *   timer      → 定时器句柄（见 timer.c）
 *   anim_exec  → 动画插值（int32 值经 int64 传递，见 anim.c）
 *   flush      → display 句柄
 *   indev_read → indev 句柄
 */
#include "lvglcj_internal.h"

/* ============================================================ T1 事件 */
void lvglcj_event_trampoline(lv_event_t *e)
{
    if (e == NULL) {
        return;
    }

    int32_t cid = lvglcj_ptr_to_cid(lv_event_get_user_data(e));
    if (cid == LVGLCJ_CID_NONE) {
        /* 内部钩子（如 DELETE 钩子）走的是专用函数，不该进到这里 */
        return;
    }

    /*
     * 事件对象是**短命**的：每次分发都新登记一个句柄，回调结束后立刻回收。
     * 这样仓颉侧的 e.target / e.currentTarget 等查询都能走统一的句柄通路，
     * 而不需要在 L1 层区分「事件句柄」与「对象句柄」两套类型。
     *
     * 回收策略：INVALIDATED → RELEASED 在同一次调用内完成，
     * 因此表项内存不会随事件数量增长（ID 仍单调不复用，符合 ADR-001）。
     */
    int64_t evh = lvglcj_handle_register(e, "lv_event_t");
    if (evh == LVGLCJ_HANDLE_NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, cid, __func__,
                            "事件句柄登记失败，回调被跳过");
        return;
    }

    /* 分发器内部已做回调深度保护与异常吞并（见 dispatch.c） */
    (void)lvglcj_call_closure(cid, evh);

    lvglcj_handle_invalidate(evh);
    lvglcj_handle_release(evh);
}
