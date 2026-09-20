/*
 * probe_v7_anim_deleted.c —— V7：lv_anim 的 deleted_cb 在几条路径下会触发？
 *
 * 为什么这个探针是 Patch P1 的前提（设计文档 §3.10.3 / R18）：
 *   我们的 anim_ctx 由 C 侧 malloc，挂在动画上，LVGL 不会释放它。
 *   Patch P1 的设计是「内部 deleted_cb 作为 ctx 的唯一释放点」——
 *   这**完全依赖** deleted_cb 在所有结束路径上都被调用。
 *   如果某条路径不触发，ctx 就永久泄漏（R18），必须改用
 *   「anim 句柄表 + 与 lv_anim_count_running() 对账」的兜底方案。
 *
 * 四条被测路径：
 *   path1 自然结束（duration 到、repeat 次数到）
 *   path2 手动删除（lv_anim_delete）
 *   path3 对象删除（变量就是 obj，LVGL 原生「对象删除自动停动画」路径）
 *   path4 对象删除但 var != obj（**这正是本项目 ADR-015 的实际形态**）
 *
 * path4 单独列出的原因：
 *   ADR-015 决定 var = anim_ctx（而非 obj），代价是 LVGL 原生的
 *   「对象删除 → 自动停该对象动画」失效，必须由 DELETE 钩子手动补偿。
 *   本探针用实测确认这个代价的严重程度（动画是否残留、ctx 是否泄漏）。
 */
#include "lvglcj_probe.h"
#include "lvglcj_bridge.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- 计数与回调 */
static int g_deleted[5];   /* 下标 1..4 */
static int g_exec[5];

/* ============================ headless display（path3/path4 的前置） ============================
 * 实测发现：LVGL v9 中没有 display 时 lv_screen_active() 返回 NULL，
 * 随后 lv_obj_create(NULL) 会触发断言并终止进程（"No display created yet"）。
 * 也就是说「对象必须挂在 display 上」是 LVGL 的硬约束，headless 也一样。
 * 因此本探针自己建一个最小 display —— 这同时顺带验证了 §3.4 的缓冲分配与 flush 链路。
 * ============================================================================================== */
#define V7_DISP_W     320
#define V7_DISP_H     240
#define V7_DISP_LINES 40
#define V7_DISP_BPP   2 /* RGB565 */

static uint8_t g_v7_buf[V7_DISP_W * V7_DISP_BPP * V7_DISP_LINES];
static int     g_v7_flush_calls;

static void v7_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    g_v7_flush_calls++;
    /* headless：不做实际传输，立即声明刷新完成 */
    lv_display_flush_ready(disp);
}

static lv_display_t *v7_ensure_display(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d != NULL) {
        return d;
    }

    d = lv_display_create(V7_DISP_W, V7_DISP_H);
    if (d == NULL) {
        return NULL;
    }

    /* ★ 缓冲尺寸一律调 LVGL 官方算子，绝不自己算（§3.4.2） */
    uint32_t stride = lv_draw_buf_width_to_stride(V7_DISP_W, LV_COLOR_FORMAT_RGB565);
    uint32_t bytes = stride * (uint32_t)V7_DISP_LINES;
    if (bytes > (uint32_t)sizeof(g_v7_buf)) {
        lvglcj_probe_report("V7", "display_buffer", "ERROR(静态缓冲过小)");
        return NULL;
    }

    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    /* PARTIAL 模式 + 40 行部分缓冲（§3.4.4 的 MVP 默认形态） */
    lv_display_set_buffers(d, g_v7_buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, v7_flush_cb);
    lv_display_set_default(d);
    return d;
}

static void v7_deleted_p1(lv_anim_t *a) { (void)a; g_deleted[1]++; }
static void v7_deleted_p2(lv_anim_t *a) { (void)a; g_deleted[2]++; }
static void v7_deleted_p3(lv_anim_t *a) { (void)a; g_deleted[3]++; }
static void v7_deleted_p4(lv_anim_t *a) { (void)a; g_deleted[4]++; }

static void v7_exec_p1(void *var, int32_t v) { (void)var; (void)v; g_exec[1]++; }
static void v7_exec_p2(void *var, int32_t v) { (void)var; (void)v; g_exec[2]++; }
static void v7_exec_p3(void *var, int32_t v) { (void)var; (void)v; g_exec[3]++; }
static void v7_exec_p4(void *var, int32_t v) { (void)var; (void)v; g_exec[4]++; }

/* --------------------------------------------------------------- 辅助工具 */
static void v7_pump_ms(int ms)
{
    /* tick 源是 CLOCK_MONOTONIC（conf_probe.c 注册），所以只需真实等待 + 处理定时器 */
    int loops = ms / 2 + 1;
    for (int i = 0; i < loops; i++) {
        lv_timer_handler();
        usleep(2000);
    }
}

static void v7_reset(int path)
{
    g_deleted[path] = 0;
    g_exec[path] = 0;
}

/* ================================================================ path1 */
static int probe_path1_natural_end(void)
{
    v7_reset(1);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL); /* 自然结束路径不关心 var */
    lv_anim_set_exec_cb(&a, v7_exec_p1);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 30);
    lv_anim_set_repeat_count(&a, 1); /* ★ P0 断言 8 用的就是 repeat(1) */
    lv_anim_set_deleted_cb(&a, v7_deleted_p1);
    lv_anim_start(&a);

    v7_pump_ms(120); /* 远超 30ms，确保动画跑完 */

    lvglcj_probe_report("V7", "path1_deleted_cb_fired",
                        (g_deleted[1] > 0) ? "YES" : "NO");
    return g_deleted[1];
}

/* ================================================================ path2 */
static int32_t g_p2_var;

