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
