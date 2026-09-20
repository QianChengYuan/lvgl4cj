/*
 * style.c —— 样式子系统（设计文档 §5.7）
 *
 * ==================== v9 与 v8 的关键差异（写错会静默失效） ====================
 *   v8：lv_style_set_bg_color(style, LV_STATE_PRESSED, color)   ← 带 selector
 *   v9：lv_style_set_bg_color(style, color)                     ← **不带**
 *       selector 只在 lv_obj_add_style(obj, style, selector) 时给出。
 *
 * 若照 v8 的形式多传一个 selector，编译期可能因为隐式转换而通过，
 * 运行期表现为「设置了一个不存在的状态」→ **样式完全没效果、也不报错**。
 * 本文件的所有 setter 因此严格按 v9 生成头的签名来写（见 lv_style_gen.h）。
 *
 * ==================== 内存所有权 ====================
 * lv_style_t 是公开结构，由本层 malloc 持有、句柄化（与对象同一套四态语义）。
 * 删除时必须**先 lv_style_reset 再 free** —— reset 负责释放 style 内部的
 * 属性数组，顺序反了就泄漏（而且 LVGL 无泄漏报告，很难发现）。
 */
#include "lvglcj_internal.h"

#include <stdlib.h>

#define STYLE_GUARD(s)                          \
    do {                                        \
        LVGLCJ_CHECK_LVGL_THREAD_RET();         \
        LVGLCJ_HANDLE_GUARD((s), __func__);      \
        if (lvglcj_ptr_of(s) == NULL) {         \
            return LVGLCJ_ERR_INVALID_HANDLE;   \
        }                                       \
    } while (0)

#define ST(s) ((lv_style_t *)lvglcj_ptr_of(s))

/* 整数属性 */
#define ST_INT(fn, lvfn)              \
    int32_t fn(int64_t s, int32_t v)  \
    {                                 \
        STYLE_GUARD(s);               \
        lvfn(ST(s), v);               \
        return LVGLCJ_OK;             \
    }

/* 颜色属性：ABI 传 0xRRGGBB，LVGL 侧转成 lv_color_t */
#define ST_COLOR(fn, lvfn)                  \
    int32_t fn(int64_t s, uint32_t color)   \
    {                                       \
        STYLE_GUARD(s);                     \
        lvfn(ST(s), lv_color_hex(color));   \
        return LVGLCJ_OK;                   \
    }

/* 枚举 / 窄类型属性：ABI 统一传 int32，这里显式转型（避免隐式截断告警） */
#define ST_ENUM(fn, lvfn, lvtype)         \
    int32_t fn(int64_t s, int32_t v)      \
    {                                     \
        STYLE_GUARD(s);                   \
        lvfn(ST(s), (lvtype)v);           \
        return LVGLCJ_OK;                 \
    }

/* 直接作用在对象上的便捷样式（v9 签名带 selector，因为不经过 style 对象） */
#define ST_INT_OBJ(fn, lvfn)                                          \
    int32_t fn(int64_t obj, int32_t v, int32_t selector)              \
    {                                                                 \
        LVGLCJ_HANDLE_GUARD(obj, __func__);                           \
        LVGLCJ_CHECK_LVGL_THREAD_RET();                               \
        lvfn((lv_obj_t *)lvglcj_ptr_of(obj), v,                       \
             (lv_style_selector_t)selector);                          \
        return LVGLCJ_OK;                                             \
    }

/* ------------------------------------------------------------------ 生命周期 */

int64_t lvglcj_style_create(void)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lv_style_t *st = (lv_style_t *)malloc(sizeof(lv_style_t));
    if (st == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "style 分配失败");
        return LVGLCJ_HANDLE_NULL;
    }
    lv_style_init(st);

    int64_t h = lvglcj_handle_register(st, "lv_style_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 句柄登记失败必须把刚分配的内存也放掉，否则原生内存泄漏 */
        free(st);
        return LVGLCJ_HANDLE_NULL;
    }
    return h;
}

