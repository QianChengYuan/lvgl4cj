/*
 * obj.c —— 对象树（设计文档 §5.5）
 *
 * 三件事在这里被「一次性做对」：
 *   1. 创建时**统一挂 DELETE 钩子**（§3.3.3）—— 保证所有经绑定层创建的对象
 *      都能被感知到删除，这是「句柄失效可检测」的前提
 *   2. 删除时区分「回调中」与「非回调中」（§3.8.2 延迟删除）
 *   3. 删除后把句柄从 INVALIDATED 推进到 RELEASED（§3.1.4 步骤 2）
 *
 * ⚠️ 例外（§3.1.5）：LVGL 内部自己创建的对象（如 dropdown 的列表、msgbox 的按钮）
 *   不经过 lvglcj_obj_create，因此没有钩子。策略是「这类对象不向仓颉暴露句柄」，
 *   或在暴露前由 C 侧补挂钩子。P0 只暴露经本入口创建的对象。
 */
#include "lvglcj_internal.h"

/* ------------------------------------------------------------ 创建 / 删除 */
int64_t lvglcj_obj_create(int64_t parent)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (parent != LVGLCJ_HANDLE_NULL) {
        p = (lv_obj_t *)lvglcj_ptr_of(parent);
        if (p == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, parent, 0, __func__,
                                "父对象句柄无效");
            return LVGLCJ_HANDLE_NULL;
        }
    }

    lv_obj_t *o = lv_obj_create(p);
    if (o == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_obj_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t h = lvglcj_handle_register(o, "lv_obj_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 句柄登记失败就必须把原生对象也删掉，否则原生对象泄漏 */
        lv_obj_delete(o);
        return LVGLCJ_HANDLE_NULL;
    }

    /* ★ 统一挂 DELETE 钩子（§3.3.3），顺序上必须在返回句柄之前完成 */
    lvglcj_lifecycle_install_hook(o);
    return h;
}

/*
 * 立即删除（内部）—— 不走延迟队列。
 *
 * 调用方：lvglcj_obj_delete（非回调场景）与 deferred.c 的 drain。
 * 幂等：句柄已失效/已回收时直接返回成功。
 */
int32_t lvglcj_obj_delete_now(int64_t h)
{
    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(h);
    if (o == NULL) {
        /* 已经删过了（或从未有效）：幂等返回，并顺手回收表项 */
        lvglcj_handle_release(h);
        return LVGLCJ_OK;
    }

    /*
     * lv_obj_delete 会**同步**触发 LV_EVENT_DELETE，
     * 我们的钩子在那里完成 (a) 闭包清理 (b) 子树递归失效 (c) 停动画 (d) 自身失效。
     */
    lv_obj_delete(o);

    /* §3.1.4 步骤 2：INVALIDATED → RELEASED，回收表项内存 */
    lvglcj_handle_release(h);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_delete(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (lvglcj_callback_depth() > 0) {
        /*
         * 回调中删除 → 延迟（§3.8.2）。
         * 返回 +1（OK_DEFERRED，非错误）：对象立刻进入「待删除」状态，
         * 实际删除最迟在同一帧内完成。
         */
        int32_t prc = lvglcj_deferred_push(LVGLCJ_DEFER_DELETE, obj, 0);
        if (prc != LVGLCJ_OK) {
            return prc;
        }
        lvglcj_handle_mark_pending_delete(obj);
        return LVGLCJ_OK_DEFERRED;
    }

    return lvglcj_obj_delete_now(obj);
}

/* ------------------------------------------------------------ clean */
/*
 * lv_obj_clean：删除全部子对象，但**父对象本身保持存活**。
 * g_cleaning_parent 用保存-恢复实现，因此支持嵌套（§3.3.2）。
 */
int32_t lvglcj_obj_clean(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(obj);
    if (o == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    int64_t saved = lvglcj_lifecycle_begin_clean(obj);
    lv_obj_clean(o);
    lvglcj_lifecycle_end_clean(saved);

    /*
     * ★ 父句柄此刻必须仍然 ALIVE —— 这正是 clean 与 delete 的语义差别。
     *   若这里断言失败，说明子树失效逻辑误伤了父对象，是严重缺陷。
     */
    if (!lvglcj_handle_alive(obj)) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, obj, 0, __func__,
                            "clean 后父句柄被误失效（clean 语义要求父保持存活）");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 几何与层级 */
int32_t lvglcj_obj_set_pos(int64_t obj, int32_t x, int32_t y)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_set_pos((lv_obj_t *)lvglcj_ptr_of(obj), x, y);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_set_size(int64_t obj, int32_t w, int32_t h)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_set_size((lv_obj_t *)lvglcj_ptr_of(obj), w, h);
    return LVGLCJ_OK;
}

/*
 * 滚动。
 *
 * ★ 必须 LV_ANIM_OFF：带动画时滚动要若干帧才稳定，"滚一下再读回位置"就成了
 *   时序赌局 —— 本项目已经在 roller 的选中值上踩过同一个坑（那里也是关动画）。
 *
 * 注：本入口**只提供** scroll_by，不把 get_scroll_y 做成 ABI —— 它与负错误码
 *   共用同一取值空间（滚动量可以是任意 int32，含负值），无法用返回码区分成功与失败。
 *   需要读当前滚动量的用例直接调 lv_obj_get_scroll_y（如 C 侧测试）。
 *   顺带记一个**实测结论**（我先写错过）：`scrollBy(0, -60)` 之后
 *   `lv_obj_get_scroll_y` 从 0 变成 **+60** —— 也就是"向下滚"是**变大**，
 *   不是变小。（见 test_style_widgets 的 7l 段打印的 scroll_y。）
 */
int32_t lvglcj_obj_scroll_by(int64_t obj, int32_t dx, int32_t dy)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_scroll_by((lv_obj_t *)lvglcj_ptr_of(obj), dx, dy, LV_ANIM_OFF);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_set_parent(int64_t obj, int64_t parent)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (parent != LVGLCJ_HANDLE_NULL) {
        p = (lv_obj_t *)lvglcj_ptr_of(parent);
        if (p == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, parent, 0, __func__,
                                "新父对象句柄无效");
            return LVGLCJ_ERR_INVALID_HANDLE;
        }
    }
    lv_obj_set_parent((lv_obj_t *)lvglcj_ptr_of(obj), p);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_align(int64_t obj, int32_t align, int32_t x_ofs, int32_t y_ofs)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_align((lv_obj_t *)lvglcj_ptr_of(obj), (lv_align_t)align, x_ofs, y_ofs);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_get_x(int64_t obj)
{
    if (lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) return 0;
    return lv_obj_get_x((lv_obj_t *)lvglcj_ptr_of(obj));
}

