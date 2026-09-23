/*
 * test_style_widgets.c —— C 侧单测：样式（§5.7）与 P0 控件（label/button）
 *
 * 覆盖：
 *   · ★ 样式**真的生效**：设进去的值能按 selector 读回来
 *     （v9 的 setter 不带 selector，selector 只在 add_style 给出；
 *       一旦按 v8 写就会「设置到不存在的状态」→ 静默失效，本用例正是防它）
 *   · 样式生命周期：删除后句柄回收、重复删除返回 INVALID_HANDLE
 *   · 提交嵌套：add_style 多次 / remove_style / remove_style_all 不崩
 *   · label：建、设文本、设 long_mode（非法值被拒）
 *   · button：建、挂样式
 *   · ★ 控件句柄同样级联失效：删父容器后 label/button 句柄为 INVALIDATED
 *     （这是 P0 断言 4 在**控件**上的直接证据 —— 控件漏挂钩子就会在此暴露）
 *
 * 用 CTest 注册：bash scripts/build_native.sh --tests
 */
#include "lvglcj_internal.h"
#include "lvglcj_backend_null.h"

#include <stdio.h>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        g_total++;                                    \
        if (cond) {                                   \
            printf("  [ok]   %s\n", (msg));           \
        } else {                                      \
            printf("  [FAIL] %s\n", (msg));           \
            g_fail++;                                 \
        }                                             \
    } while (0)

/* 便于阅读的常量：主部件 + 默认状态 */
#define SEL_MAIN ((int32_t)LV_PART_MAIN | (int32_t)LV_STATE_DEFAULT)