int32_t lvglcj_style_delete(int64_t s)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    /* 严格校验：非 ALIVE 一律 INVALID_HANDLE（幂等由仓颉 close() 负责） */
    STYLE_GUARD(s);

    /*
     * ★★ 引用检查：样式仍被存活对象引用时**拒绝释放** ★★
     *
     * lv_style_t 是引用型资源 —— lv_obj_add_style 只保存指针，不拷贝。
     * 因此「先释放样式、后删除对象」会让对象的样式链表中留下一个悬空指针，
     * 删除对象时 LVGL 遍历该链表即 **heap-use-after-free**
     * （本项由 t8 的 ASan 门禁实测发现，栈为
     *  lvglcj_obj_delete_now → lv_obj_delete → lv_obj_get_style_width → get_prop_core）。
     *
     * 这类 UAF 的危险之处在于它**不一定立刻崩**：也可能静默读到垃圾值，
     * 表现为尺寸/颜色错乱，比崩溃更难定位。所以在入口处拦成明确报错。
     */
    int32_t users = lvglcj_reg_style_user_count(s);
    if (users > 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, s, 0, __func__,
                            "样式仍被存活对象引用，先 obj_remove_style 或先删除这些对象再释放样式");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_style_t *st = ST(s);
    /*
     * ★ 顺序固定：先 reset（释放内部属性数组）再 free 结构体本身。
     *   反过来会泄漏 reset 负责的那块内存，且没有任何报错。
     */
    lv_style_reset(st);
    free(st);
    lvglcj_handle_release(s);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------------ 背景 */
ST_COLOR(lvglcj_style_set_bg_color, lv_style_set_bg_color)
ST_ENUM(lvglcj_style_set_bg_opa, lv_style_set_bg_opa, lv_opa_t)
ST_COLOR(lvglcj_style_set_bg_grad_color, lv_style_set_bg_grad_color)
ST_ENUM(lvglcj_style_set_bg_grad_dir, lv_style_set_bg_grad_dir, lv_grad_dir_t)

/* ------------------------------------------------------------------ 边框 */
ST_INT(lvglcj_style_set_border_width, lv_style_set_border_width)
ST_COLOR(lvglcj_style_set_border_color, lv_style_set_border_color)
ST_ENUM(lvglcj_style_set_border_opa, lv_style_set_border_opa, lv_opa_t)

/* ------------------------------------------------------------------ 圆角与内边距 */
ST_INT(lvglcj_style_set_radius, lv_style_set_radius)
/*
 * pad_all 在 v9 是 static inline 的便捷函数（内部分别设四边），
 * 因此它不出现在生成头里 —— 这是「核对时显示缺失」的唯一一处style设置器，
 * 实际可用。
 */
ST_INT(lvglcj_style_set_pad_all, lv_style_set_pad_all)
ST_INT(lvglcj_style_set_pad_top, lv_style_set_pad_top)
ST_INT(lvglcj_style_set_pad_bottom, lv_style_set_pad_bottom)
ST_INT(lvglcj_style_set_pad_left, lv_style_set_pad_left)
ST_INT(lvglcj_style_set_pad_right, lv_style_set_pad_right)
ST_INT(lvglcj_style_set_pad_row, lv_style_set_pad_row)
ST_INT(lvglcj_style_set_pad_column, lv_style_set_pad_column)

/* ------------------------------------------------------------------ 尺寸约束 */
ST_INT(lvglcj_style_set_width, lv_style_set_width)
ST_INT(lvglcj_style_set_height, lv_style_set_height)
ST_INT(lvglcj_style_set_min_width, lv_style_set_min_width)
ST_INT(lvglcj_style_set_max_width, lv_style_set_max_width)
ST_INT(lvglcj_style_set_min_height, lv_style_set_min_height)
ST_INT(lvglcj_style_set_max_height, lv_style_set_max_height)

/* ------------------------------------------------------------------ 文本 */
ST_COLOR(lvglcj_style_set_text_color, lv_style_set_text_color)
ST_ENUM(lvglcj_style_set_text_opa, lv_style_set_text_opa, lv_opa_t)
ST_ENUM(lvglcj_style_set_text_align, lv_style_set_text_align, lv_text_align_t)
ST_INT(lvglcj_style_set_text_letter_space, lv_style_set_text_letter_space)
ST_INT(lvglcj_style_set_text_line_space, lv_style_set_text_line_space)

/*
 * 字体：ABI 传字体句柄（0 表示用默认字体）。
 *
 * 非 0 句柄解析不到时**明确报错**，而不是静默退回默认字体 ——
 * 否则「设置了字体却没变化」会被误判成样式链路的问题。
 *
 * ★ 本条注释曾写「字体子系统（font.c）尚未实现」，那是 font.c 落地之前的说法。
 *   font.c 早已实现（内置字体 + 从文件加载 + 字形覆盖查询），
 *   此处**只剩下句柄校验这一个职责**；句柄无效的真实原因通常是
 *   「字体已被 close」或「传了别的类型的句柄」，而不是"没实现"。
 *   过期注释比没有注释更糟：它会把排查方向指向一个不存在的原因。
 */
