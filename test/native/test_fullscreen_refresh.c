/*
 * test_fullscreen_refresh.c —— 全屏反复失效的刷新路径压力测试
 *
 * ==================== 为什么单独有这么一个用例 ====================
 * 它来自一次真实崩溃：在 src/bench.cj 的 FPS 场景里，800×480 的 display 上
 * 放一个铺满屏幕的对象、在相距 100 px 的两个位置往复，几十帧后进程 SIGSEGV，
 * 且**崩溃点会漂移**（有时在局部刷新段、有时在全屏段）——
 * 这是内存破坏而非确定性逻辑错误的典型特征。
 *
 * ★ 混合进程（仓颉 + C）跑不了 ASan（见 docs/P0_RESULTS.md §12.4：ASan 的
 *   pthread_join 拦截器与仓颉运行时线程簿记硬冲突）。但**同一场景可以在纯 C 下
 *   原样复现** —— 这正是把 ASan 边界划在 C 侧的价值：这里能拿到精确的越界报告，
 *   而不是靠「崩在哪个函数」去猜。
 *
 * 覆盖的正是最容易出错的两条不变式：
 *   1. PARTIAL 缓冲必须容得下 LVGL 投递的每个 flush 块（缓冲高 × stride）；
 *   2. SDL sink 按 `area` 构造 SDL_Rect、按 `stride` 逐行读，
 *      因此 area 的高度×stride 必须落在缓冲内 —— 面积一旦超过缓冲高度就是越界读。
 */
#include "lvglcj_bridge.h"
#include "lvglcj_internal.h"
#include "lvglcj_backend_null.h"
#include "lvglcj_backend_sdl2.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