int main(void)
{
    printf("=== test_style_widgets：样式与 P0 控件 ===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    /* G1：必须先有 display 才能建对象 */
    if (lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
        printf("  [FAIL] headless display 初始化\n");
        return 1;
    }

    int64_t scr = lvglcj_screen_active();
    CHECK(scr != LVGLCJ_HANDLE_NULL, "活动屏幕可用");

    /* ================================================== 1. 样式创建与设置 */
    printf("\n-- 1. 样式创建与属性设置 --\n");
    int64_t st = lvglcj_style_create();
    CHECK(st != LVGLCJ_HANDLE_NULL, "style_create 成功");

    CHECK(lvglcj_style_set_bg_color(st, 0x101820) == LVGLCJ_OK, "设 bg_color");
    CHECK(lvglcj_style_set_bg_opa(st, 255) == LVGLCJ_OK, "设 bg_opa = 255(COVER)");
    CHECK(lvglcj_style_set_radius(st, 12) == LVGLCJ_OK, "设 radius");
    CHECK(lvglcj_style_set_border_width(st, 0) == LVGLCJ_OK, "设 border_width = 0");
    CHECK(lvglcj_style_set_text_color(st, 0x00A0FF) == LVGLCJ_OK, "设 text_color");
    CHECK(lvglcj_style_set_pad_all(st, 8) == LVGLCJ_OK, "设 pad_all");
    CHECK(lvglcj_style_set_width(st, 200) == LVGLCJ_OK, "设 width");
    CHECK(lvglcj_style_set_height(st, 60) == LVGLCJ_OK, "设 height");

    /* ================================================== 2. ★ 样式真的生效 */
    printf("\n-- 2. ★ 样式生效验证（v9 selector 语义）--\n");
    int64_t btn = lvglcj_button_create(scr);
    CHECK(btn != LVGLCJ_HANDLE_NULL, "button 创建成功");

    CHECK(lvglcj_obj_add_style(btn, st, SEL_MAIN) == LVGLCJ_OK, "把样式挂到 button");
    /*
     * ★ 关键断言：读回的应当是我们设进去的值。
     *   若 setter 误按 v8 传了 selector，这里读出来就是默认值（即样式没生效）。
     */
    int64_t got_bg = lvglcj_obj_get_style_prop(btn, LV_STYLE_BG_COLOR, SEL_MAIN);
    CHECK(got_bg == 0x101820,
          "★ 读回 bg_color 与设置值一致（证明样式确实生效，未被 selector 语义坑到）");
    CHECK(lvglcj_obj_get_style_prop(btn, LV_STYLE_RADIUS, SEL_MAIN) == 12,
          "★ 读回 radius 一致");
    CHECK(lvglcj_obj_get_style_prop(btn, LV_STYLE_BG_OPA, SEL_MAIN) == 255,
          "★ 读回 bg_opa 一致");
    CHECK(lvglcj_obj_get_style_prop(btn, LV_STYLE_WIDTH, SEL_MAIN) == 200,
          "★ 读回 width 一致（尺寸约束类属性同样生效）");

    /* ================================================== 3. 样式的重复挂载与移除 */
    printf("\n-- 3. 重复挂载与移除 --\n");
    CHECK(lvglcj_obj_add_style(btn, st, SEL_MAIN) == LVGLCJ_OK,
          "同一对象重复挂同一 style 不报错");
    CHECK(lvglcj_obj_remove_style(btn, st, SEL_MAIN) == LVGLCJ_OK, "remove_style 可用");
    CHECK(lvglcj_obj_remove_style_all(btn) == LVGLCJ_OK, "remove_style_all 可用");
    CHECK(lvglcj_obj_add_style(btn, st, SEL_MAIN) == LVGLCJ_OK, "移除后可再次挂上");

    /* ================================================== 4. label 控件 */
    printf("\n-- 4. label --\n");
    int64_t lbl = lvglcj_label_create(btn);
    CHECK(lbl != LVGLCJ_HANDLE_NULL, "label 创建成功（父为 button）");
    CHECK(lvglcj_label_set_text(lbl, "点我") == LVGLCJ_OK, "设置文本");
    CHECK(lvglcj_label_set_text(lbl, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
          "NULL 文本被拒");
    CHECK(lvglcj_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP) == LVGLCJ_OK,
          "设置 long_mode = CLIP");
    CHECK(lvglcj_label_set_long_mode(lbl, 99) == LVGLCJ_ERR_INVALID_ARGUMENT,
          "非法 long_mode 被拒");
    /* 文本变更可观测：宽度应能反映文本（非 0 宽度说明 label 真的布局了） */
    CHECK(lvglcj_obj_get_width(lbl) >= 0, "label 宽度可读");

    /* ================================================== 5. 计数与树关系 */
    printf("\n-- 5. 对象树 --\n");
    CHECK(lvglcj_obj_get_child_count(scr) >= 1, "屏幕下有子对象");
    CHECK(lvglcj_obj_get_child_count(btn) == 1, "button 下有 1 个子对象（label）");

    /* ================================================== 6. ★ 控件句柄级联失效 */
    printf("\n-- 6. ★ 删父容器 → 控件句柄级联失效（断言 4）--\n");
    {
        /*
         * 造一个容器，里面放 label 与 button，然后删容器。
         * 控件漏挂 DELETE 钩子时，它们的句柄会停留在 ALIVE —— 本用例会立刻暴露。
         */
        int64_t box = lvglcj_obj_create(scr);
        int64_t b2 = lvglcj_button_create(box);
        int64_t l2 = lvglcj_label_create(b2);
        CHECK(box != 0 && b2 != 0 && l2 != 0, "创建容器 + 按钮 + 标签");

        CHECK(lvglcj_obj_delete(box) == LVGLCJ_OK, "删除容器（父）");
        CHECK(lvglcj_handle_alive(b2) == 0, "★ 按钮句柄已不再存活");
        CHECK(lvglcj_handle_alive(l2) == 0, "★ 标签句柄已不再存活");
        CHECK(lvglcj_handle_state(b2) == LVGLCJ_HSTATE_INVALIDATED,
              "★ 按钮句柄为 INVALIDATED（表项保留以便诊断）");
        CHECK(lvglcj_handle_state(l2) == LVGLCJ_HSTATE_INVALIDATED,
              "★ 标签句柄为 INVALIDATED");
        /* 失效后对控件调用业务 API 必须被明确拒绝，而不是写到已释放的内存 */
        CHECK(lvglcj_label_set_text(l2, "x") == LVGLCJ_ERR_INVALID_HANDLE,
              "★ 对已失效的 label 设文本返回 INVALID_HANDLE");
        CHECK(lvglcj_style_set_radius(st, 1) == LVGLCJ_OK,
              "样式对象不受对象树删除影响（它不属于任何对象）");
    }

    /* ================================================== 7. ★ 样式释放的引用守卫 */
    printf("\n-- 7. ★ 样式释放：仍被引用时必须拒绝 --\n");
    /*
     * ★ 这一组断言来自 t8 的 ASan 门禁实测发现的缺陷。
     *
     *   样式是**引用型**资源：lv_obj_add_style 只保存指针，不拷贝。
     *   于是「先释放样式、后删除对象」会让对象的样式链表里留下悬空指针，
     *   删除对象时 LVGL 遍历该链表即 heap-use-after-free
     *   （ASan 栈：lvglcj_obj_delete_now → lv_obj_delete → lv_obj_get_style_width → get_prop_core）。
     *
     *   更危险的是它在普通构建下**不一定崩**：也可能静默读到垃圾值，
     *   表现为尺寸/颜色错乱 —— 比崩溃更难定位。所以入口处拦成明确报错。
     */
    CHECK(lvglcj_style_delete(st) == LVGLCJ_ERR_INVALID_ARGUMENT,
          "★ 样式仍被存活对象引用时被拒（把静默 UAF 变成明确报错）");
    CHECK(lvglcj_style_set_radius(st, 10) == LVGLCJ_OK,
          "★ 被拒后样式仍然可用（拒绝不是「半途而废」的破坏）");

    CHECK(lvglcj_obj_remove_style(btn, st, SEL_MAIN) == LVGLCJ_OK,
          "先 remove_style 解除引用");
    CHECK(lvglcj_style_delete(st) == LVGLCJ_OK,
          "★ 引用解除后 style_delete 成功");
    CHECK(lvglcj_handle_state(st) == LVGLCJ_HSTATE_UNINIT,
          "删除后表项已回收");
    CHECK(lvglcj_style_delete(st) == LVGLCJ_ERR_INVALID_HANDLE,
          "★ 重复删除返回 INVALID_HANDLE（幂等由仓颉 close() 负责）");
    CHECK(lvglcj_style_set_radius(st, 1) == LVGLCJ_ERR_INVALID_HANDLE,
          "对已删除样式操作被拒");

    /*
     * ★ 守卫的另一半：删除**对象**也必须解除引用。
     *   若 DELETE 钩子漏了这一步，样式会永远显示「有人在用」而无法释放 ——
     *   守卫就从「防 UAF」退化成「永久死锁」。顺便这也验证了删除路径
     *   与样式表配合正确（正是原 UAF 的触发路径）。
     */
    {
        int64_t st2 = lvglcj_style_create();
        int64_t tmp = lvglcj_obj_create(scr);
        CHECK(st2 != 0 && tmp != 0, "准备第二个样式与被引用的对象");
        CHECK(lvglcj_obj_add_style(tmp, st2, SEL_MAIN) == LVGLCJ_OK, "挂到对象上");
        CHECK(lvglcj_style_delete(st2) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 有引用时同样被拒");
        CHECK(lvglcj_obj_delete(tmp) == LVGLCJ_OK, "删除该对象（引用随之解除）");
        CHECK(lvglcj_style_delete(st2) == LVGLCJ_OK,
              "★ 对象删除后样式即可释放（DELETE 钩子已解除引用）");
    }

    /* ================================================== 7b. P1 批次 1 控件
     *
     * 覆盖三类**容易漏掉**的行为，而不是只做"创建成功"的冒烟：
     *   1. 状态往返（设了能读回来）—— 这一条钉住 set_value 关动画的决定：
     *      若哪天改回带动画，这里的读值会读到中间态而立刻失败。
     *   2. 参数校验（min >= max 必须被拦下）—— 放行的话它会变成
     *      "设了值但显示不对"这种难以归因的现象，而不是一个明确报错。
     *   3. ★ 父对象删除后句柄必须**级联失效** —— 这是控件实现最容易漏的一步
     *      （创建时忘挂 DELETE 钩子）。漏了则 isAlive 仍为 true 而底层对象已消失，
     *      即 §3.3 要防的悬空句柄。三个新控件逐一验证。
     */
    printf("\n-- 7b. P1 批次 1 控件（switch / checkbox / bar）--\n");
    {
        int64_t holder = lvglcj_obj_create(scr);
        CHECK(holder != 0, "创建承载父对象");

        int64_t sw = lvglcj_switch_create(holder);
        CHECK(sw != 0, "创建 switch");
        CHECK(lvglcj_handle_state(sw) == LVGLCJ_HSTATE_ALIVE, "switch 句柄登记为 ALIVE");
        CHECK(lvglcj_switch_is_checked(sw) == 0, "初始未勾选（0 是答案，不是错误）");
        CHECK(lvglcj_switch_set_checked(sw, 1) == LVGLCJ_OK, "置为勾选");
        CHECK(lvglcj_switch_is_checked(sw) == 1, "读回已勾选");
        CHECK(lvglcj_switch_set_checked(sw, 0) == LVGLCJ_OK, "置回未勾选");
        CHECK(lvglcj_switch_is_checked(sw) == 0, "读回未勾选");

        int64_t cb = lvglcj_checkbox_create(holder);
        CHECK(cb != 0, "创建 checkbox");
        CHECK(lvglcj_checkbox_set_text(cb, "选项 A") == LVGLCJ_OK, "设置文本");
        CHECK(lvglcj_checkbox_set_text(cb, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "文本指针为 NULL：明确拒绝");
        CHECK(lvglcj_checkbox_set_checked(cb, 1) == LVGLCJ_OK, "勾选");
        CHECK(lvglcj_checkbox_is_checked(cb) == 1, "读回已勾选");

        int64_t bar = lvglcj_bar_create(holder);
        CHECK(bar != 0, "创建 bar");
        CHECK(lvglcj_bar_set_range(bar, 0, 100) == LVGLCJ_OK, "设置范围 0..100");
        CHECK(lvglcj_bar_set_value(bar, 42) == LVGLCJ_OK, "设值 42");
        int32_t got = -1;
        CHECK(lvglcj_bar_get_value(bar, &got) == LVGLCJ_OK, "读值");
        CHECK(got == 42, "★ 设完立刻读回即为新值（关动画的直接后果）");
        CHECK(lvglcj_bar_get_value(bar, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "出参为 NULL：明确拒绝");
        CHECK(lvglcj_bar_set_range(bar, 100, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ min > max 被拒绝（否则会成为显示异常而非报错）");
        CHECK(lvglcj_bar_set_range(bar, 5, 5) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "min == max 同样被拒绝");
        /* 负值范围是合法的（bar 的值域本身可为负），不该被误拦 */
        CHECK(lvglcj_bar_set_range(bar, -50, 50) == LVGLCJ_OK, "负值范围合法");
        CHECK(lvglcj_bar_set_value(bar, -20) == LVGLCJ_OK, "设负值");
        CHECK(lvglcj_bar_get_value(bar, &got) == LVGLCJ_OK && got == -20,
              "★ 负值能正确读回（这正是 get_value 必须用出参的原因）");

        /* 无效句柄必须报错而不是崩溃 */
        CHECK(lvglcj_switch_set_checked(0, 1) != LVGLCJ_OK, "句柄 0：报错而非崩溃");
        CHECK(lvglcj_bar_get_value(123456789, &got) != LVGLCJ_OK,
              "不存在的句柄：报错而非崩溃");

        /* ★ 级联失效：删掉承载父对象后，三个控件的句柄都必须变为 INVALIDATED */
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除承载父对象");
        CHECK(lvglcj_handle_state(sw) == LVGLCJ_HSTATE_INVALIDATED,
              "★ switch 句柄随父级联失效");
        CHECK(lvglcj_handle_state(cb) == LVGLCJ_HSTATE_INVALIDATED,
              "★ checkbox 句柄随父级联失效");
        CHECK(lvglcj_handle_state(bar) == LVGLCJ_HSTATE_INVALIDATED,
              "★ bar 句柄随父级联失效");
        CHECK(lvglcj_switch_set_checked(sw, 1) != LVGLCJ_OK,
              "对已失效的 switch 操作：报错而非 UAF");
    }

    /* ================================================== 7c. Canvas（§3.11.2）
     *
     * 本段最要紧的是**缓冲所有权**：像素缓冲由 C 侧 malloc、随对象释放，
     * 所以「重复 set_buffer」「删除 canvas」两条路径都必须既不泄漏也不重复释放 ——
     * 而这正是 ASan 能精确抓到、靠人眼很难发现的类别。
     * 因此本段在 ASan 下的价值高于在普通构建下的价值。
     */
    printf("\n-- 7c. Canvas（自定义绘制的唯一出口）--\n");
    {
        int64_t cv = lvglcj_canvas_create(scr);
        CHECK(cv != 0, "创建 canvas");

        /* ★ 未设缓冲就绘制：必须明确报错，而不是崩在 LVGL 内部 */
        CHECK(lvglcj_canvas_draw_point(cv, 0, 0, 0xFF0000u) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 未 set_buffer 就绘制：明确报错而非崩溃");
        CHECK(lvglcj_canvas_fill_bg(cv, 0x112233u, 255) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "未 set_buffer 就 fill_bg：同样明确报错");

        /* 尺寸校验（只拦不可能的情形，不设经验上限） */
        CHECK(lvglcj_canvas_set_buffer(cv, 0, 64) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "宽度为 0 被拒绝");
        CHECK(lvglcj_canvas_set_buffer(cv, -1, 64) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "宽度为负被拒绝");

        CHECK(lvglcj_canvas_set_buffer(cv, 64, 64) == LVGLCJ_OK, "设置 64×64 缓冲");

        /* 绘制操作全部放行 */
        CHECK(lvglcj_canvas_fill_bg(cv, 0x1A1D2Eu, 255) == LVGLCJ_OK, "fill_bg 不透明");
        CHECK(lvglcj_canvas_fill_bg(cv, 0x1A1D2Eu, 128) == LVGLCJ_OK, "fill_bg 半透明");
        CHECK(lvglcj_canvas_draw_point(cv, 1, 1, 0xFF0000u) == LVGLCJ_OK, "draw_point");
        CHECK(lvglcj_canvas_draw_line(cv, 0, 0, 63, 63, 0xFFFFFFu) == LVGLCJ_OK, "draw_line");
        CHECK(lvglcj_canvas_draw_rect(cv, 4, 4, 8, 8, 0x00FF00u) == LVGLCJ_OK, "draw_rect");
        CHECK(lvglcj_canvas_draw_arc(cv, 32, 32, 20, 0, 180, 0x00D9B5u) == LVGLCJ_OK, "draw_arc");
        CHECK(lvglcj_canvas_set_palette(cv, 1, 0x80FF0000u) == LVGLCJ_OK, "set_palette");

        /* 参数校验 */
        CHECK(lvglcj_canvas_draw_rect(cv, 0, 0, 0, 8, 0xFFFFFFu) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "矩形宽为 0 被拒绝");
        CHECK(lvglcj_canvas_draw_arc(cv, 0, 0, 0, 0, 90, 0xFFFFFFu) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "半径为 0 被拒绝");
        CHECK(lvglcj_canvas_draw_arc(cv, 0, 0, 70000, 0, 90, 0xFFFFFFu) ==
                  LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 半径超过 uint16 被拒绝（放行会被 LVGL 静默截断）");
        CHECK(lvglcj_canvas_set_palette(cv, 256, 0u) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "调色板索引越界被拒绝");
        CHECK(lvglcj_canvas_set_palette(cv, -1, 0u) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "调色板索引为负被拒绝");
        CHECK(lvglcj_canvas_fill_bg(cv, 0u, 999) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "opa 越界被拒绝");

        /* ★ 重复设置缓冲：内部必须先释放旧的那块，否则泄漏（ASan 会报） */
        CHECK(lvglcj_canvas_set_buffer(cv, 32, 32) == LVGLCJ_OK, "★ 改尺寸：重复设缓冲成功");
        CHECK(lvglcj_canvas_draw_point(cv, 5, 5, 0xFFFFFFu) == LVGLCJ_OK,
              "改尺寸后仍可正常绘制");

        /* 删除 canvas：DELETE 钩子释放缓冲；ASan 会在这里报出泄漏或重复释放 */
        CHECK(lvglcj_obj_delete(cv) == LVGLCJ_OK, "★ 删除 canvas（缓冲随之释放）");
        /*
         * ★ 这里断言的是「不再是 ALIVE」，而**不是**某个具体状态。
         *
         *   原因：7b 段验证的是「父对象被删 → 子句柄 INVALIDATED」，那是**级联**路径；
         *   本段删的是 canvas **自身**，是另一条路径，其终态不一定是 INVALIDATED
         *   （可能是 RELEASED/UNINIT —— 表项被回收）。
         *   首版照抄了 INVALIDATED，于是在 ASan 下报出一条看起来像产品缺陷的失败；
         *   实际上它只是把我的**未经核实的预期**当成了断言。
         *
         *   真正必须成立的不变量只有一个：**已删除对象的句柄不得再报告 ALIVE** ——
         *   否则调用方会以为它还能用。断言这一条，既够强又不越界。
         */
        CHECK(lvglcj_handle_state(cv) != LVGLCJ_HSTATE_ALIVE,
              "★ 删除后句柄不再报告 ALIVE（不限定具体终态，见注释）");
    }

    /* ================================================== 7d. P1 批次 2 控件
     *
     * 本段的重点不是"能创建"，而是三条**形状复用**是否真的复用到了行为上：
     *   · slider / arc / bar 共用同一段范围校验 —— 所以三者的「范围写反被拒」
     *     必须表现一致（如果只有一处生效，就是共享受损的信号）
     *   · 两者的读值同样必须走出参：**负值要能原样读回**
     *   · led 亮度边界必须拦在 0..255（LVGL 收 uint8_t，放行会静默截断）
     */
    printf("\n-- 7d. P1 批次 2 控件（slider / arc / led / spinner）--\n");
    {
        int64_t holder = lvglcj_obj_create(scr);
        CHECK(holder != 0, "创建承载父对象");

        int64_t sl = lvglcj_slider_create(holder);
        CHECK(sl != 0, "创建 slider");
        CHECK(lvglcj_slider_set_range(sl, 0, 100) == LVGLCJ_OK, "slider 设范围 0..100");
        CHECK(lvglcj_slider_set_value(sl, 42) == LVGLCJ_OK, "slider 设值 42");
        int32_t got = -1;
        CHECK(lvglcj_slider_get_value(sl, &got) == LVGLCJ_OK, "slider 读值");
        CHECK(got == 42, "★ slider 设完立刻读回即为新值");
        CHECK(lvglcj_slider_set_range(sl, -50, 50) == LVGLCJ_OK, "slider 负值范围合法");
        CHECK(lvglcj_slider_set_value(sl, -20) == LVGLCJ_OK, "slider 设负值");
        CHECK(lvglcj_slider_get_value(sl, &got) == LVGLCJ_OK && got == -20,
              "★ slider 负值原样读回（出参形式的意义所在）");
        CHECK(lvglcj_slider_set_range(sl, 100, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ slider 范围写反被拒（与 bar 同一段实现，行为必须一致）");
        CHECK(lvglcj_slider_get_value(sl, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "slider 出参为 NULL 被拒");

        int64_t ar = lvglcj_arc_create(holder);
        CHECK(ar != 0, "创建 arc");
        CHECK(lvglcj_arc_set_range(ar, 0, 360) == LVGLCJ_OK, "arc 设范围 0..360");
        CHECK(lvglcj_arc_set_value(ar, 90) == LVGLCJ_OK, "arc 设值 90");
        CHECK(lvglcj_arc_get_value(ar, &got) == LVGLCJ_OK && got == 90,
              "★ arc 设完立刻读回即为新值");
        CHECK(lvglcj_arc_set_range(ar, 5, 5) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "arc 范围 min == max 被拒（同上，一致性）");

        int64_t ld = lvglcj_led_create(holder);
        CHECK(ld != 0, "创建 led");
        CHECK(lvglcj_led_set_color(ld, 0xFF0000u) == LVGLCJ_OK, "led 设颜色");
        CHECK(lvglcj_led_set_brightness(ld, 0) == LVGLCJ_OK, "led 亮度 0 合法");
        CHECK(lvglcj_led_set_brightness(ld, 255) == LVGLCJ_OK, "led 亮度 255 合法");
        CHECK(lvglcj_led_set_brightness(ld, 256) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ led 亮度 256 被拒（放行会被 uint8_t 静默截断）");
        CHECK(lvglcj_led_set_brightness(ld, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "led 亮度为负被拒");
        CHECK(lvglcj_led_set_on(ld, 1) == LVGLCJ_OK, "led 开");
        CHECK(lvglcj_led_set_on(ld, 0) == LVGLCJ_OK, "led 关");

        int64_t sp = lvglcj_spinner_create(holder);
        CHECK(sp != 0, "创建 spinner（无属性可设，旋转由 LVGL 内部动画驱动）");
        CHECK(lvglcj_handle_state(sp) == LVGLCJ_HSTATE_ALIVE, "spinner 句柄登记为 ALIVE");

        /* 级联失效：删承载父对象后四个新控件的句柄都必须失效 */
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除承载父对象");
        CHECK(lvglcj_handle_state(sl) == LVGLCJ_HSTATE_INVALIDATED, "★ slider 句柄级联失效");
        CHECK(lvglcj_handle_state(ar) == LVGLCJ_HSTATE_INVALIDATED, "★ arc 句柄级联失效");
        CHECK(lvglcj_handle_state(ld) == LVGLCJ_HSTATE_INVALIDATED, "★ led 句柄级联失效");
        CHECK(lvglcj_handle_state(sp) == LVGLCJ_HSTATE_INVALIDATED, "★ spinner 句柄级联失效");
    }

    /* ================================================== 7e. P1 批次 3：dropdown
     *
     * 这一批引入新的值类型（字符串列表），所以重点是：
     *   · 选项字符串**确实被拷贝** —— 调用方（L1）传的是临时 CString，
     *     返回后即释放；若误用 set_options_static 而没拷贝，这里会读到已释放内存，
     *     ASan 下必报。也就是说这条断言同时钉住了"拷贝语义"这个契约。
     *   · 索引/个数与错误码不重叠，所以 get_* 直接返回数值而不是出参。
     */
    printf("\n-- 7e. P1 批次 3 控件（dropdown）--\n");
    {
        int64_t dd = lvglcj_dropdown_create(scr);
        CHECK(dd != 0, "创建 dropdown");

        CHECK(lvglcj_dropdown_set_options(dd, "one\ntwo\nthree") == LVGLCJ_OK, "设置 3 个选项");
        CHECK(lvglcj_dropdown_get_option_count(dd) == 3, "★ 选项个数读回 3（含拷贝语义）");
        /* 默认选中第 0 项：0 是合法答案，不是错误 —— 这正是 get_* 能直接返回值的前提 */
        CHECK(lvglcj_dropdown_get_selected(dd) == 0, "默认选中第 0 项（0 是答案不是错误）");
        CHECK(lvglcj_dropdown_set_selected(dd, 2) == LVGLCJ_OK, "选中第 2 项");
        CHECK(lvglcj_dropdown_get_selected(dd) == 2, "读回第 2 项");

        /* 重新设置选项：LVGL 会替换掉旧的（旧字符串由它自己释放） */
        CHECK(lvglcj_dropdown_set_options(dd, "a\nb") == LVGLCJ_OK, "改为 2 个选项");
        CHECK(lvglcj_dropdown_get_option_count(dd) == 2, "个数变成 2");

        CHECK(lvglcj_dropdown_set_options(dd, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "选项字符串为 NULL 被拒");
        CHECK(lvglcj_dropdown_set_selected(dd, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 负索引被拒（底层收 uint32_t，放行会变成巨大索引）");

        int64_t holder = lvglcj_obj_create(scr);
        int64_t dd2 = lvglcj_dropdown_create(holder);
        CHECK(lvglcj_dropdown_set_options(dd2, "x\ny\nz") == LVGLCJ_OK, "挂到父对象上的 dropdown");
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(dd2) == LVGLCJ_HSTATE_INVALIDATED, "★ dropdown 句柄级联失效");
        CHECK(lvglcj_dropdown_get_selected(dd2) < 0,
              "对已失效的 dropdown 取选中项：返回负错误码而非崩溃");

        CHECK(lvglcj_obj_delete(dd) == LVGLCJ_OK, "删除 dropdown");
    }

    /* ================================================== 7f. P1 批次 4：line
     *
     * line 是**第二类所有权**：LVGL 不拷贝点数组，只保存我们给的地址。
     * 所以本段最要紧的就是把"我们持有"这件事的两条路径都走一遍：
     *   · 重设点集（必须先释放旧的那块）
     *   · 删除对象（DELETE 钩子释放）
     * 二者若有闪失，就是泄漏或重复释放 —— 这正是 ASan 能精确抓到的类别。
     */
    printf("\n-- 7f. P1 批次 4 控件（line）--\n");
    {
        int64_t ln = lvglcj_line_create(scr);
        CHECK(ln != 0, "创建 line");

        /* 扁平坐标对：3 个点 */
        const int32_t pts[6] = { 0, 0, 10, 20, 30, 0 };
        CHECK(lvglcj_line_set_points(ln, pts, 3) == LVGLCJ_OK, "设置 3 个点");
        CHECK(lvglcj_line_set_y_invert(ln, 1) == LVGLCJ_OK, "y 轴反向开");
        CHECK(lvglcj_line_set_y_invert(ln, 0) == LVGLCJ_OK, "y 轴反向关");

        CHECK(lvglcj_line_set_points(ln, NULL, 3) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "坐标数组为 NULL 被拒");
        CHECK(lvglcj_line_set_points(ln, pts, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "点个数为 0 被拒");
        CHECK(lvglcj_line_set_points(ln, pts, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "点个数为负被拒");

        /* ★ 重设点集：内部先释放旧的那块，ASan 会在这里报泄漏 */
        const int32_t pts2[4] = { 0, 0, 5, 5 };
        CHECK(lvglcj_line_set_points(ln, pts2, 2) == LVGLCJ_OK, "★ 重设点集（旧的先释放）");
        /* 再来一次，确保反复重设也不漏 */
        CHECK(lvglcj_line_set_points(ln, pts, 3) == LVGLCJ_OK, "★ 再次重设点集");

        int64_t holder = lvglcj_obj_create(scr);
        int64_t ln2 = lvglcj_line_create(holder);
        CHECK(lvglcj_line_set_points(ln2, pts, 3) == LVGLCJ_OK, "挂到父对象上的 line");
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(ln2) == LVGLCJ_HSTATE_INVALIDATED, "★ line 句柄级联失效");

        /* ★ 删除自身：DELETE 钩子释放点数组；ASan 会报出泄漏或重复释放 */
        CHECK(lvglcj_obj_delete(ln) == LVGLCJ_OK, "★ 删除 line（点集随之释放）");
        CHECK(lvglcj_handle_state(ln) != LVGLCJ_HSTATE_ALIVE, "删除后不再报告 ALIVE");
    }

    /* ================================================== 7g. P1 批次 5：image
     *
     * ★ 本段刻意**不做"能加载出图像"的断言**：那需要一个真实的图像文件与解码器，
     *   属于端到端场景，不是控件契约的一部分。这里钉住的是契约本身：
     *   · 路径字符串被**拷贝**（调用方字符串可立即释放，见下）
     *   · 路径不存在时**不报错**（这是 LVGL 的既定行为，我们如实透传并写进契约）
     *   · 缩放的**单位**是 256 = 100%，非法取值被拦
     */
    printf("\n-- 7g. P1 批次 5 控件（image）--\n");
    {
        int64_t im = lvglcj_image_create(scr);
        CHECK(im != 0, "创建 image");

        /*
         * ★ 用**栈上的临时缓冲区**作路径，并在调用后立刻覆写它。
         *   这正是在验证"LVGL 会拷贝路径"这一契约：实现里是 lv_strdup，
         *   所以覆写调用方的缓冲区不该影响已设置的源。
         *   若哪天它改成"只存指针"，这个用例会在 ASan 下立刻报出使用已释放内存。
         */
        char path[32];
        strcpy(path, "A:/does/not/exist.bin");
        CHECK(lvglcj_image_set_src(im, path) == LVGLCJ_OK, "设置图像源（路径不存在）");
        memset(path, 0x5A, sizeof(path)); /* 覆写调用方缓冲区 */

        CHECK(lvglcj_image_set_offset(im, 3, -4) == LVGLCJ_OK, "设置偏移（含负值）");
        CHECK(lvglcj_image_set_scale(im, 256) == LVGLCJ_OK, "缩放 256 = 100%");
        CHECK(lvglcj_image_set_scale(im, 128) == LVGLCJ_OK, "缩放 128 = 50%");
        CHECK(lvglcj_image_set_scale(im, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 缩放 0 被拒（0 不是无缩放——那是 256）");
        CHECK(lvglcj_image_set_scale(im, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 缩放为负被拒（底层收 uint32_t，放行会变成巨大缩放值）");

        int64_t holder = lvglcj_obj_create(scr);
        int64_t im2 = lvglcj_image_create(holder);
        CHECK(lvglcj_image_set_src(im2, "A:/x.bin") == LVGLCJ_OK, "挂到父对象上的 image");
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(im2) == LVGLCJ_HSTATE_INVALIDATED, "★ image 句柄级联失效");

        /* 删除自身：LVGL 会释放它自己 dup 的那份路径字符串（它的账，但它必须不泄漏） */
        CHECK(lvglcj_obj_delete(im) == LVGLCJ_OK, "删除 image");
        CHECK(lvglcj_handle_state(im) != LVGLCJ_HSTATE_ALIVE, "删除后不再报告 ALIVE");
    }

    /* ================================================== 7h. P1 批次 6：roller
     *
     * 钉住三件读实现才知道的事：
     *   ① 两种模式**都拷贝**选项串（NORMAL 走 label，INFINITE 自己复制后 lv_free），
     *      所以传栈上临时串并在调用后覆写它是安全的；
     *   ② options == NULL 是**错误**而不是"清空"（与 image 相反，实现里有断言）；
     *   ③ 选中索引**须由本层校验**：底层收 uint32_t 且不钳制。
     */
    printf("\n-- 7h. P1 批次 6 控件（roller）--\n");
    {
        int64_t ro = lvglcj_roller_create(scr);
        CHECK(ro != 0, "创建 roller");

        char opts[32];
        strcpy(opts, "One\nTwo\nThree");
        CHECK(lvglcj_roller_set_options(ro, opts, LVGLCJ_ROLLER_MODE_NORMAL) == LVGLCJ_OK,
              "设置选项（NORMAL）");
        memset(opts, 0x5A, sizeof(opts)); /* 覆写调用方缓冲：钉住"拷贝"这条契约 */

        CHECK(lvglcj_roller_get_selected(ro) == 0, "初始选中第 0 项");
        CHECK(lvglcj_roller_set_selected(ro, 2) == LVGLCJ_OK, "选中第 2 项");
        CHECK(lvglcj_roller_get_selected(ro) == 2,
              "★ 读回一致（已关闭动画，设完即稳定，不是时序赌局）");
        CHECK(lvglcj_roller_set_selected(ro, 3) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 越界索引被拒（底层不钳制，必须由本层拦）");
        CHECK(lvglcj_roller_set_selected(ro, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "负索引被拒（转 uint32_t 会变成巨大索引）");
        CHECK(lvglcj_roller_set_options(ro, NULL, LVGLCJ_ROLLER_MODE_NORMAL)
                  == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ options 为 NULL 被拒（与 image 的 NULL 表示清空 相反）");
        CHECK(lvglcj_roller_set_options(ro, "A", 99) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "非法 mode 被拒");
        CHECK(lvglcj_roller_set_visible_row_count(ro, 3) == LVGLCJ_OK, "可见 3 行");
        CHECK(lvglcj_roller_set_visible_row_count(ro, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "可见 0 行被拒");

        /* 无限模式同样是拷贝，且对外选项数语义不变（get_option_count 会除回 inf_page_cnt） */
        char opts2[32];
        strcpy(opts2, "A\nB\nC");
        CHECK(lvglcj_roller_set_options(ro, opts2, LVGLCJ_ROLLER_MODE_INFINITE) == LVGLCJ_OK,
              "设置选项（INFINITE）");
        memset(opts2, 0x5A, sizeof(opts2));
        CHECK(lvglcj_roller_set_selected(ro, 1) == LVGLCJ_OK, "★ 无限模式下按对外索引选中");
        CHECK(lvglcj_roller_set_selected(ro, 3) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 无限模式下越界同样被拒（校验基准是对外选项数）");

        int64_t holder = lvglcj_obj_create(scr);
        int64_t ro2 = lvglcj_roller_create(holder);
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(ro2) == LVGLCJ_HSTATE_INVALIDATED, "★ roller 句柄级联失效");

        CHECK(lvglcj_obj_delete(ro) == LVGLCJ_OK, "删除 roller");
        CHECK(lvglcj_handle_state(ro) != LVGLCJ_HSTATE_ALIVE, "删除后不再报告 ALIVE");
    }

    /* ================================================== 7i. P1 批次 7：textarea
     *
     * 本段的重点全在 get_text 这个"第一个字符串取回入口"上：
     *   · 调用方缓冲被写入后，内容来自 LVGL 自己的拷贝（所以覆写过输入缓冲也没关系）；
     *   · 缓冲不足**必须报错且一个字都不写** —— 截断会得到一个"看起来对"的短字符串，
     *     那比报错更难发现；
     *   · 失败路径必须与"空文本"可区分（前者是负错误码，后者是长度 0）。
     */
    printf("\n-- 7i. P1 批次 7 控件（textarea）--\n");
    {
        int64_t ta = lvglcj_textarea_create(scr);
        CHECK(ta != 0, "创建 textarea");

        char txt[32];
        strcpy(txt, "hello");
        CHECK(lvglcj_textarea_set_text(ta, txt) == LVGLCJ_OK, "设置文本");
        memset(txt, 0x5A, sizeof(txt)); /* 覆写调用方缓冲：钉住"拷贝"这条契约 */

        char out[64];
        memset(out, 0, sizeof(out));
        CHECK(lvglcj_textarea_get_text(ta, out, sizeof(out)) == 5, "★ 取回长度为 5");
        CHECK(strcmp(out, "hello") == 0, "★ 内容正确（输入缓冲已被覆写，说明确是拷贝）");

        /* 探测用法：传 NULL 只回报长度，不要求调用方先知道要多大 */
        CHECK(lvglcj_textarea_get_text(ta, NULL, 0) == 5, "★ 传 NULL 只回报长度");

        /* 缓冲不足：报错，且不写入 —— 用哨兵字节验证"一个字都没写" */
        char small[3];
        memset(small, 0x11, sizeof(small));
        CHECK(lvglcj_textarea_get_text(ta, small, (int32_t)sizeof(small))
                  == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 缓冲不足被拒（不是截断成 hel）");
        CHECK((unsigned char)small[0] == 0x11 && (unsigned char)small[1] == 0x11
                  && (unsigned char)small[2] == 0x11,
              "★ 缓冲不足时一个字都没写（哨兵未被破坏）");

        /* 恰好放得下（长度+1 = 6）应当成功 —— 边界值必须能用 */
        char exact[6];
        CHECK(lvglcj_textarea_get_text(ta, exact, (int32_t)sizeof(exact)) == 5,
              "★ 缓冲恰好容纳 长度+1 时成功（边界值）");

        CHECK(lvglcj_textarea_set_text(ta, NULL) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "NULL 文本被拒（空文本应传空串）");
        CHECK(lvglcj_textarea_set_placeholder_text(ta, "type here") == LVGLCJ_OK, "占位文本");
        CHECK(lvglcj_textarea_set_one_line(ta, 1) == LVGLCJ_OK, "单行模式");
        CHECK(lvglcj_textarea_set_max_length(ta, 0) == LVGLCJ_OK, "最大长度 0 = 不限制");
        CHECK(lvglcj_textarea_set_max_length(ta, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 负最大长度被拒（转 uint32 会变成不限制）");

        /* 密码模式：显示成圆点，但取回的应是真实文本 */
        CHECK(lvglcj_textarea_set_password_mode(ta, 1) == LVGLCJ_OK, "开启密码模式");
        char pwd[64];
        memset(pwd, 0, sizeof(pwd));
        CHECK(lvglcj_textarea_get_text(ta, pwd, sizeof(pwd)) == 5,
              "★ 密码模式下可取回真实文本长度");
        CHECK(strcmp(pwd, "hello") == 0, "★ 取回的是真实文本，不是圆点");
        CHECK(lvglcj_textarea_set_password_mode(ta, 0) == LVGLCJ_OK, "关闭密码模式");

        /* 空文本：长度 0（与"失败"必须可区分） */
        CHECK(lvglcj_textarea_set_text(ta, "") == LVGLCJ_OK, "设为空文本");
        CHECK(lvglcj_textarea_get_text(ta, out, sizeof(out)) == 0, "★ 空文本返回长度 0");

        /* 中文按**字节**计：3 个汉字 = 9 字节（不是 3） */
        CHECK(lvglcj_textarea_set_text(ta, "中文测") == LVGLCJ_OK, "设置中文文本");
        CHECK(lvglcj_textarea_get_text(ta, out, sizeof(out)) == 9,
              "★ 多字节按字节计（3 个汉字 = 9 字节），与契约一致");

        /* 失效句柄：必须是负错误码，不能返回 0 冒充空文本 */
        int64_t holder = lvglcj_obj_create(scr);
        int64_t ta2 = lvglcj_textarea_create(holder);
        CHECK(lvglcj_obj_delete(holder) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(ta2) == LVGLCJ_HSTATE_INVALIDATED, "★ textarea 句柄级联失效");
        CHECK(lvglcj_textarea_get_text(ta2, out, sizeof(out)) < 0,
              "★ 失效句柄取文本返回负错误码（不是 0 冒充空文本）");

        CHECK(lvglcj_obj_delete(ta) == LVGLCJ_OK, "删除 textarea");
        CHECK(lvglcj_handle_state(ta) != LVGLCJ_HSTATE_ALIVE, "删除后不再报告 ALIVE");
    }

    /* ================================================== 8. 清理顺序 */
    printf("\n-- 8. 清理 --\n");
    /*
     * 注意这里刻意保留「样式已释放 → 再删对象」的顺序：
     * 它正是原 UAF 的触发条件。st 已从 btn 上摘下，所以删除是安全的；
     * 若守卫或 remove_style 的登记有遗漏，ASan 会在这一步报出来。
     */
    CHECK(lvglcj_obj_delete(lbl) == LVGLCJ_OK, "删除 label");
    CHECK(lvglcj_obj_delete(btn) == LVGLCJ_OK, "删除 button");
    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless 后端");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
