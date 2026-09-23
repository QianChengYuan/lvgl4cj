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
                            "lv_label_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
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
                            "lv_button_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
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
int32_t lvglcj_widget_parent_of(int64_t parent, const char *fn_name, lv_obj_t **out)
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
int64_t lvglcj_widget_register_created(lv_obj_t *obj, const char *type_name)
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
    if (lvglcj_widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *sw = lv_switch_create(p);
    if (sw == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_switch_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_widget_register_created(sw, "lv_switch_t");

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
    if (lvglcj_widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *cb = lv_checkbox_create(p);
    if (cb == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_checkbox_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_widget_register_created(cb, "lv_checkbox_t");
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
    if (lvglcj_widget_parent_of(parent, __func__, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *bar = lv_bar_create(p);
    if (bar == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_bar_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_widget_register_created(bar, "lv_bar_t");
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

/* ============================================================ P1 批次 2（§7.1）
 *
 * slider / arc / led / spinner。
 *
 * ★ 这批有一半是"形状复用"：slider 与 arc 和上面的 bar 是**同一种形状**
 *   （值 + 范围），所以范围校验与取值读取抽成两个共用 helper，
 *   三者共用 —— 而不是把 bar 那段抄三遍。抄三遍的代价不是篇幅，
 *   是"三份实现各自漂移"，而范围校验一旦有一处漏掉，
 *   症状是"设了值但显示不对"这种极难归因的现象。
 */

/* bar / slider / arc 共用：范围必须 min < max */
static int32_t widget_set_int_range(int64_t h, int32_t lo, int32_t hi, const char *fn_name,
                                    void (*setter)(lv_obj_t *, int32_t, int32_t))
{
    LVGLCJ_HANDLE_GUARD(h, fn_name);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (lo >= hi) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, h, lo, fn_name,
                            "范围要求 min < max；收到 min >= max");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    setter((lv_obj_t *)lvglcj_ptr_of(h), lo, hi);
    return LVGLCJ_OK;
}

/* bar / slider / arc 共用：出参读取（均收 const lv_obj_t*） */
static int32_t widget_read_int(int64_t h, const char *fn_name, int32_t *out,
                               int32_t (*getter)(const lv_obj_t *))
{
    LVGLCJ_HANDLE_GUARD(h, fn_name);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (out == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, h, 0, fn_name,
                            "出参指针为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    *out = getter((const lv_obj_t *)lvglcj_ptr_of(h));
    return LVGLCJ_OK;
}

/* 三个控件共用的"创建 + 登记"外壳；各自只给出 LVGL 的创建函数与类型名 */
static int64_t widget_create_common(int64_t parent, const char *fn_name,
                                    lv_obj_t *(*creator)(lv_obj_t *), const char *type_name)
{
    int32_t rc = lvglcj_require_initialized(fn_name);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *p = NULL;
    if (lvglcj_widget_parent_of(parent, fn_name, &p) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    lv_obj_t *o = creator(p);
    if (o == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, fn_name,
                            "控件创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_widget_register_created(o, type_name);
}

/* ------------------------------------------------------------ slider */

int64_t lvglcj_slider_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_slider_create, "lv_slider_t");
}

int32_t lvglcj_slider_set_range(int64_t slider, int32_t min, int32_t max)
{
    return widget_set_int_range(slider, min, max, __func__, lv_slider_set_range);
}

int32_t lvglcj_slider_set_value(int64_t slider, int32_t value)
{
    LVGLCJ_HANDLE_GUARD(slider, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_slider_set_value((lv_obj_t *)lvglcj_ptr_of(slider), value, LV_ANIM_OFF);
    return LVGLCJ_OK;
}

int32_t lvglcj_slider_get_value(int64_t slider, int32_t *out_value)
{
    return widget_read_int(slider, __func__, out_value, lv_slider_get_value);
}

/* ------------------------------------------------------------ arc */

int64_t lvglcj_arc_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_arc_create, "lv_arc_t");
}

int32_t lvglcj_arc_set_range(int64_t arc, int32_t min, int32_t max)
{
    return widget_set_int_range(arc, min, max, __func__, lv_arc_set_range);
}

int32_t lvglcj_arc_set_value(int64_t arc, int32_t value)
{
    LVGLCJ_HANDLE_GUARD(arc, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    /* lv_arc_set_value 没有 anim 参数（与 slider 的差异，见桥接头） */
    lv_arc_set_value((lv_obj_t *)lvglcj_ptr_of(arc), value);
    return LVGLCJ_OK;
}

int32_t lvglcj_arc_get_value(int64_t arc, int32_t *out_value)
{
    return widget_read_int(arc, __func__, out_value, lv_arc_get_value);
}

/* ------------------------------------------------------------ led */

int64_t lvglcj_led_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_led_create, "lv_led_t");
}

int32_t lvglcj_led_set_color(int64_t led, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(led, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 与 Canvas 绘制类一致：0xRRGGBB，高 8 位不解释（LED 颜色本身不带 alpha） */
    lv_led_set_color((lv_obj_t *)lvglcj_ptr_of(led), lv_color_hex(color & 0xFFFFFFu));
    return LVGLCJ_OK;
}

int32_t lvglcj_led_set_brightness(int64_t led, int32_t brightness)
{
    LVGLCJ_HANDLE_GUARD(led, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* LVGL 收的是 uint8_t：越界会被静默截断，表现为"设 300 结果和 44 一样" */
    if (brightness < 0 || brightness > 255) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, led, brightness, __func__,
                            "亮度超出 0..255（LVGL 收 uint8_t，放行会静默截断）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_led_set_brightness((lv_obj_t *)lvglcj_ptr_of(led), (uint8_t)brightness);
    return LVGLCJ_OK;
}

int32_t lvglcj_led_set_on(int64_t led, int32_t on)
{
    LVGLCJ_HANDLE_GUARD(led, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(led);
    if (on) {
        lv_led_on(o);
    } else {
        lv_led_off(o);
    }
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ spinner */

int64_t lvglcj_spinner_create(int64_t parent)
{
    /* 无属性可设：旋转由 LVGL 内部的弧动画自行驱动，不经过本层 */
    return widget_create_common(parent, __func__, lv_spinner_create, "lv_spinner_t");
}

/* ------------------------------------------------------------ dropdown */

int64_t lvglcj_dropdown_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_dropdown_create, "lv_dropdown_t");
}

int32_t lvglcj_dropdown_set_options(int64_t dd, const char *options)
{
    LVGLCJ_HANDLE_GUARD(dd, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (options == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, dd, 0, __func__,
                            "选项字符串为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /*
     * ★ 只用 set_options（会拷贝），**绝不用 set_options_static**。
     *   后者要求这块内存一直有效，而仓颉侧传进来的 CString 在调用返回后即被释放
     *   —— 换成 static 版本就是立刻制造一个悬空指针，且不会当场报错。
     *   契约里没有提供那个入口，这里也就不存在"选错"的可能。
     */
    lv_dropdown_set_options((lv_obj_t *)lvglcj_ptr_of(dd), options);
    return LVGLCJ_OK;
}

int32_t lvglcj_dropdown_set_selected(int64_t dd, int32_t idx)
{
    LVGLCJ_HANDLE_GUARD(dd, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 底层收 uint32_t：负数会被隐式转成一个巨大的索引 */
    if (idx < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, dd, idx, __func__,
                            "选中项索引不能为负（底层收 uint32_t，放行会变成大整数）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_dropdown_set_selected((lv_obj_t *)lvglcj_ptr_of(dd), (uint32_t)idx);
    return LVGLCJ_OK;
}

int32_t lvglcj_dropdown_get_selected(int64_t dd)
{
    LVGLCJ_HANDLE_GUARD(dd, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return (int32_t)lv_dropdown_get_selected((lv_obj_t *)lvglcj_ptr_of(dd));
}

int32_t lvglcj_dropdown_get_option_count(int64_t dd)
{
    LVGLCJ_HANDLE_GUARD(dd, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return (int32_t)lv_dropdown_get_option_count((lv_obj_t *)lvglcj_ptr_of(dd));
}
