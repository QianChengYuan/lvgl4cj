/*
 * event.c —— 事件注册 / 注销 / 发送 / 查询（设计文档 §5.6 / §3.7）
 *
 * 三个关键点：
 *   1. **DRAW_* 事件直接拒绝**（§3.7.3）：lv_draw_* 是明确的非目标（ADR-005），
 *      与其让用户注册后拿到未定义行为，不如在注册时返回 NOT_SUPPORTED(-13)
 *   2. **target 与 current_target 必须分开**（§3.7.1）：
 *      target 是冒泡链起点，current_target 是当前处理者，两者常见地不相等
 *   3. **param 双路径改为两个显式入口**（见下）
 *
 * ★ 与设计文档 §5.6 的一处有据偏离：
 *   原文用单一 lvglcj_obj_send_event(obj, code, param) 承载两条语义
 *   （param 既可能是句柄，也可能是编码后的标量），由「注册侧决定、接收侧用对应 API 取回」。
 *   问题：发送侧无法在 C 层自动区分二者 —— 若用「param 是否命中有效句柄」来判断，
 *   会发生「标量恰好等于某个句柄 ID」的静默误判。
 *   因此拆成两个显式入口：
 *     lvglcj_obj_send_event(...)          句柄路径（param 是句柄，转成裸指针）
 *     lvglcj_obj_send_event_scalar(...)   标量路径（经 intptr_t 原样传递）
 *   接收侧仍用 lvglcj_event_get_user_data / lvglcj_event_get_param_scalar 对应取回。
 */
#include "lvglcj_internal.h"

/* ------------------------------------------- DRAW_* 拒绝（§3.7.3） */
static int event_is_draw_code(int32_t code)
{
    switch ((lv_event_code_t)code) {
        case LV_EVENT_DRAW_MAIN_BEGIN:
        case LV_EVENT_DRAW_MAIN:
        case LV_EVENT_DRAW_MAIN_END:
        case LV_EVENT_DRAW_POST_BEGIN:
        case LV_EVENT_DRAW_POST:
        case LV_EVENT_DRAW_POST_END:
            return 1;
        default:
            return 0;
    }
}

/* ------------------------------------------------------------ 注册 */
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int32_t cid)
{
    int32_t rc = lvglcj_handle_require(obj, __func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (event_is_draw_code(code)) {
        /* 明确拒绝而不是静默接受：绘制事件是设计文档 §1.4 的非目标 */
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, obj, cid, __func__,
                            "LV_EVENT_DRAW_* 不在支持范围内（用 LvCanvas 替代，P2 落地）");
        return LVGLCJ_HANDLE_NULL;
    }
    if (cid <= LVGLCJ_CID_NONE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, obj, cid, __func__,
                            "closure_id 必须为正（0 保留给「无闭包」）");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(obj);
    if (o == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    lv_event_dsc_t *dsc = lv_obj_add_event_cb(o, lvglcj_event_trampoline,
                                             (lv_event_code_t)code,
                                             lvglcj_cid_to_ptr(cid));
    if (dsc == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, obj, cid, __func__,
                            "lv_obj_add_event_cb 失败");
        return LVGLCJ_HANDLE_NULL;
    }

    /*
     * 登记到桥接层自己的注册表（而非依赖 LVGL 事件数组）：
     * DELETE 钩子要在对象删除过程中拿到「该对象有哪些闭包」，
     * 那时 LVGL 内部状态是否完整属于实现细节，不该依赖（见 lvglcj_internal.h）。
     */
    if (lvglcj_reg_event_add(obj, dsc, cid) != LVGLCJ_OK) {
        /* 登记失败则回滚，避免出现「LVGL 有回调但注册表不知道」的不一致 */
        (void)lv_obj_remove_event_dsc(o, dsc);
        return LVGLCJ_HANDLE_NULL;
    }

    /* 事件描述符也给一个句柄，供 lvglcj_obj_remove_event 精确注销 */
    return lvglcj_handle_register(dsc, "lv_event_dsc_t");
}