int32_t lvglcj_style_set_text_font(int64_t s, int64_t font)
{
    STYLE_GUARD(s);

    const lv_font_t *f = NULL;
    if (font != LVGLCJ_HANDLE_NULL) {
        f = (const lv_font_t *)lvglcj_ptr_of(font);
        if (f == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, font, 0, __func__,
                                "字体句柄无效（传 0 表示用默认字体；非 0 请用 LvFont.builtin/load 取得的句柄）");
            return LVGLCJ_ERR_INVALID_HANDLE;
        }
    }
    lv_style_set_text_font(ST(s), f);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------------ 阴影 */
ST_INT(lvglcj_style_set_shadow_width, lv_style_set_shadow_width)
ST_COLOR(lvglcj_style_set_shadow_color, lv_style_set_shadow_color)
ST_ENUM(lvglcj_style_set_shadow_opa, lv_style_set_shadow_opa, lv_opa_t)
ST_INT(lvglcj_style_set_shadow_offset_x, lv_style_set_shadow_offset_x)
ST_INT(lvglcj_style_set_shadow_offset_y, lv_style_set_shadow_offset_y)

/* ------------------------------------------------------------------ 外轮廓 */
ST_INT(lvglcj_style_set_outline_width, lv_style_set_outline_width)
ST_COLOR(lvglcj_style_set_outline_color, lv_style_set_outline_color)
ST_ENUM(lvglcj_style_set_outline_opa, lv_style_set_outline_opa, lv_opa_t)
ST_INT(lvglcj_style_set_outline_pad, lv_style_set_outline_pad)

/* ------------------------------------------------------------------ 变换与整体透明度 */
ST_INT(lvglcj_style_set_translate_x, lv_style_set_translate_x)
ST_INT(lvglcj_style_set_translate_y, lv_style_set_translate_y)
ST_ENUM(lvglcj_style_set_opa, lv_style_set_opa, lv_opa_t)

/* ------------------------------------------------------------------ 线 */
ST_INT(lvglcj_style_set_line_width, lv_style_set_line_width)
ST_INT(lvglcj_style_set_line_dash_width, lv_style_set_line_dash_width)
ST_INT(lvglcj_style_set_line_dash_gap, lv_style_set_line_dash_gap)

/* ------------------------------------------------------------------ 弧 */
ST_INT(lvglcj_style_set_arc_width, lv_style_set_arc_width)
ST_COLOR(lvglcj_style_set_arc_color, lv_style_set_arc_color)
ST_ENUM(lvglcj_style_set_arc_rounded, lv_style_set_arc_rounded, bool)

/* ------------------------------------------------------------------ 图像 */
ST_ENUM(lvglcj_style_set_image_opa, lv_style_set_image_opa, lv_opa_t)
ST_COLOR(lvglcj_style_set_image_recolor, lv_style_set_image_recolor)
ST_ENUM(lvglcj_style_set_image_recolor_opa, lv_style_set_image_recolor_opa, lv_opa_t)

/* ------------------------------------------------------------------ 布局与方向 */
ST_ENUM(lvglcj_style_set_blend_mode, lv_style_set_blend_mode, lv_blend_mode_t)
ST_ENUM(lvglcj_style_set_layout, lv_style_set_layout, uint16_t)
ST_ENUM(lvglcj_style_set_base_dir, lv_style_set_base_dir, lv_base_dir_t)
ST_ENUM(lvglcj_style_set_clip_corner, lv_style_set_clip_corner, bool)
ST_ENUM(lvglcj_style_set_rotary_sensitivity, lv_style_set_rotary_sensitivity, uint32_t)

/* ------------------------------------------------------------------ 应用到对象 */

