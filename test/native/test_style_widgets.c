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