int32_t lvglcj_obj_get_y(int64_t obj)
{
    if (lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) return 0;
    return lv_obj_get_y((lv_obj_t *)lvglcj_ptr_of(obj));
}

int32_t lvglcj_obj_get_width(int64_t obj)
{
    if (lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) return 0;
    return lv_obj_get_width((lv_obj_t *)lvglcj_ptr_of(obj));
}

int32_t lvglcj_obj_get_height(int64_t obj)
{
    if (lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) return 0;
    return lv_obj_get_height((lv_obj_t *)lvglcj_ptr_of(obj));
}

int32_t lvglcj_obj_add_flag(int64_t obj, int32_t flag)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_add_flag((lv_obj_t *)lvglcj_ptr_of(obj), (lv_obj_flag_t)flag);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_remove_flag(int64_t obj, int32_t flag)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_remove_flag((lv_obj_t *)lvglcj_ptr_of(obj), (lv_obj_flag_t)flag);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_add_state(int64_t obj, int32_t state)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_add_state((lv_obj_t *)lvglcj_ptr_of(obj), (lv_state_t)state);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_remove_state(int64_t obj, int32_t state)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_remove_state((lv_obj_t *)lvglcj_ptr_of(obj), (lv_state_t)state);
    return LVGLCJ_OK;
}

int64_t lvglcj_obj_get_child(int64_t obj, int32_t idx)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    lv_obj_t *c = lv_obj_get_child((lv_obj_t *)lvglcj_ptr_of(obj), idx);
    if (c == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_handle_of(c); /* 只返回已登记的对象；内部对象返回 0（§3.1.5 例外） */
}

int32_t lvglcj_obj_get_child_count(int64_t obj)
{
    if (lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) return 0;
    return (int32_t)lv_obj_get_child_count((lv_obj_t *)lvglcj_ptr_of(obj));
}

int32_t lvglcj_obj_move_foreground(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_move_foreground((lv_obj_t *)lvglcj_ptr_of(obj));
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_move_background(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_move_background((lv_obj_t *)lvglcj_ptr_of(obj));
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_move_to_index(int64_t obj, int32_t index)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_move_to_index((lv_obj_t *)lvglcj_ptr_of(obj), index);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_swap(int64_t obj1, int64_t obj2)
{
    LVGLCJ_HANDLE_GUARD(obj1, __func__);
    LVGLCJ_HANDLE_GUARD(obj2, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_swap((lv_obj_t *)lvglcj_ptr_of(obj1), (lv_obj_t *)lvglcj_ptr_of(obj2));
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 屏幕 */
int64_t lvglcj_screen_active(void)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        /* §P0_RESULTS G1：没有 display 时 lv_screen_active() 返回 NULL */
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, __func__,
                            "没有活动屏幕（是否忘了先创建 display？）");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t h = lvglcj_handle_of(scr);
    if (h != LVGLCJ_HANDLE_NULL) {
        return h;
    }
    /*
     * 屏幕由 LVGL 在 display 创建时自动生成，不经过 lvglcj_obj_create，
     * 因此首次取用时要补登记 + 补挂 DELETE 钩子（§3.1.5 的补救措施）。
     */
    h = lvglcj_handle_register(scr, "lv_screen");
    if (h != LVGLCJ_HANDLE_NULL) {
        lvglcj_lifecycle_install_hook(scr);
    }
    return h;
}

int32_t lvglcj_screen_load(int64_t scr)
{
    LVGLCJ_HANDLE_GUARD(scr, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_screen_load((lv_obj_t *)lvglcj_ptr_of(scr));
    return LVGLCJ_OK;
}

int32_t lvglcj_screen_load_anim(int64_t scr, int32_t anim_type, int32_t time_ms,
                                int32_t delay_ms, int32_t auto_del)
{
    LVGLCJ_HANDLE_GUARD(scr, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_screen_load_anim((lv_obj_t *)lvglcj_ptr_of(scr), (lv_screen_load_anim_t)anim_type,
                        (uint32_t)time_ms, (uint32_t)delay_ms, auto_del != 0);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 布局 */
int32_t lvglcj_obj_set_flex_flow(int64_t obj, int32_t flow)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_set_flex_flow((lv_obj_t *)lvglcj_ptr_of(obj), (lv_flex_flow_t)flow);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_set_flex_align(int64_t obj, int32_t main_place, int32_t cross_place,
                                  int32_t track_place)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_set_flex_align((lv_obj_t *)lvglcj_ptr_of(obj),
                          (lv_flex_align_t)main_place,
                          (lv_flex_align_t)cross_place,
                          (lv_flex_align_t)track_place);
    return LVGLCJ_OK;
}