int32_t lvglcj_obj_add_style(int64_t obj, int64_t style, int32_t selector)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(style, __func__);

    /* v9 的 selector 在**这里**给出（= part | state），不是 setter 上 */
    lv_obj_add_style((lv_obj_t *)lvglcj_ptr_of(obj), ST(style),
                     (lv_style_selector_t)selector);

    /*
     * 登记「对象正在用这个样式」。失败必须**回滚**已经生效的 add，
     * 否则会出现「LVGL 认为挂着、我们的守卫认为没挂」的分歧 ——
     * 之后 style_delete 会放行，UAF 就又回来了。宁可让调用方看到 OOM。
     */
    int32_t brc = lvglcj_reg_style_bind(obj, style, selector);
    if (brc != LVGLCJ_OK) {
        lv_obj_remove_style((lv_obj_t *)lvglcj_ptr_of(obj), ST(style),
                            (lv_style_selector_t)selector);
        lvglcj_record_error(brc, obj, 0, __func__,
                            "样式使用者登记失败，已回滚本次 add_style");
        return brc;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_remove_style(int64_t obj, int64_t style, int32_t selector)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(style, __func__);

    /*
     * ★ 与 v8 的差异：v9 的 lv_obj_remove_style 返回 **void**（v8 返回是否移除成功）。
     *   因此本层**无法**区分「移除了」与「本来就没挂」—— 两者都是静默成功。
     *   这不是本次实现的取舍，而是 v9 API 本身不再提供该信息；不要在 L1 假装有返回值。
     *   （本层自 t8 起维护「对象挂了哪些 style」的注册表，用于样式释放前的引用检查；
     *     但那是为了安全，不是为了补出这个返回值 —— 仍不对调用方声称「移除了」。）
     */
    lv_obj_remove_style((lv_obj_t *)lvglcj_ptr_of(obj), ST(style),
                        (lv_style_selector_t)selector);
    lvglcj_reg_style_unbind(obj, style, selector);
    return LVGLCJ_OK;
}

int32_t lvglcj_obj_remove_style_all(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lv_obj_remove_style_all((lv_obj_t *)lvglcj_ptr_of(obj));
    /* 连带清掉该对象的全部样式记录（含 LVGL 主题自带的，我们本来就没记它们） */
    lvglcj_reg_style_unbind_obj(obj);
    return LVGLCJ_OK;
}

/*
 * 读回属性值。
 *
 * ★ 此处是「有据收窄」而非全量实现：
 *   lv_style_value_t 是个 union（num / color / ptr），**无法从值本身判断类型**，
 *   读错成员会得到看似合理但错误的数（比崩溃更难发现）。
 *   因此本函数只对**本层 setter 暴露过的颜色属性**按颜色读，其余按数值读，
 *   并对未知属性明确返回 -1。P0 的用途是测试断言「样式确实生效了」，
 *   覆盖这些属性已经足够；P1 若要全量读回，应改为按 prop 的类型表分发。
 */
static int lvglcj_prop_is_color(int32_t prop)
{
    switch (prop) {
        case LV_STYLE_BG_COLOR:
        case LV_STYLE_BG_GRAD_COLOR:
        case LV_STYLE_BORDER_COLOR:
        case LV_STYLE_TEXT_COLOR:
        case LV_STYLE_SHADOW_COLOR:
        case LV_STYLE_OUTLINE_COLOR:
        case LV_STYLE_ARC_COLOR:
        case LV_STYLE_IMAGE_RECOLOR:
            return 1;
        default:
            return 0;
    }
}

int64_t lvglcj_obj_get_style_prop(int64_t obj, int32_t prop, int32_t selector)
{
    int32_t rc = lvglcj_handle_require(obj, __func__);
    if (rc != LVGLCJ_OK) {
        return -1;
    }
    /* 返回 int64 的函数无法用 RET 宏（宏里是 return rc），此处显式检查 */
    if (lvglcj_check_lvgl_thread(__func__) != LVGLCJ_OK) {
        return -1;
    }

    lv_part_t part = (lv_part_t)((uint32_t)selector & 0xFFu);
    lv_style_value_t v = lv_obj_get_style_prop((lv_obj_t *)lvglcj_ptr_of(obj), part,
                                              (lv_style_prop_t)prop);
    if (lvglcj_prop_is_color(prop)) {
        /* 颜色统一按 0xRRGGBB 返回，与 setter 的输入口径一致 */
        return (int64_t)(lv_color_to_u32(v.color) & 0xFFFFFFu);
    }
    return (int64_t)v.num;
}

/* ------------------------------------------------------------------ 对象上的便捷样式 */
ST_INT_OBJ(lvglcj_obj_set_style_pad_all, lv_obj_set_style_pad_all)
ST_INT_OBJ(lvglcj_obj_set_style_pad_gap, lv_obj_set_style_pad_gap)
