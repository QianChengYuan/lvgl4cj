/*
 * widgets.c —— P0 控件集（设计文档 §5「控件（P0：仅三个）」）
 *
 * P0 只提供 obj / label / button 三者，其中 label 与 button 在本文件实现。
 * 控件集扩展（dropdown / slider / chart 等）留待 P1 —— 按设计文档的门禁要求：
 * 「先跑通 hello_cj 与八条断言再加控件，跑不稳不加控件」。
 *
 * ==================== 这里唯一容易被忽略的一步 ====================
 * 控件创建后必须**和 obj_create 一样挂 DELETE 钩子并登记句柄**。
 *   · 不登记句柄 → 仓颉侧拿不到它，等于控件不可用
 *   · 不挂 DELETE 钩子 → 父对象被删除时该控件的句柄**不会被递归失效**，
 *     于是 isAlive() 仍返回 true，而底层对象已经没了 —— 这正是 §3.3
 *     要防的悬空句柄（也直接关系到 P0 断言 4）。
 *
 * 因此每个控件创建函数都完整走这四步：创建 → 登记 → 挂钩子 → 返回。
 * 这段顺序与 obj.c 的 lvglcj_obj_create 保持一致，两边不得漂移。
 */
#include "lvglcj_internal.h"

/* ------------------------------------------------------------ label */

int64_t lvglcj_label_create(int64_t parent)
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

    lv_obj_t *lbl = lv_label_create(p);
    if (lbl == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_label_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t h = lvglcj_handle_register(lbl, "lv_label_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 登记失败必须把原生对象也删掉，否则原生对象泄漏 */
        lv_obj_delete(lbl);
        return LVGLCJ_HANDLE_NULL;
    }
    /* ★ 与 obj_create 一致：统一挂 DELETE 钩子，保证级联失效覆盖控件 */
    lvglcj_lifecycle_install_hook(lbl);
    return h;
}

