/*
 * canvas.c —— Canvas 控件（设计文档 §3.11.2「替代路径：Canvas 控件」/ §6.5）
 *
 * ==================== 为什么它值得单独一个文件 ====================
 * §3.11.1 把 lv_draw_* 明确列为**非目标**：本项目不打算让用户直接接触绘制层。
 * §3.11.2 给出的替代路径就是 Canvas。所以这个文件不是"又补了一个控件"，
 * 而是**自定义绘制的唯一出口** —— 它的完成度直接决定用户能不能画自己的东西。
 *
 * ==================== 本文件真正需要设计的一处：缓冲归谁 ====================
 * LVGL 的 lv_canvas_set_buffer() 要求调用方提供像素缓冲，
 * 而我们的 ABI 是 set_buffer(canvas, w, h) —— **只给尺寸，不给指针**。
 * 这是刻意的（ADR-002：绝不把裸指针交给仓颉，GC 会移动缓冲），
 * 于是缓冲必须由 C 侧分配，并与 canvas 同生命周期。两条决定：
 *
 *   1. 指针存在**对象自身的 user_data** 里，不引入旁路表。
 *      这条是有教训的：font.c 曾用固定容量（8）的数组登记内置字体，
 *      满了就静默不再记录，最终导致内置字体被误释放而挂死。
 *      凡是"容量有限的旁路表"都要先问一句「满了怎么办」——
 *      user_data 根本没有容量问题，它与对象同生共死。
 *      （对象 user_data 是空闲的：生命周期钩子用的是**事件** user_data，不是它。）
 *   2. 在 LV_EVENT_DELETE 释放；set_buffer 重复调用时先释放旧的，
 *      否则旧的那块必然泄漏（这是最容易漏的一条：改尺寸是常见操作）。
 *
 * ==================== 一处必须写在注释里的 ABI 不一致 ====================
 * 这些函数都收 uint32_t 颜色，但**解释方式不同**：
 *   · 绘制类（point/line/rect/arc）与 fill_bg：0xRRGGBB，高 8 位**不解释**；
 *     透明度由 fill_bg 的 opa 参数单独给出，其余绘制类恒为不透明。
 *   · set_palette：0xAARRGGBB —— 调色板项本身就是带 alpha 的 32 位色
 *     （lv_color32_t 的成员是 blue/green/red/alpha），没有"单独的 opa 参数"可放。
 * 这是 ABI 层面的不一致，不是笔误。写成注释而不是"顺手统一"，
 * 因为统一会悄悄改变**已声明签名**的语义；真要改必须走契约变更。
 *
 * ==================== 另一处契约限制 ====================
 * 线宽没有暴露在 ABI 里（draw_line / draw_arc 都没有 width 参数），
 * 因此本实现固定为 1（line）与 2（arc）。需要别的线宽要扩契约，
 * 这里如实写明而不是偷偷让它取某个 LVGL 的内部默认值。
 */
#include "lvglcj_internal.h"

/*
 * ★ 必须显式包含 stdlib.h。
 *   不包含时 gcc 只给"implicit declaration of function 'calloc'"这类**告警**，
 *   编译仍会成功 —— 但隐式声明的返回值被当作 int，在 64 位上**会截断指针**。
 *   也就是说：一条被忽略的告警 = 一个只在大分配量下才现形的悬空指针。
 *   （本项目的零告警纪律不是洁癖，这条就是它的实例。）
 */
#include <stdlib.h>

/* ABI 的 0xRRGGBB → lv_color_t。显式屏蔽高 8 位，避免把 0xAARRGGBB 的 alpha 当成颜色位 */
static lv_color_t canvas_color_rgb(uint32_t c)
{
    return lv_color_hex(c & 0xFFFFFFu);
}

static void canvas_free_buffer(lv_obj_t *obj)
{
    void *buf = lv_obj_get_user_data(obj);
    if (buf != NULL) {
        free(buf);
        lv_obj_set_user_data(obj, NULL);
    }
}

/*
 * DELETE 时释放缓冲。
 * 它与 lvglcj_delete_hook（句柄级联失效）是两个独立的 DELETE 处理器，
 * 互不依赖、顺序也无关紧要 —— 一个管句柄表，一个管这块内存。
 */
static void canvas_delete_hook(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (obj != NULL) {
        canvas_free_buffer(obj);
    }
}

/*
 * 「取对象 + 校验已设缓冲 + init layer」。
 * line / rect / arc 三者的正式绘制步骤不同，但这一步完全相同 ——
 * 尤其"没设缓冲就绘制"这条守卫，抽出来是为了三处都有、且不会有一处漏掉。
 */
static int32_t canvas_begin_draw(int64_t canvas, const char *fn_name,
                                 lv_obj_t **out_obj, lv_layer_t *layer)
{
    *out_obj = (lv_obj_t *)lvglcj_ptr_of(canvas);
    if (lv_obj_get_user_data(*out_obj) == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, 0, fn_name,
                            "尚未设置像素缓冲：请先调用 setBuffer，否则绘制会写到空指针");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_canvas_init_layer(*out_obj, layer);
    return LVGLCJ_OK;
}