static int probe_path2_manual_delete(void)
{
    v7_reset(2);
    g_p2_var = 0;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &g_p2_var);
    lv_anim_set_exec_cb(&a, v7_exec_p2);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 10000); /* 拉到很长，确保是「我们主动删」而不是自然结束 */
    lv_anim_set_deleted_cb(&a, v7_deleted_p2);
    lv_anim_start(&a);

    /* lv_anim_delete 是**按 (var, exec_cb) 键**删除 —— 这一点对 ADR-015 很重要：
       因为我们的 var = ctx，删除动画必须先能找回 ctx 才能构造出这个键 */
    lv_anim_delete(&g_p2_var, v7_exec_p2);

    lvglcj_probe_report("V7", "path2_deleted_cb_fired",
                        (g_deleted[2] > 0) ? "YES" : "NO");
    return g_deleted[2];
}

/* ================================================================ path3 */
static int probe_path3_obj_delete_var_is_obj(void)
{
    v7_reset(3);

    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    if (obj == NULL) {
        lvglcj_probe_report("V7", "path3_deleted_cb_fired", "ERROR(无对象)");
        return 0;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj); /* ★ 原生形态：var 就是被动画的对象 */
    lv_anim_set_exec_cb(&a, v7_exec_p3);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 10000);
    lv_anim_set_deleted_cb(&a, v7_deleted_p3);
    lv_anim_start(&a);

    lv_obj_delete(obj); /* 期望：LVGL 自动停掉该对象的动画并触发 deleted_cb */

    lvglcj_probe_report("V7", "path3_deleted_cb_fired",
                        (g_deleted[3] > 0) ? "YES" : "NO");
    return g_deleted[3];
}

/* ================================================================ path4 */
/* 模拟 ADR-015 的真实形态：var 是「上下文结构体」而不是 obj */
typedef struct {
    int64_t  obj_handle;
    int32_t  closure_id;
} v7_fake_ctx_t;

static v7_fake_ctx_t g_p4_ctx;

static int probe_path4_obj_delete_var_is_ctx(void)
{
    v7_reset(4);

    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    if (obj == NULL) {
        lvglcj_probe_report("V7", "path4_deleted_cb_fired", "ERROR(无对象)");
        return 0;
    }
    memset(&g_p4_ctx, 0, sizeof(g_p4_ctx));

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &g_p4_ctx); /* ★ var != obj（ADR-015 的形态） */
    lv_anim_set_exec_cb(&a, v7_exec_p4);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 10000);
    lv_anim_set_deleted_cb(&a, v7_deleted_p4);
    lv_anim_start(&a);

    uint16_t running_before = lv_anim_count_running();

    lv_obj_delete(obj);

    uint16_t running_after = lv_anim_count_running();

    /* ★ 在**对象删除这一刻**定格判读结果。
       下面的手动清理也会触发一次 deleted_cb，若不先定格就会污染 path4 的结论。 */
    int fired_by_obj_delete = g_deleted[4];

    /* 关键判读：
       若 running_after == running_before 且 deleted_cb 未触发，
       则证实「var != obj 时 LVGL 完全不管这个动画」→ 必须由 DELETE 钩子手动补偿 */
    char buf[160];
    snprintf(buf, sizeof(buf), "obj_delete_fired_deleted_cb=%s, running %u->%u",
             (fired_by_obj_delete > 0) ? "YES" : "NO",
             (unsigned)running_before, (unsigned)running_after);
    lvglcj_probe_report("V7", "path4_var_is_ctx_not_obj", buf);

    /* 把残留动画清掉，避免影响后续断言与退出清理（这会额外触发一次 deleted_cb） */
    lv_anim_delete(&g_p4_ctx, v7_exec_p4);

    return fired_by_obj_delete;
}

/* ================================================================== main */
int32_t lvglcj_probe_v7_anim_deleted(void)
{
    printf("\n=== V7: lv_anim deleted_cb 触发路径验证（Patch P1 前提）===\n");

    if (lvglcj_init() != LVGLCJ_OK) {
        printf("[V7] lvglcj_init 失败，无法继续\n");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }

    memset(g_deleted, 0, sizeof(g_deleted));
    memset(g_exec, 0, sizeof(g_exec));

    /* path3/path4 需要对象，而对象必须先有 display（见 v7_ensure_display 的说明） */
    if (v7_ensure_display() == NULL) {
        lvglcj_probe_report("V7", "display", "ERROR(无法创建 display)");
        lvglcj_deinit();
        return 1;
    }

    int p1 = probe_path1_natural_end();
    int p2 = probe_path2_manual_delete();
    int p3 = probe_path3_obj_delete_var_is_obj();
    int p4 = probe_path4_obj_delete_var_is_ctx();

    printf("\n[V7] 汇总：path1(自然结束)=%d  path2(手动删除)=%d  "
           "path3(对象删除,var=obj)=%d  path4(对象删除,var=ctx)=%d\n",
           p1, p2, p3, p4);

    if (p1 > 0 && p2 > 0 && p3 > 0) {
        printf("[V7] 结论：三条主路径均触发 deleted_cb "
               "→ Patch P1 的「internal deleted_cb 作唯一释放点」成立\n");
        if (p4 == 0) {
            printf("[V7] 注意：var!=obj 时对象删除不会自动停动画 "
                   "→ 证实 ADR-015 的代价，必须由 DELETE 钩子手动补偿\n");
        }
    } else {
        printf("[V7] 结论：存在不触发的路径 "
               "→ Patch P1 不成立，必须启用兜底（anim 句柄表 + 对账）\n");
    }

    lvglcj_deinit();
    return (p1 > 0 && p2 > 0 && p3 > 0) ? 0 : 1;
}

int main(void)
{
    return (lvglcj_probe_v7_anim_deleted() == 0) ? 0 : 1;
}
