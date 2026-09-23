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

/*
 * ★ line 的点集要用 malloc/free（LVGL 不拷贝点数组，缓冲归我们持有），
 *   所以这里必须显式包含 stdlib.h。
 *
 *   不包含时 gcc 只给 "implicit declaration of function 'calloc'" 这类**告警**，
 *   编译照样通过 —— 但隐式声明的返回值被当作 int，**64 位上会截断指针**。
 *   也就是说：一条被忽略的告警 = 一个只在大分配量下才现形的悬空指针。
 *
 *   这已经是第二次遇到（第一次在 native/src/canvas.c）：凡是新引入 malloc/free
 *   的文件都会踩一次。更好的做法是把它放进 lvglcj_internal.h 让所有 C 文件共享，
 *   那样这类问题就不存在"忘记包含"的可能 —— 列为待办，不在此处顺手改，
 *   因为改内部头会影响全部 22 个 C 文件，应当单独验证一次。
 */
#include <stdlib.h>

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

/* ------------------------------------------------------------ line */

/*
 * ★ line 与 canvas 是同一类：**LVGL 只保存地址，生命周期由我们负责**。
 *   LVGL 的 set_points 注释明写 "Only the address is saved, so the array needs
 *   to be alive while the line exists" —— 所以点数组必须由 C 侧持有。
 *   做法与 canvas 的像素缓冲完全一致（复用同一模式，不再另创一套）：
 *     · 指针存在**对象自身的 user_data** 里（没有容量上限这回事）
 *     · DELETE 时 free
 *     · 重设时先 free 旧的（否则旧的那块必然泄漏）
 */
static void line_free_points(lv_obj_t *obj)
{
    void *p = lv_obj_get_user_data(obj);
    if (p != NULL) {
        free(p);
        lv_obj_set_user_data(obj, NULL);
    }
}

static void line_delete_hook(lv_event_t *e)
{
    lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
    if (o != NULL) {
        line_free_points(o);
    }
}