/* ------------------------------------------------------------ 注销 */
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t ev_dsc)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_HANDLE_GUARD(ev_dsc, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(obj);
    lv_event_dsc_t *dsc = (lv_event_dsc_t *)lvglcj_ptr_of(ev_dsc);
    if (o == NULL || dsc == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    int32_t cid = lvglcj_ptr_to_cid(lv_event_dsc_get_user_data(dsc));

    lvglcj_reg_event_remove_by_dsc(dsc);
    (void)lv_obj_remove_event_dsc(o, dsc);

    /* 载体已被移除 → 通知仓颉侧注销闭包（防泄漏） */
    lvglcj_closure_unregister(cid);

    lvglcj_handle_invalidate(ev_dsc);
    lvglcj_handle_release(ev_dsc);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_remove_event_by_cid(int64_t obj, int32_t cid)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (cid <= LVGLCJ_CID_NONE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, obj, cid, __func__,
                            "closure_id 必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(obj);
    if (o == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    /*
     * 用 (trampoline, user_data) 组合精确移除。
     * 注意只移除我们的 trampoline，不会误删用户自己用 LVGL 原生 API 注册的回调。
     */
    uint32_t removed = lv_obj_remove_event_cb_with_user_data(o, lvglcj_event_trampoline,
                                                            lvglcj_cid_to_ptr(cid));
    (void)removed;

    lvglcj_closure_unregister(cid);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 发送 */
int32_t lvglcj_obj_send_event(int64_t obj, int32_t code, int64_t param_handle)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (event_is_draw_code(code)) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, obj, 0, __func__,
                            "LV_EVENT_DRAW_* 不在支持范围内");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    void *param = NULL;
    if (param_handle != LVGLCJ_HANDLE_NULL) {
        param = lvglcj_ptr_of(param_handle);
        if (param == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, param_handle, 0, __func__,
                                "param 句柄无效");
            return LVGLCJ_ERR_INVALID_HANDLE;
        }
    }

    lv_obj_send_event((lv_obj_t *)lvglcj_ptr_of(obj), (lv_event_code_t)code, param);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_send_event_scalar(int64_t obj, int32_t code, int64_t param_scalar)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (event_is_draw_code(code)) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, obj, 0, __func__,
                            "LV_EVENT_DRAW_* 不在支持范围内");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    /* 显式经 intptr_t 编码（ADR-009：仅 64 位，标量可完整装入） */
    lv_obj_send_event((lv_obj_t *)lvglcj_ptr_of(obj), (lv_event_code_t)code,
                      (void *)(intptr_t)param_scalar);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 查询 */
/* 内部：由事件句柄取 lv_event_t* */
static lv_event_t *event_of(int64_t evh, const char *func)
{
    if (lvglcj_handle_require(evh, func) != LVGLCJ_OK) {
        return NULL;
    }
    return (lv_event_t *)lvglcj_ptr_of(evh);
}

int32_t lvglcj_event_get_code(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    return (e != NULL) ? (int32_t)lv_event_get_code(e) : 0;
}

int64_t lvglcj_event_get_target(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    /* 只返回已登记对象；LVGL 内部对象返回 0（§3.1.5 例外） */
    return lvglcj_handle_of(lv_event_get_target(e));
}

int64_t lvglcj_event_get_current_target(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_handle_of(lv_event_get_current_target(e));
}

/*
 * ★ 与设计文档 §5.6 的第二处有据偏离（重要）：
 *   原文的「句柄路径」是「param 放句柄 → 接收侧用 lvglcj_event_get_user_data 取回」。
 *   但分支 B **已经用 user_data 装载 closure_id**（那是 trampoline 找回闭包的唯一凭据），
 *   二者不可能共存；而且 LVGL v9.2 也没有 lv_event_set_user_data 这种运行期设置接口。
 *
 *   正确做法：承载载荷一律走 **param**。因此提供两个对称的读取入口：
 *     lvglcj_event_get_param_handle()   —— 发送侧用「句柄路径」时的读取方式
 *     lvglcj_event_get_param_scalar()   —— 发送侧用「标量路径」时的读取方式
 *   并额外提供 closure_id 的读取入口用于诊断。
 */
int64_t lvglcj_event_get_param_handle(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    void *p = lv_event_get_param(e);
    if (p == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    return lvglcj_handle_of(p);
}

/* 诊断用：当前正在处理该事件的是哪个闭包 */
int32_t lvglcj_event_get_closure_id(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_CID_NONE;
    }
    return lvglcj_ptr_to_cid(lv_event_get_user_data(e));
}

int64_t lvglcj_event_get_param_scalar(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return 0;
    }
    /* 标量路径：param 经 intptr_t 编码 */
    return (int64_t)(intptr_t)lv_event_get_param(e);
}

int32_t lvglcj_event_stop_bubbling(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_event_stop_bubbling(e);
    return LVGLCJ_OK;
}

/*
 * LVGL v9.2 **没有** lv_event_stop_trickling()；对应能力是
 * lv_event_stop_processing()（停止本次事件的后续处理）。
 * 因此对外 API 名也从 stop_trickling 改为 stop_processing ——
 * 名字要与真实语义一致，否则用户会以为它只影响 trickle 方向。
 */
int32_t lvglcj_event_stop_processing(int64_t evh)
{
    lv_event_t *e = event_of(evh, __func__);
    if (e == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_event_stop_processing(e);
    return LVGLCJ_OK;
}