int64_t lvglcj_canvas_create(int64_t parent)
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

    lv_obj_t *c = lv_canvas_create(p);
    if (c == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, parent, 0, __func__,
                            "lv_canvas_create 失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）");
        return LVGLCJ_HANDLE_NULL;
    }

    /*
     * 释放钩子在 create 时挂上，而不是等 set_buffer。
     * 后者要额外处理"钩子挂了没有"这个状态，而 create 时挂是幂等且无状态的。
     */
    (void)lv_obj_add_event_cb(c, canvas_delete_hook, LV_EVENT_DELETE, NULL);
    return lvglcj_widget_register_created(c, "lv_canvas_t");
}

int32_t lvglcj_canvas_set_buffer(int64_t canvas, int32_t w, int32_t h)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * 尺寸校验只针对**不可能/不安全**的情形，不设"经验上限"：
     * 上限定多少都是拍脑袋，而"传 0 或负数"与"乘法溢出"是实打实的错误。
     */
    if (w <= 0 || h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, w, __func__,
                            "缓冲区尺寸必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    int64_t bytes = (int64_t)w * (int64_t)h * 4; /* ARGB8888 = 4 字节/像素 */
    if (bytes > (int64_t)INT32_MAX) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, w, __func__,
                            "缓冲区尺寸过大（ARGB8888 下超过 2GB）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(canvas);

    /* ★ 重复设置先释放旧缓冲 —— 改尺寸是常见操作，漏这一步必定泄漏 */
    canvas_free_buffer(o);

    void *buf = calloc((size_t)bytes, 1);
    if (buf == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, canvas, w, __func__,
                            "像素缓冲分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    lv_obj_set_user_data(o, buf);
    lv_canvas_set_buffer(o, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_set_palette(int64_t canvas, int32_t idx, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (idx < 0 || idx > 255) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, idx, __func__,
                            "调色板索引超出 0..255");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /* 调色板项是 32 位色、含 alpha —— 与绘制类的 0xRRGGBB 不同，见文件头
     * （lv_color32_t 的成员依次是 blue/green/red/alpha，不要按名字想当然） */
    lv_color32_t c32;
    c32.blue = (uint8_t)(color & 0xFFu);
    c32.green = (uint8_t)((color >> 8) & 0xFFu);
    c32.red = (uint8_t)((color >> 16) & 0xFFu);
    c32.alpha = (uint8_t)((color >> 24) & 0xFFu);
    lv_canvas_set_palette((lv_obj_t *)lvglcj_ptr_of(canvas), (uint8_t)idx, c32);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_fill_bg(int64_t canvas, uint32_t color, int32_t opa)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (opa < 0 || opa > 255) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, opa, __func__,
                            "opa 超出 0..255");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(canvas);
    if (lv_obj_get_user_data(o) == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, 0, __func__,
                            "尚未设置像素缓冲：请先调用 setBuffer");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_canvas_fill_bg(o, canvas_color_rgb(color), (lv_opa_t)opa);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_draw_point(int64_t canvas, int32_t x, int32_t y, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = (lv_obj_t *)lvglcj_ptr_of(canvas);
    if (lv_obj_get_user_data(o) == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, 0, __func__,
                            "尚未设置像素缓冲：请先调用 setBuffer");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /* v9 的 set_px 多了 opa 参数（v8 没有）—— 单点绘制恒为不透明 */
    lv_canvas_set_px(o, x, y, canvas_color_rgb(color), LV_OPA_COVER);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_draw_line(int64_t canvas, int32_t x1, int32_t y1, int32_t x2,
                                int32_t y2, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_obj_t *o = NULL;
    lv_layer_t layer;
    int32_t rc = canvas_begin_draw(canvas, __func__, &o, &layer);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = canvas_color_rgb(color);
    dsc.width = 1; /* ABI 未暴露线宽，见文件头 */
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(&layer, &dsc);

    lv_canvas_finish_layer(o, &layer);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_draw_rect(int64_t canvas, int32_t x, int32_t y, int32_t w,
                                int32_t h, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (w <= 0 || h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, w, __func__,
                            "矩形宽高必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = NULL;
    lv_layer_t layer;
    int32_t rc = canvas_begin_draw(canvas, __func__, &o, &layer);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = canvas_color_rgb(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    /* LVGL 的矩形用"闭区间"表示区域，右/下是坐标本身而非宽高 */
    lv_area_t area;
    area.x1 = x;
    area.y1 = y;
    area.x2 = x + w - 1;
    area.y2 = y + h - 1;
    lv_draw_rect(&layer, &dsc, &area);

    lv_canvas_finish_layer(o, &layer);
    return LVGLCJ_OK;
}

int32_t lvglcj_canvas_draw_arc(int64_t canvas, int32_t cx, int32_t cy, int32_t r,
                               int32_t start_angle, int32_t end_angle, uint32_t color)
{
    LVGLCJ_HANDLE_GUARD(canvas, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 半径为 0 画不出东西；dsc.radius 是 uint16_t，超出会静默截断 —— 显式拦住 */
    if (r <= 0 || r > 65535) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, canvas, r, __func__,
                            "半径必须为正且不超过 65535（dsc.radius 是 uint16_t，超出会截断）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_obj_t *o = NULL;
    lv_layer_t layer;
    int32_t rc = canvas_begin_draw(canvas, __func__, &o, &layer);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = canvas_color_rgb(color);
    dsc.width = 2; /* ABI 未暴露线宽，见文件头 */
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = cx;
    dsc.center.y = cy;
    dsc.radius = (uint16_t)r;
    dsc.start_angle = start_angle;
    dsc.end_angle = end_angle;
    lv_draw_arc(&layer, &dsc);

    lv_canvas_finish_layer(o, &layer);
    return LVGLCJ_OK;
}