int64_t lvglcj_line_create(int64_t parent)
{
    int64_t h = widget_create_common(parent, __func__, lv_line_create, "lv_line_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    /* 与 canvas 同理：释放钩子在 create 时挂上，避免"挂没挂过"这种状态 */
    (void)lv_obj_add_event_cb((lv_obj_t *)lvglcj_ptr_of(h), line_delete_hook, LV_EVENT_DELETE,
                              NULL);
    return h;
}

int32_t lvglcj_line_set_points(int64_t line, const int32_t *xy, int32_t point_count)
{
    LVGLCJ_HANDLE_GUARD(line, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (xy == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, line, 0, __func__,
                            "坐标数组为 NULL");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    if (point_count <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, line, point_count, __func__,
                            "点集至少要有 1 个点");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    int64_t bytes = (int64_t)point_count * (int64_t)sizeof(lv_point_precise_t);
    if (bytes > (int64_t)INT32_MAX) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, line, point_count, __func__,
                            "点集过大（超过 2GB）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(line);

    /* ★ 重设先释放旧点集 —— 改数据是常见操作，漏这一步必定泄漏 */
    line_free_points(o);

    lv_point_precise_t *pts = (lv_point_precise_t *)calloc((size_t)bytes, 1);
    if (pts == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, line, point_count, __func__,
                            "点集分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }
    /* 扁平 int32 坐标对 → lv_point_precise_t（成员类型随上游配置，故在此转换一次，
     * 不把它的布局暴露到 ABI 上，见桥接头说明） */
    for (int32_t i = 0; i < point_count; i++) {
        pts[i].x = xy[i * 2];
        pts[i].y = xy[i * 2 + 1];
    }

    lv_obj_set_user_data(o, pts);
    lv_line_set_points(o, pts, (uint32_t)point_count);
    return LVGLCJ_OK;
}

int32_t lvglcj_line_set_y_invert(int64_t line, int32_t on)
{
    LVGLCJ_HANDLE_GUARD(line, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_line_set_y_invert((lv_obj_t *)lvglcj_ptr_of(line), on ? true : false);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ image */

int64_t lvglcj_image_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_image_create, "lv_image_t");
}

int32_t lvglcj_image_set_src(int64_t img, const char *path)
{
    LVGLCJ_HANDLE_GUARD(img, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * ★ path == NULL 是**合法**输入，表示清空图像（LVGL 会走 UNKNOWN 分支、
     *   释放旧的源并把 src 置空）。这里刻意不套用"NULL 即错误"的惯例：
     *   图像没有"空字符串"这种表达方式，NULL 是表达"没有图像"的唯一途径；
     *   而文本类 setter 用 NULL 是错误，因为那里 "" 才是"空文本"。
     *
     * ★ 安全性：LVGL 对 FILE 类型执行 lv_strdup(src)，会拷贝一份并由它自己释放，
     *   所以调用方的字符串在返回后即可销毁（与 dropdown 的选项同性质）。
     *   这也是本层不提供描述符形态的原因所在（VARIABLE 分支只存指针不拷贝）。
     */
    lv_image_set_src((lv_obj_t *)lvglcj_ptr_of(img), path);
    return LVGLCJ_OK;
}

int32_t lvglcj_image_set_offset(int64_t img, int32_t x, int32_t y)
{
    LVGLCJ_HANDLE_GUARD(img, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(img);
    lv_image_set_offset_x(o, x);
    lv_image_set_offset_y(o, y);
    return LVGLCJ_OK;
}

int32_t lvglcj_image_set_scale(int64_t img, int32_t zoom)
{
    LVGLCJ_HANDLE_GUARD(img, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * 底层收 uint32_t，所以负数会被隐式转成巨大的缩放值（画面炸掉且不报错）。
     * 0 同样拦下：它不是"无缩放"（那是 256），而是一个退化的取值。
     */
    if (zoom <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, img, zoom, __func__,
                            "缩放值必须为正；注意单位不是百分比，256 = 100%");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_image_set_scale((lv_obj_t *)lvglcj_ptr_of(img), (uint32_t)zoom);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ roller */

int64_t lvglcj_roller_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_roller_create, "lv_roller_t");
}

int32_t lvglcj_roller_set_options(int64_t roller, const char *options, int32_t mode)
{
    LVGLCJ_HANDLE_GUARD(roller, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * NULL 必须在这里拦下：实现里有 LV_ASSERT_NULL(options)，
     * 放过去就是一次直接断言（Release 下则是解引用空指针）。
     * 这与 image 的 set_src(NULL)=清空 恰好相反 —— 同一个库里的两种约定，
     * 只能逐个读实现来确认，不能按"NULL 通常表示清空"外推。
     */
    if (options == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, roller, 0, __func__,
                            "options 不能为空；roller 没有“清空选项”的语义，要空就传不含换行的串");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    if (mode != LVGLCJ_ROLLER_MODE_NORMAL && mode != LVGLCJ_ROLLER_MODE_INFINITE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, roller, mode, __func__,
                            "mode 必须是 LVGLCJ_ROLLER_MODE_NORMAL 或 LVGLCJ_ROLLER_MODE_INFINITE");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_roller_set_options((lv_obj_t *)lvglcj_ptr_of(roller), options,
                          mode == LVGLCJ_ROLLER_MODE_NORMAL ? LV_ROLLER_MODE_NORMAL
                                                            : LV_ROLLER_MODE_INFINITE);
    return LVGLCJ_OK;
}

int32_t lvglcj_roller_set_selected(int64_t roller, int32_t sel)
{
    LVGLCJ_HANDLE_GUARD(roller, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(roller);

    /*
     * 范围校验是本层的责任，不是底层的：lv_roller_set_selected 收 uint32_t
     * 且**不钳制** —— 越界不报错，只是把内部 id 设大，等到渲染或取串时才越界读。
     * 那种失败会出现在远离调用点的地方，代价远高于在这里拦一下。
     *
     * ★ 这里用 lv_roller_get_option_count() 作基准，它在**两种模式下都返回对外语义的
     *   选项数**：无限模式内部把 option_cnt 乘过 inf_page_cnt，而这个 getter 会除回去
     *   （option_cnt / inf_page_cnt）。所以校验不需要按模式分支 ——
     *   但这是读实现才敢下的结论，不是可以从名字推出来的（"内部计数"与"对外计数"
     *   在无限模式下确实不是同一个数）。
     */
    int32_t n = (int32_t)lv_roller_get_option_count(o);
    if (sel < 0 || sel >= n) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, roller, sel, __func__,
                            "选中索引超出选项范围（须满足 0 <= sel < 选项数）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /* 关闭动画：开动画时选中值要等若干帧之后才稳定，
     * 会让"设完立刻读回"这种确定性用法变成时序赌局。 */
    lv_roller_set_selected(o, (uint32_t)sel, LV_ANIM_OFF);
    return LVGLCJ_OK;
}

int32_t lvglcj_roller_get_selected(int64_t roller)
{
    LVGLCJ_HANDLE_GUARD(roller, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return (int32_t)lv_roller_get_selected((lv_obj_t *)lvglcj_ptr_of(roller));
}

int32_t lvglcj_roller_set_visible_row_count(int64_t roller, int32_t rows)
{
    LVGLCJ_HANDLE_GUARD(roller, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (rows <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, roller, rows, __func__,
                            "可见行数必须为正；0 会得到一个零高度的控件");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_roller_set_visible_row_count((lv_obj_t *)lvglcj_ptr_of(roller), (uint32_t)rows);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ textarea */

int64_t lvglcj_textarea_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_textarea_create, "lv_textarea_t");
}

int32_t lvglcj_textarea_set_text(int64_t ta, const char *txt)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 与 label 一致：文本 setter 不收 NULL（空文本用 "" 表达）。
     * LVGL 侧不保证对 NULL 友好，放进去等于把一次空指针解引用留给它。 */
    if (txt == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, ta, 0, __func__,
                            "文本不能为空指针；空文本请传空串");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_textarea_set_text((lv_obj_t *)lvglcj_ptr_of(ta), txt);
    return LVGLCJ_OK;
}

int32_t lvglcj_textarea_get_text(int64_t ta, char *buf, int32_t size)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    const char *txt = lv_textarea_get_text((lv_obj_t *)lvglcj_ptr_of(ta));
    if (txt == NULL) {
        /* 语义上不该发生（LVGL 对空文本返回 ""），但这里若真拿到 NULL，
         * 下面就是一次解引用 —— 一个 strlen 的代价换掉这种可能，是划算的。 */
        txt = "";
    }

    size_t n = strlen(txt); /* strlen 由 lvglcj_internal.h 统一提供（见该处的说明） */

    if (buf == NULL) {
        return (int32_t)n; /* 探测用法：只回报所需长度 */
    }
    if (size <= (int32_t)n) {
        /*
         * 缓冲不足：**一个字都不写**并报错。
         * 选择报错而不是截断，是因为截断的后果更隐蔽 ——
         * 调用方会拿到一个语法完全合法、但内容被悄悄削短的字符串，
         * 然后在很远的地方表现出"文本不对"。报错的代价只是多一次调用。
         */
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, ta, size, __func__,
                            "缓冲不足：需要 文本字节数+1（含结尾 NUL）；本次未写入任何内容");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    memcpy(buf, txt, n);
    buf[n] = '\0';
    return (int32_t)n;
}

int32_t lvglcj_textarea_set_placeholder_text(int64_t ta, const char *txt)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (txt == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, ta, 0, __func__,
                            "占位文本不能为空指针");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_textarea_set_placeholder_text((lv_obj_t *)lvglcj_ptr_of(ta), txt);
    return LVGLCJ_OK;
}

int32_t lvglcj_textarea_set_one_line(int64_t ta, int32_t on)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_textarea_set_one_line((lv_obj_t *)lvglcj_ptr_of(ta), on ? true : false);
    return LVGLCJ_OK;
}

int32_t lvglcj_textarea_set_password_mode(int64_t ta, int32_t on)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_textarea_set_password_mode((lv_obj_t *)lvglcj_ptr_of(ta), on ? true : false);
    return LVGLCJ_OK;
}

int32_t lvglcj_textarea_set_max_length(int64_t ta, int32_t len)
{
    LVGLCJ_HANDLE_GUARD(ta, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 底层收 uint32_t：负数会被隐式转成巨大的上限，等于"悄悄变成不限制" ——
     * 与调用方想表达的正好相反，且不会有任何提示。0 则是 LVGL 的"不限制"约定，放行。 */
    if (len < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, ta, len, __func__,
                            "最大长度不能为负（0 才表示不限制）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_textarea_set_max_length((lv_obj_t *)lvglcj_ptr_of(ta), (uint32_t)len);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ table */

int64_t lvglcj_table_create(int64_t parent)
{
    return widget_create_common(parent, __func__, lv_table_create, "lv_table_t");
}

int32_t lvglcj_table_set_cell_value(int64_t tbl, int32_t row, int32_t col, const char *txt)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (txt == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, 0, __func__,
                            "单元格文本不能为空指针；空单元格请传空串");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /*
     * ★★ 负值必须在这里拦下 —— 这一处比"索引越界"严重得多。
     *
     *   LVGL 对超出当前规模的行列是**自动扩容**的（文档明确写了 "New rows/columns
     *   are added automatically if required"）。而负数转成 uint32_t 会变成约 42 亿，
     *   于是「设一个单元格」就变成「申请一张 42 亿列的表」——
     *   不是越界读，是当场把内存吃光。校验放在调用点之外就没有意义了。
     */
    if (row < 0 || col < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, row, __func__,
                            "行列索引不能为负：LVGL 会自动扩容，负值转 uint32_t 会变成极大值");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_table_set_cell_value((lv_obj_t *)lvglcj_ptr_of(tbl), (uint32_t)row, (uint32_t)col, txt);
    return LVGLCJ_OK;
}

int32_t lvglcj_table_get_cell_value(int64_t tbl, int32_t row, int32_t col, char *buf, int32_t size)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(tbl);
    int32_t rows = (int32_t)lv_table_get_row_count(o);
    int32_t cols = (int32_t)lv_table_get_column_count(o);

    /*
     * 取用**不**自动扩容（底层会断言，且语义上也不该由"读"改变对象结构）：
     * 若这里也放行，"读错一格"会变成静默把表格撑大，之后再读别的格子就已经不是
     * 原来的表了。宁可报错。
     */
    if (row < 0 || col < 0 || row >= rows || col >= cols) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, row, __func__,
                            "索引超出当前行列范围（取用不会自动扩容）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    const char *txt = lv_table_get_cell_value(o, (uint32_t)row, (uint32_t)col);
    /*
     * ★ 这个分支实际到不了：读实现可见 lv_table_get_cell_value 对**空格子**与
     *   **越界索引**一律返回空串（""，不是 NULL）。保留它只是让"下游解引用空指针"
     *   不重新成为一种可能，成本为零。
     *
     * ★ 也正因为它把"越界"和"空格子"都表达成 ""、从返回值上无从区分，
     *   本层才坚持自己先判范围：那一步把"读错一格"与"读到了空格子"还给调用方区分开。
     */
    if (txt == NULL) {
        txt = "";
    }
    size_t n = strlen(txt);

    if (buf == NULL) {
        return (int32_t)n; /* 探测用法 */
    }
    if (size <= (int32_t)n) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, size, __func__,
                            "缓冲不足：需要 文本字节数+1（含结尾 NUL）；本次未写入任何内容");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    memcpy(buf, txt, n);
    buf[n] = '\0';
    return (int32_t)n;
}

int32_t lvglcj_table_set_row_count(int64_t tbl, int32_t rows)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (rows < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, rows, __func__,
                            "行数不能为负（0 可以，相当于清空）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_table_set_row_count((lv_obj_t *)lvglcj_ptr_of(tbl), (uint32_t)rows);
    return LVGLCJ_OK;
}

/*
 * ★★ 上游缺陷（LVGL v9.2.2）：把列数**调小**会段错误。gdb 回溯定在
 *       lv_table_set_column_count          lv_table.c:280
 *       if(table->cell_data[idx]->user_data)        ← 解引用 NULL
 *
 *   成因：cell_data[cell] 只在**该格被设置过**时才分配，未设过的格子是 NULL；
 *   而缩列时它遍历"将被丢弃的格子"并直接解引用 —— 未设过的格子恰好是 NULL。
 *   触发条件平凡到不能不管：「先设一格把表撑开，再把列数调小」。
 *
 *   ★ 判定这是上游缺陷（而非我们误用）有一条硬证据：
 *     兄弟函数 lv_table_set_row_count 做同一件事时**有** NULL 检查
 *         if(table->cell_data[i] && table->cell_data[i]->user_data) { ... }
 *     而缩列这条路径**没有**。同一文件里两个函数对同一种情况处理不一致。
 *
 *   处置：版本是冻结的，不改第三方源码，改为在缩小之前把所有将被丢弃的格子
 *   填成空串，使 cell_data 不再是 NULL，于是上游那条路径拿到的是合法指针。
 *   代价是 O(丢弃格数) 次填充，且只在缩小时发生。
 *   回归用例 testTableShrinkMustNotCrash 钉住它 —— 修复前该处正是段错误。
 */
static void table_prefill_cols_to_drop(lv_obj_t *o, int32_t keep_cols)
{
    int32_t rows = (int32_t)lv_table_get_row_count(o);
    int32_t cols = (int32_t)lv_table_get_column_count(o);

    for (int32_t r = 0; r < rows; r++) {
        for (int32_t c = keep_cols; c < cols; c++) {
            /*
             * ★ 这里**无条件**写空串，不先去"判断该格是否为空"。
             *
             *   第一版写的是 `if (lv_table_get_cell_value(...) == NULL) { ... }`，
             *   结果规避没生效、照样段错误。原因是那个判断依据不可靠：
             *   lv_table_get_cell_value 给出的是**文本视角**的结果，
             *   而崩溃点看的是 cell_data[cell] 这个**指针**是否为 NULL ——
             *   两者不是一回事，用前者推后者就是这次出错的地方。
             *
             *   而这些格子本来就即将被丢弃，写空串没有语义代价；
             *   ctrl 与 user_data 也会被 set_cell_value 保留（其实现里先存后恢复），
             *   所以 user_data 的释放路径不受影响。
             *   取舍：用"无条件做一件无害的事"换掉"有条件地做一件关键的事" ——
             *   可靠性优先于省几次内存写。另外目标格仍在当前行列范围内，不会触发自动扩容。
             */
            lv_table_set_cell_value(o, (uint32_t)r, (uint32_t)c, "");
        }
    }
}

int32_t lvglcj_table_set_column_count(int64_t tbl, int32_t cols)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (cols < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, cols, __func__,
                            "列数不能为负（0 可以，相当于清空）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(tbl);

    /* 只在缩小（会走到上游那条缺 NULL 检查的路径）时做前置填充。
     * 扩容路径不涉及释放，无此问题。 */
    if (cols < (int32_t)lv_table_get_column_count(o)) {
        table_prefill_cols_to_drop(o, cols);
    }

    lv_table_set_column_count(o, (uint32_t)cols);
    return LVGLCJ_OK;
}

int32_t lvglcj_table_get_row_count(int64_t tbl)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return (int32_t)lv_table_get_row_count((lv_obj_t *)lvglcj_ptr_of(tbl));
}

int32_t lvglcj_table_get_column_count(int64_t tbl)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    return (int32_t)lv_table_get_column_count((lv_obj_t *)lvglcj_ptr_of(tbl));
}

/*
 * 控制位：行/列同样要挡住负值（理由与 set_cell_value 一致 —— 底层收 uint32_t）。
 * ctrl 本身不做位校验：LVGL 定义的就是"若干位按位或"，未知位它只是存着不处理，
 * 拦下来反而会让将来新增的位在旧版本里不可用。
 */
int32_t lvglcj_table_add_cell_ctrl(int64_t tbl, int32_t row, int32_t col, int32_t ctrl)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (row < 0 || col < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, row, __func__,
                            "行列索引不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_table_add_cell_ctrl((lv_obj_t *)lvglcj_ptr_of(tbl), (uint32_t)row, (uint32_t)col,
                           (lv_table_cell_ctrl_t)ctrl);
    return LVGLCJ_OK;
}

int32_t lvglcj_table_clear_cell_ctrl(int64_t tbl, int32_t row, int32_t col, int32_t ctrl)
{
    LVGLCJ_HANDLE_GUARD(tbl, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (row < 0 || col < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, tbl, row, __func__,
                            "行列索引不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_table_clear_cell_ctrl((lv_obj_t *)lvglcj_ptr_of(tbl), (uint32_t)row, (uint32_t)col,
                             (lv_table_cell_ctrl_t)ctrl);
    return LVGLCJ_OK;
}