int main(void)
{
    printf("=== test_fullscreen_refresh：全屏往复失效下的刷新路径 ===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }

    /* ---- 1. 先用 headless 测「缓冲上限」这条不变式 ---- */
    printf("\n-- 1. 缓冲尺寸不变式（headless）--\n");
    {
        if (lvglcj_null_init(800, 480, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
            printf("  [FAIL] headless 初始化失败\n");
            return 1;
        }
        int64_t disp = lvglcj_null_display();
        int32_t stride = lvglcj_display_get_buf_stride(disp);
        int64_t bytes = lvglcj_display_get_buf_bytes(disp);
        printf("        stride=%d  buf_bytes=%lld  行数=%lld\n", stride, (long long)bytes,
               (long long)(stride > 0 ? bytes / stride : 0));
        CHECK(stride > 0, "stride 可用");
        /*
         * ★ PARTIAL 模式下 LVGL 每次投递的 flush 块高度不得超过缓冲行数。
         *   buf_bytes / stride 就是缓冲能容纳的行数 —— 它是硬约束。
         */
        CHECK(bytes / stride >= 48, "★ 缓冲至少容纳 1/10 屏（48 行 @480）");

        int64_t scr = lvglcj_screen_active();
        int64_t full = lvglcj_obj_create(scr);
        lvglcj_obj_set_size(full, 800, 480);

        int64_t frames = 0;
        for (int32_t i = 0; i < 400; i++) {
            lvglcj_obj_set_pos(full, (i % 2) ? 0 : 100, 0);
            (void)lvglcj_null_pump(1);
            frames++;
        }
        printf("        headless 全屏往复 %lld 帧完成\n", (long long)frames);
        CHECK(1, "★ headless 全屏往复 400 帧无异常");
        CHECK(lvglcj_obj_delete(full) == LVGLCJ_OK, "删除全屏对象");
        CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless");
    }

    /* ---- 1b. ★ 重复 init/deinit 循环 ----
     * 这一条来自 bench_cj 的真实结构：它先跑一轮 headless 微基准并 deinitLvgl()，
     * 再重新 initLvgl() 建 SDL2 后端跑 FPS。混合进程的崩溃就发生在这之后。
     *
     * 判据很直接：若 lvglcj_deinit 漏了某个子系统的重置（或留下悬空指针），
     * 第二次 init 之后就会带着脏状态继续跑 —— 表现为几十帧后才出现、
     * 且崩溃点会漂移的段错误。纯 C 下跑一遍就能用 ASan 精确定位。 */
    printf("\n-- 1b. init → deinit → init（bench_cj 的实际结构）--\n");
    {
        CHECK(lvglcj_deinit() == LVGLCJ_OK, "第一次 deinit");
        CHECK(lvglcj_init() == LVGLCJ_OK, "★ 第二次 init 成功（重复初始化）");

        /* 第二次 init 后必须能正常建显示、建对象、跑帧 —— 任一环节用到脏状态都会暴露 */
        CHECK(lvglcj_null_init(800, 480, LV_COLOR_FORMAT_RGB565, 0) == LVGLCJ_OK,
              "★ 第二次 init 后仍能创建 display");
        int64_t scr2 = lvglcj_screen_active();
        int64_t o2 = lvglcj_obj_create(scr2);
        CHECK(o2 != LVGLCJ_HANDLE_NULL, "★ 第二次 init 后仍能创建对象");
        lvglcj_obj_set_size(o2, 800, 480);
        for (int32_t i = 0; i < 200; i++) {
            lvglcj_obj_set_pos(o2, (i % 2) ? 0 : 100, 0);
            (void)lvglcj_null_pump(1);
        }
        CHECK(1, "★ 第二次 init 后全屏往复 200 帧无异常");
        CHECK(lvglcj_obj_delete(o2) == LVGLCJ_OK, "删除对象");
        CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless");
        CHECK(lvglcj_deinit() == LVGLCJ_OK, "第二次 deinit");
        CHECK(lvglcj_init() == LVGLCJ_OK, "第三次 init（进入 SDL2 段）");
    }

    /* ---- 2. 真实 SDL2 sink 路径（崩溃原始场景）---- */
    printf("\n-- 2. SDL2 sink 路径（800×480 PARTIAL，全屏往复）--\n");
#ifdef LVGLCJ_TEST_HAVE_SDL2
    {
        /*
         * 用 dummy 视频驱动：无窗口但走完整的 SDL_UpdateTexture 路径 ——
         * 崩溃发生在 SDL 按 stride 逐行读像素时，dummy 驱动同样会读。
         */
        if (lvglcj_sdl2_init(800, 480, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
            printf("  [skip] SDL2 不可用（无显示环境），跳过真实 sink 路径\n");
            CHECK(1, "SDL2 不可用：明确跳过（不伪装通过）");
            lvglcj_set_dispatch(NULL);
            lvglcj_deinit();
            printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
            return (g_fail == 0) ? 0 : 1;
        }

        int64_t disp = lvglcj_sdl2_display();
        int32_t stride = lvglcj_display_get_buf_stride(disp);
        int64_t bytes = lvglcj_display_get_buf_bytes(disp);
        printf("        stride=%d  buf_bytes=%lld  行数=%lld\n", stride, (long long)bytes,
               (long long)(bytes / stride));

        int64_t scr = lvglcj_screen_active();

        /* 约 50 个对象，与 bench 场景一致（制造真实重绘面） */
        int64_t boxes[50];
        for (int32_t i = 0; i < 50; i++) {
            boxes[i] = lvglcj_obj_create(scr);
            lvglcj_obj_set_pos(boxes[i], (i % 10) * 78, (i / 10) * 90);
            lvglcj_obj_set_size(boxes[i], 70, 80);
        }
        int64_t full = lvglcj_obj_create(scr);
        lvglcj_obj_set_size(full, 1, 1); /* 先小，避免影响局部刷新段 */

        /*
         * ★ 每轮必须 sleep 2ms（与 hello_cj 的实际帧节奏一致）。
         *
         *   紧循环 pump 时 lv_timer_handler 看到时间几乎没推进，刷新定时器
         *   （LV_DEF_REFR_PERIOD = 33ms）根本不到期 —— 实测 600 轮紧循环只产生
         *   **10 次 flush**，等于几乎没走到刷新路径上。
         *   而崩溃需要足够的刷新量才会出现，所以第一版复现「没崩」是假阴性：
         *   它压根没覆盖到出问题的代码。
         *   加 2ms 节奏后，3 秒 ≈ 90 次全屏刷新，与仓颉侧崩溃时的量级一致。
         */
        int32_t flush0 = lvglcj_sdl2_flush_count();
        for (int32_t i = 0; i < 1500; i++) {
            lvglcj_obj_set_pos(boxes[0], 10 + (i % 30), 0);
            (void)lvglcj_sdl2_poll_events();
            (void)lvglcj_pump();
            usleep(2000);
        }
        int32_t flushPartial = lvglcj_sdl2_flush_count() - flush0;
        printf("        局部刷新段 flush 次数 = %d（紧循环时只有个位数）\n", flushPartial);
        CHECK(flushPartial > 40, "★ 确实驱动了足够多的局部刷新（否则本用例是假阴性）");

        /* ---- 全屏段：放大到整屏，在相距 100 px 处往复 ---- */
        lvglcj_obj_set_size(full, 800, 480);
        int32_t flush1 = lvglcj_sdl2_flush_count();
        for (int32_t i = 0; i < 1500; i++) {
            lvglcj_obj_set_pos(full, (i % 2) ? 0 : 100, 0);
            (void)lvglcj_sdl2_poll_events();
            (void)lvglcj_pump();
            usleep(2000);
        }
        int32_t flushFull = lvglcj_sdl2_flush_count() - flush1;
        printf("        全屏往复段 flush 次数 = %d\n", flushFull);
        CHECK(flushFull > 40, "★ 确实驱动了足够多的全屏刷新（原崩溃场景）");

        /* ---- 收尾：逐个删除，只在失败时报（避免 50 条重复输出刷屏） ---- */
        CHECK(lvglcj_obj_delete(full) == LVGLCJ_OK, "删除全屏对象");
        int32_t delBad = 0;
        for (int32_t i = 0; i < 50; i++) {
            if (lvglcj_obj_delete(boxes[i]) != LVGLCJ_OK) {
                delBad++;
            }
        }
        CHECK(delBad == 0, "删除 50 个盒子全部成功");
        CHECK(lvglcj_sdl2_deinit() == LVGLCJ_OK, "反初始化 SDL2");
    }
#else
    /*
     * 未构建 SDL2（例如 aarch64 交叉构建：本机没有 arm64 的 SDL2 开发包）。
     * 明确说明「这一段没跑」，而不是静默少跑 —— 后者会让人以为覆盖了真实 sink 路径。
     * 上面的第 1、1b 节仍在跑，它们覆盖了缓冲尺寸不变式与重复 init/deinit。
     */
    printf("  [skip] 未构建 SDL2 后端 → 真实 sink 路径用例未运行（交叉构建下的已知裁剪）\n");
    CHECK(1, "SDL2 未构建：明确跳过（不伪装通过）");
#endif

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_set_dispatch(NULL);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