int32_t lvglcj_label_set_text(int64_t label, const char *text)
{
    LVGLCJ_HANDLE_GUARD(label, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (text == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, label, 0, __func__,
                            "文本指针为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /*
     * LVGL 会自己拷贝一份文本，因此仓颉侧的 CString 在调用返回后即可释放，
     * 不存在生命周期问题（这也是这里敢直接收 const char* 的原因）。
     */
    lv_label_set_text((lv_obj_t *)lvglcj_ptr_of(label), text);
    return LVGLCJ_OK;
}

int32_t lvglcj_label_set_long_mode(int64_t label, int32_t mode)
{
    LVGLCJ_HANDLE_GUARD(label, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * 上界用 LV_LABEL_LONG_CLIP：v9 的枚举顺序是
     *   WRAP(0) / DOT(1) / SCROLL(2) / SCROLL_CIRCULAR(3) / CLIP(4)
     * —— CLIP 排在**最后**，不是 SCROLL_CIRCULAR。
     * （写成 SCROLL_CIRCULAR 会把合法的 CLIP 误判为非法，实测踩到过。）
     */
    if (mode < (int32_t)LV_LABEL_LONG_WRAP || mode > (int32_t)LV_LABEL_LONG_CLIP) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, label, 0, __func__,
                            "未知的 long_mode（0=WRAP 1=DOT 2=SCROLL 3=SCROLL_CIRCULAR 4=CLIP）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_label_set_long_mode((lv_obj_t *)lvglcj_ptr_of(label),
                           (lv_label_long_mode_t)mode);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ button */

int64_t lvglcj_button_create(int64_t parent)
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

    lv_obj_t *btn = lv_button_create(p);
    if (btn == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_button_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t h = lvglcj_handle_register(btn, "lv_button_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        lv_obj_delete(btn);
        return LVGLCJ_HANDLE_NULL;
    }
    /* ★ 同上：控件也必须挂钩子，否则父删除时它的句柄不会被失效 */
    lvglcj_lifecycle_install_hook(btn);
    return h;
}

/* ============================================================ P1 批次 1（§7.1）
 *
 * switch / checkbox / bar。三条共有约定见桥接头同名注释块 —— 这里不复述，
 * 只说**实现层面**多出来的两个 helper 及其理由。
 */

/*
 * 解析父句柄。
 *
 * ★ 用「错误码返回 + 出参」而不是"直接返回解析出的指针"，因为
 *   parent == 0 是**合法**输入（表示挂到当前活动屏幕，与 obj.c 一致），
 *   此时解析结果也是 NULL —— 与"父句柄无效"的 NULL 无法区分。
 *   若把两者混成一个 NULL，调用方就没法给出正确报错
 *   （实测这类混淆会让"传错句柄"表现为"挂在屏幕上"，问题被静默吞掉）。
 */
static int32_t widget_parent_of(int64_t parent, const char *fn_name, lv_obj_t **out)
{
    *out = NULL;
    if (parent == LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_OK; /* 显式表示"用当前活动屏幕" */
    }
    lv_obj_t *p = (lv_obj_t *)lvglcj_ptr_of(parent);
    if (p == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, parent, 0, fn_name,
                            "父对象句柄无效；若想挂到当前屏幕，请传 0");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    *out = p;
    return LVGLCJ_OK;
}

/*
 * 「创建 → 登记 → 挂 DELETE 钩子」三步。
 *
 * ★ 抽出来不是为了少写几行，而是**去掉三份拷贝各自漂移的可能**：
 *   这三步里漏掉挂钩子会产生悬空句柄（isAlive 为 true 而对象已删），
 *   这类问题极难定位；三份拷贝等于三次犯错机会。
 *   报错文本仍由各自函数提供（要带自己的函数名），所以这里只管机械动作。
 */
static int64_t widget_register_created(lv_obj_t *obj, const char *type_name)
{
    int64_t h = lvglcj_handle_register(obj, type_name);
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 登记失败必须把原生对象也删掉，否则原生对象泄漏 */
        lv_obj_delete(obj);
        return LVGLCJ_HANDLE_NULL;
    }
    lvglcj_lifecycle_install_hook(obj);
    return h;
}

/* ------------------------------------------------------------ switch */

int64_t lvglcj_switch_create(int64_t parent)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *sw = lv_switch_create(p);
    if (sw == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_switch_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }
    return widget_register_created(sw, "lv_switch_t");
}

/*
 * 勾选态用 LVGL 的 CHECKED 状态位实现，而不是控件自己的字段 ——
 * 这样样式系统（LV_STATE_CHECKED 选择器）与状态位天然一致，
 * 不会出现"控件认为已开、样式仍按未开渲染"的分叉。
 */
static int32_t widget_set_checked(int64_t handle, int32_t checked, const char *fn_name)
{
    LVGLCJ_HANDLE_GUARD(handle, fn_name);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(handle);
    if (checked) {
        lv_obj_add_state(o, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(o, LV_STATE_CHECKED);
    }
    return LVGLCJ_OK;
}

static int32_t widget_is_checked(int64_t handle, const char *fn_name)
{
    LVGLCJ_HANDLE_GUARD(handle, fn_name);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return lv_obj_has_state((lv_obj_t *)lvglcj_ptr_of(handle), LV_STATE_CHECKED) ? 1 : 0;
}

int32_t lvglcj_switch_set_checked(int64_t sw, int32_t checked)
{
    return widget_set_checked(sw, checked, __func__);
}

int32_t lvglcj_switch_is_checked(int64_t sw)
{
    return widget_is_checked(sw, __func__);
}

/* ------------------------------------------------------------ checkbox */

int64_t lvglcj_checkbox_create(int64_t parent)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *cb = lv_checkbox_create(p);
    if (cb == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_checkbox_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }
    return widget_register_created(cb, "lv_checkbox_t");
}

int32_t lvglcj_checkbox_set_text(int64_t cb, const char *text)
{
    LVGLCJ_HANDLE_GUARD(cb, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (text == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, cb, 0, __func__,
                            "文本指针为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /* 与 label_set_text 同理：LVGL 自己拷贝，仓颉侧的 CString 返回后即可释放 */
    lv_checkbox_set_text((lv_obj_t *)lvglcj_ptr_of(cb), text);
    return LVGLCJ_OK;
}

int32_t lvglcj_checkbox_set_checked(int64_t cb, int32_t checked)
{
    return widget_set_checked(cb, checked, __func__);
}

int32_t lvglcj_checkbox_is_checked(int64_t cb)
{
    return widget_is_checked(cb, __func__);
}

/* ------------------------------------------------------------ bar */

int64_t lvglcj_bar_create(int64_t parent)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *bar = lv_bar_create(p);
    if (bar == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_bar_create 失败（LVGL 内存池可能已满）");
        return LVGLCJ_HANDLE_NULL;
    }
    return widget_register_created(bar, "lv_bar_t");
}

int32_t lvglcj_bar_set_range(int64_t bar, int32_t min, int32_t max)
{
    LVGLCJ_HANDLE_GUARD(bar, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * LVGL 要求 min < max。这里显式拦住相反或相等的顺序：
     * 放行的话 LVGL 内部的取值范围换算会出现除零/反向映射，
     * 表现为"设了值但显示不对"这种很难归因的现象 ——
     * 与其让它变成一个显示问题，不如在这里变成一个明确报错。
     */
    if (min >= max) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, bar, min, __func__,
                            "范围要求 min < max；收到 min >= max");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_bar_set_range((lv_obj_t *)lvglcj_ptr_of(bar), min, max);
    return LVGLCJ_OK;
}

int32_t lvglcj_bar_set_value(int64_t bar, int32_t value)
{
    LVGLCJ_HANDLE_GUARD(bar, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* ★ 关动画：带动画时"设完立刻读值"读到的是中间态，见桥接头的约定 2 */
    lv_bar_set_value((lv_obj_t *)lvglcj_ptr_of(bar), value, LV_ANIM_OFF);
    return LVGLCJ_OK;
}

int32_t lvglcj_bar_get_value(int64_t bar, int32_t *out_value)
{
    LVGLCJ_HANDLE_GUARD(bar, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (out_value == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, bar, 0, __func__,
                            "出参指针为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    *out_value = lv_bar_get_value((lv_obj_t *)lvglcj_ptr_of(bar));
    return LVGLCJ_OK;
}
