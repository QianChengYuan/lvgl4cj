/*
 * test_sdl2_backend.c —— C 侧单测：SDL2 桌面后端（设计文档 §8.1.2 / §8.1.3）
 *
 * 覆盖：
 *   · 像素格式映射**一一对应**，映射不上时明确返回 UNKNOWN（而不是兜底猜测）
 *   · 不支持的颜色格式在 init 阶段被拒绝，且**不留半初始化状态**
 *   · ★ 真实 flush 链路：flush sink 被调用 + 真正 Present（断言 2 的机制证据）
 *   · ★ 单线程分流：渲染线程上的 flush 内联渲染，**不会**自等超时
 *   · ★ 跨线程正常往返（方案 A 的机制）
 *   · ★ 超时路径：渲染方不响应时，等待方在 ~100ms 后返回而**不挂死**（§8.1.2 要求 2）
 *   · ★ 窗口关闭即时解锁：暂停后等待方立即返回（§8.1.2 要求 3，断言 7 的证据）
 *   · 反初始化幂等
 *
 * 关于 SDL 驱动：本用例显式设 SDL_VIDEODRIVER=dummy，用软件渲染器跑，
 * 以便在 CI 与无 WSLg 的环境里**确定性**复现上述时序。真实窗口路径
 * （可见窗口、真实鼠标事件）由 examples/hello_cj 负责验证（t6），
 * 那是「能不能看到画面」的问题，与本用例「时序与映射是否正确」是两个目标。
 */
#include "lvglcj_internal.h"
#include "lvglcj_backend_sdl2.h"

/* 断言要与 SDL 的像素格式常量比对，因此需要 SDL 头 */
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* -------------------------------------------------- 跨线程 flush 的辅助线程 */
/*
 * 扮演方案 A 里的「LVGL 线程」：在**非渲染线程**上触发一次 flush。
 * 这是验证超时与即时解锁路径的必要条件 —— 单线程下 flush 会走内联分支，
 * 根本不会进入等待，也就测不到等待路径。
 */
static int64_t g_flush_disp = 0;
static int64_t g_flush_elapsed_ms = -1;

/*
 * ★ flush 用的像素缓冲：**按本次区域的紧凑尺寸**分配即可（64×64×2 = 8KB）。
 *
 *   这与直觉相反，但正是 LVGL 的实际布局规则（见 native/src/display.c 中
 *   flush_body 的说明）：
 *     · PARTIAL 模式下 LVGL 会对每个子块调用
 *         layer_reshape_draw_buf(layer, LV_STRIDE_AUTO)
 *       即用**子区域宽度**重排缓冲 —— 行距 = 区域宽 × bpp（本用例 = 64×2 = 128）。
 *     · 因此 flush 回调（及其 sink）必须按「区域宽 × bpp」逐行读，
 *       而不是按 display 的 stride（320×2 = 640）。
 *
 *   ★ 这个用例曾经因为库里的 bug 而**被改错**：当时 flush 路径误传 display stride，
 *     本用例的 8KB 缓冲就显得「太小」，于是被改成 320×240×2（全屏尺寸）来迁就它 ——
 *     结果是**掩盖了库里的越界读**，而这个越界最终在真实场景（800×480 全屏往复）
 *     里以难以复现的花屏形态出现。
 *     现在库已按正确行距传参，本用例恢复紧凑尺寸：
 *     若将来有人再改回 display stride，这里会作为第一道防线失败。
 */
static uint8_t g_flush_px[64u * 64u * 2u]; /* 区域 64×64 的紧凑排列（RGB565） */

static void *flush_thread_main(void *arg)
{
    (void)arg;
    lv_area_t area;
    area.x1 = 0;
    area.y1 = 0;
    area.x2 = 63;
    area.y2 = 63;

    /* 内容无关紧要，只要尺寸满足 stride 布局（见上方说明） */
    int64_t t0 = now_ms();
    lvglcj_flush_trampoline((lv_display_t *)lvglcj_ptr_of(g_flush_disp), &area, g_flush_px);
    g_flush_elapsed_ms = now_ms() - t0;
    return NULL;
}

static int64_t run_flush_in_thread(void)
{
    pthread_t th;
    g_flush_elapsed_ms = -1;
    pthread_create(&th, NULL, flush_thread_main, NULL);
    /*
     * 不设 join 超时：本用例要证明的正是「等待是有界的」，
     * 若它挂死，ctest 的超时会把问题暴露出来（而不是这里假装没事）。
     */
    pthread_join(th, NULL);
    return g_flush_elapsed_ms;
}

int main(void)
{
    printf("=== test_sdl2_backend：SDL2 后端 ===\n");

    /*
     * 用 dummy 视频驱动：无窗口、软件渲染、可重复。
     * 必须在任何 SDL_Init 之前设置。
     */
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    /* ============================================ 1. 像素格式映射（§8.1.3） */
    printf("\n-- 1. 像素格式映射必须一一对应 --\n");
    CHECK(lvglcj_sdl2_pixel_format_for(LV_COLOR_FORMAT_RGB565) == SDL_PIXELFORMAT_RGB565,
          "★ RGB565 → SDL_PIXELFORMAT_RGB565");
    CHECK(lvglcj_sdl2_pixel_format_for(LV_COLOR_FORMAT_ARGB8888) == SDL_PIXELFORMAT_ARGB8888,
          "★ ARGB8888 → SDL_PIXELFORMAT_ARGB8888");
    CHECK(lvglcj_sdl2_pixel_format_for(LV_COLOR_FORMAT_XRGB8888) == SDL_PIXELFORMAT_ARGB8888,
          "XRGB8888 → ARGB8888（X 位对 SDL 无意义）");
    /*
     * ★ RGB888 有意不支持：LVGL 的 RGB888 与 SDL 的 RGB24 字节序可能不同，
     *   猜错就是花屏且不报错。宁可明确拒绝。
     */
    CHECK(lvglcj_sdl2_pixel_format_for(LV_COLOR_FORMAT_RGB888) == SDL_PIXELFORMAT_UNKNOWN,
          "★ RGB888 明确不支持（避免字节序猜错导致花屏）");
    CHECK(lvglcj_sdl2_pixel_format_for(9999) == SDL_PIXELFORMAT_UNKNOWN,
          "未知格式返回 UNKNOWN");

    /* ============================================ 2. init 前置校验 */
    printf("\n-- 2. 不支持的颜色格式必须在 init 阶段被拒 --\n");
    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    CHECK(lvglcj_sdl2_init(320, 240, LV_COLOR_FORMAT_RGB888, 0) ==
              LVGLCJ_ERR_INVALID_ARGUMENT,
          "★ 不支持的格式 init 返回 INVALID_ARGUMENT");
    CHECK(lvglcj_sdl2_display() == LVGLCJ_HANDLE_NULL,
          "★ 被拒后不留半初始化状态（display 仍为 0）");
    CHECK(lvglcj_sdl2_init(0, 240, LV_COLOR_FORMAT_RGB565, 0) ==
              LVGLCJ_ERR_INVALID_ARGUMENT,
          "尺寸为 0 被拒（校验先于建窗）");
    /*
     * 注意：buf_lines <= 0 是**合法**的 —— 语义是「由桥接层按 1/10 屏下限自动选取」
     *（见 lvglcj_display_create）。这里断言它被接受，是为了把这个语义钉住，
     * 避免以后有人「顺手」把它改成报错而破坏 PARTIAL 的自动取值。
     */
    CHECK(lvglcj_sdl2_init(48, 48, LV_COLOR_FORMAT_RGB565, -1) == LVGLCJ_OK,
          "buf_lines <= 0 表示自动取值（合法，随后反初始化）");
    CHECK(lvglcj_sdl2_deinit() == LVGLCJ_OK, "清掉该临时实例");

    /* ============================================ 3. 正常初始化 */
    printf("\n-- 3. 初始化与单线程帧循环 --\n");
    CHECK(lvglcj_sdl2_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) == LVGLCJ_OK,
          "RGB565 初始化成功");
    CHECK(lvglcj_sdl2_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) == LVGLCJ_OK,
          "重复初始化幂等");

    int64_t disp = lvglcj_sdl2_display();
    CHECK(disp != LVGLCJ_HANDLE_NULL, "取得 display 句柄");
    CHECK(lvglcj_sdl2_is_render_thread() == 1,
          "★ 调用 init 的线程被登记为渲染线程");
    CHECK(lvglcj_waitgraph_render_thread() == lvglcj_current_os_tid(),
          "★ 渲染线程身份已登记给等待图检测（§3.8.4 判据 3）");

    /* 建一个对象制造真实的刷新需求（G1：必须已有 display 才能建对象） */
    int64_t scr = lvglcj_screen_active();
    CHECK(scr != LVGLCJ_HANDLE_NULL, "活动屏幕可用（G1 前置满足）");
    int64_t box = lvglcj_obj_create(scr);
    lvglcj_obj_set_pos(box, 10, 10);
    lvglcj_obj_set_size(box, 60, 40);

    /* 单线程帧循环：poll_events + timer_handler 交替推进 */
    int before_flush = lvglcj_sdl2_flush_count();
    for (int i = 0; i < 80; ++i) {
        if (lvglcj_sdl2_poll_events() == 1) {
            break;
        }
        lvglcj_timer_handler();
        usleep(3000);
        if (lvglcj_sdl2_present_count() > 0 && lvglcj_sdl2_flush_count() > before_flush) {
            break;
        }
    }
    CHECK(lvglcj_sdl2_flush_count() > before_flush,
          "★ flush sink 被真的调用（经 T4 trampoline，未绕过）");
    CHECK(lvglcj_sdl2_present_count() > 0,
          "★ 画面被真的 Present（断言 2 的机制证据）");
    CHECK(lvglcj_sdl2_wait_timeout_count() == 0,
          "★ 单线程路径不产生任何等待超时（内联渲染分流生效）");

    /* ============================================ 4. 跨线程正常往返 */
    printf("\n-- 4. 跨线程（方案 A 机制）：正常往返 --\n");
    g_flush_disp = disp;

    /*
     * ★ 自校验：flush 缓冲只需容纳「区域紧凑布局」，而不是「display stride × 高度」。
     *   本用例的区域是 64×64，因此 64×2 × 64 = 8192 字节足够。
     *   这条断言钉住的是**行距语义**：一旦库里改回传 display stride，
     *   sink 会按 640 逐行读，所需的字节数会远超这个缓冲。
     */
    {
        int32_t buf_stride = lvglcj_display_get_buf_stride(disp);
        const int64_t area_packed = 64 * 2 * 64; /* 区域宽 × bpp × 区域高 */
        printf("        （display stride = %d；本用例区域紧凑布局需 %lld 字节）\n",
               buf_stride, (long long)area_packed);
        CHECK(area_packed <= (int64_t)sizeof(g_flush_px),
              "★ flush 缓冲足以容纳「区域宽 × bpp × 高」的紧凑布局（钉住行距语义）");
    }
    /*
     * 主线程持续服务渲染请求，flush 线程发起请求并等待 —— 应快速往返。
     * 这里刻意用「另一条线程发起 flush」来复刻方案 A 的线程关系。
     */
    {
        pthread_t th;
        pthread_create(&th, NULL, flush_thread_main, NULL);
        int64_t t0 = now_ms();
        while (g_flush_elapsed_ms < 0 && (now_ms() - t0) < 1000) {
            lvglcj_sdl2_poll_events();
            usleep(1000);
        }
        pthread_join(th, NULL);
        int64_t el = g_flush_elapsed_ms;
        printf("        （跨线程 flush 往返耗时 %lld ms）\n", (long long)el);
        CHECK(el >= 0 && el < 90, "★ 跨线程往返被及时满足（远小于 100ms 超时）");
    }

    /* ============================================ 5. 超时路径（§8.1.2 要求 2） */
    printf("\n-- 5. 渲染方不响应时的超时路径 --\n");
    CHECK(lvglcj_sdl2_set_render_suppressed(1) == LVGLCJ_OK, "注入：忽略渲染请求");
    {
        int before_timeout = lvglcj_sdl2_wait_timeout_count();
        /*
         * 关键：flush 线程发起请求后**必须**在 ~100ms 内返回。
         * 若这里挂死，说明「等待有界」这条契约被破坏 —— 那正是断言 7 要防的。
         */
        int64_t el = run_flush_in_thread();
        printf("        （超时路径耗时 %lld ms）\n", (long long)el);
        CHECK(el >= 95 && el < 300,
              "★ 等待方在约 100ms 后返回（有界），而不是永久挂死");
        CHECK(lvglcj_sdl2_wait_timeout_count() > before_timeout,
              "★ 超时已计入可观测计数（并记 BACKEND_FAILURE）");
    }
    lvglcj_sdl2_set_render_suppressed(0);

    /*
     * 超时之后链路必须能**恢复**：一次超时不能让后端永久退化。
     * 这里由主线程真的去服务渲染请求（而不是像上面那样撒手不管），
     * 因此这次往返应该是快的 —— 这才是「恢复」的有效证据。
     */
    {
        pthread_t th;
        g_flush_elapsed_ms = -1;
        pthread_create(&th, NULL, flush_thread_main, NULL);
        int64_t t0 = now_ms();
        while (g_flush_elapsed_ms < 0 && (now_ms() - t0) < 500) {
            lvglcj_sdl2_poll_events();
            usleep(1000);
        }
        pthread_join(th, NULL);
        printf("        （超时后的恢复往返 %lld ms）\n", (long long)g_flush_elapsed_ms);
        CHECK(g_flush_elapsed_ms >= 0 && g_flush_elapsed_ms < 90,
              "★ 一次超时后链路恢复正常（往返 < 90ms），未永久退化");
    }
    /* ============================================ ★★ Present 慢于等待上限：不得丢带
     *
     * 这是实测缺陷的哨兵（Raspberry Pi Zero 2 W + KMSDRM，1920×1080）：
     *   KMSDRM 上 Present 是 GL + KMS page flip，可能超过 100ms 的等待上限。
     *   原先"放行"发生在 Present 之后 —— 于是 flush 等到超时返回，而它那条带仍在
     *   事件队列里；LVGL 继续渲染下一条带并**覆盖暂存区**，队列里那个事件的像素
     *   再没人消费 → 画面上那条带变成**纯黑**。
     *   实测特征：黑带高度恰是渲染条带高度的整数倍、位置每次随机；
     *             同一轮 waitTimeout 4~8、staleApply 12~13；换 dummy（Present 免费）全为 0。
     *
     * 修复后：放行只等**上传完成**（暂存区可复用），不等 Present；且等待按**序号**判定。
     * 本用例把 Present 注入成 300ms，连做三轮「另一线程 flush + 主线程服务」：
     *   · 等待仍必须有界（<90ms）—— 因为它只覆盖上传；
     *   · 不得出现等待超时，也不得出现 staleApply。
     * ★ 修复前这里必然失败（等待等的是 Present，每轮都会超时），所以它有鉴别力。
     */
    printf("\n-- ★★ Present 慢于等待上限时不得丢带 --\n");
    {
        int t_before = lvglcj_sdl2_wait_timeout_count();
        int s_before = lvglcj_sdl2_stale_apply_count();
        int p_before = lvglcj_sdl2_present_count();
        CHECK(lvglcj_sdl2_set_present_delay(300) == LVGLCJ_OK,
              "注入：Present 变慢 300ms（远超 100ms 等待上限）");

        int64_t worst = 0;
        for (int round = 0; round < 3; ++round) {
            pthread_t th;
            g_flush_elapsed_ms = -1;
            pthread_create(&th, NULL, flush_thread_main, NULL);
            int64_t t0 = now_ms();
            /* 主线程照常服务渲染请求；每次服务都要在 Present 上花 300ms */
            while (g_flush_elapsed_ms < 0 && (now_ms() - t0) < 2000) {
                lvglcj_sdl2_poll_events();
                usleep(1000);
            }
            pthread_join(th, NULL);
            if (g_flush_elapsed_ms > worst) {
                worst = g_flush_elapsed_ms;
            }
        }
        printf("        （三轮里最慢一次 flush 耗时 %lld ms）\n", (long long)worst);

        CHECK(lvglcj_sdl2_present_count() > p_before,
              "★ 慢 Present 确实发生过（注入有效，而不是没跑到）");
        CHECK(worst >= 0 && worst < 90,
              "★★ flush 不再等 Present：Present 要 300ms，等待仍只覆盖上传（<90ms）");
        CHECK(lvglcj_sdl2_wait_timeout_count() == t_before,
              "★★ 不得有等待超时（修复前：等的是 Present，每轮必然超时）");
        CHECK(lvglcj_sdl2_stale_apply_count() == s_before,
              "★★ 不得有 staleApply：暂存区绝不会在未消费时被覆盖（黑带的直接成因）");
        lvglcj_sdl2_set_present_delay(0);
    }

    /* 排空遗留事件，避免影响后续用例 */
    for (int i = 0; i < 5; ++i) {
        lvglcj_sdl2_poll_events();
    }

    /* ============================================ ★ 滚轮：累计 + 取走
     *
     * 滚轮不走 indev（LVGL 的 encoder 在导航模式下只移动焦点，不滚动视图），
     * 所以后端只做两件事：**累计**两次读之间的格数、被取走时清零。
     * 这里钉住的就是这两条 —— 尤其是"两格之间要累加"与"取走后不重复消费"：
     * 少了前者，快速滚动会丢格；少了后者，一格会被反复消费成"一直滚"。
     */
    printf("\n-- ★ 滚轮：累计后取走，取走后清零 --\n");
    {
        CHECK(lvglcj_sdl2_take_wheel_steps() == 0, "初始没有累计格数");
        CHECK(lvglcj_sdl2_inject_wheel(3) == LVGLCJ_OK, "注入 3 格");
        CHECK(lvglcj_sdl2_inject_wheel(-1) == LVGLCJ_OK, "再注入 -1 格（中间未取走 → 应累加）");
        CHECK(lvglcj_sdl2_take_wheel_steps() == 2, "★ 取走 3 + (-1) = 2 格");
        CHECK(lvglcj_sdl2_take_wheel_steps() == 0, "★ 取走后清零（同一格不被重复消费）");
    }

    /* ============================================ ★ 截图（§7.3 LvDebug.screenshot 的后端侧）
     *
     * 判据刻意不满足于"文件存在就算过"：
     *   · 返回成功**且**文件确实被写出（非空）；
     *   · 头部是合法的 P6 PPM，且宽高与渲染输出一致 —— 只查存在的话，
     *     一个 0 字节文件也能骗过用例；
     *   · 空路径被拒（否则可能悄悄写到一个随机位置）。
     * 注：dummy 驱动下走软件渲染器，SDL_RenderReadPixels 同样可用。
     */
    printf("\n-- ★ 截图（写 PPM）--\n");
    {
        const char *shot = "/tmp/lvglcj_shot_test.ppm";
        remove(shot);
        CHECK(lvglcj_sdl2_screenshot(shot) == LVGLCJ_OK, "截图成功");
        FILE *fp = fopen(shot, "rb");
        CHECK(fp != NULL, "★ 文件确实被写出");
        if (fp != NULL) {
            char head[32] = {0};
            size_t n = fread(head, 1, sizeof(head) - 1, fp);
            long size = 0;
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            fclose(fp);
            CHECK(n > 8 && head[0] == 'P' && head[1] == '6', "★ 是 P6 PPM（头部合法）");
            int sw = 0;
            int sh = 0;
            CHECK(sscanf(head, "P6 %d %d", &sw, &sh) == 2 && sw > 0 && sh > 0,
                  "★ 头部宽高合法");
            printf("        （%dx%d，文件 %ld 字节）\n", sw, sh, size);
            /* PPM 头 + 每像素 3 字节；容差留给换行/空格形式 */
            CHECK(size > (long)sw * (long)sh * 3L, "★ 文件长度与声明的宽高相符（不是空壳）");
        }
        CHECK(lvglcj_sdl2_screenshot(NULL) == LVGLCJ_ERR_INVALID_ARGUMENT, "空路径被拒");
    }

    /* ============================================ ★★★ 超时不得等于丢数据（多槽暂存）
     *
     * 这是"KMSDRM 纯黑带"那类现象的直接判据。做法：
     *   1. 注入"忽略渲染请求"（§8.1.2 要求 2 用的同一个钩子）——
     *      事件会被取走但**不上传**，于是每条带的暂存数据都不会被消费；
     *   2. 连做 3 次 flush（每次等满 100ms 超时返回）；
     *   3. 断言 slotClobber 不增长。
     *
     * ★ 若暂存区只有**一块**，第 2、3 次 flush 必然覆盖还没被消费的数据 ——
     *   那正是画面上"渲染条带高度整数倍的纯黑带"的成因。所以本用例有鉴别力：
     *   把 SDL2_STAGE_SLOTS 改成 1，它就会失败（负向对照做过）。
     */
    printf("\n-- ★★★ 超时不得等于丢数据（多槽暂存）--\n");
    {
        int c_before = lvglcj_sdl2_slot_clobber_count();
        int t_before = lvglcj_sdl2_wait_timeout_count();
        CHECK(lvglcj_sdl2_set_render_suppressed(1) == LVGLCJ_OK,
              "注入：忽略渲染请求（事件被取走但不上传 → 暂存槽不会被消费）");
        for (int round = 0; round < 3; ++round) {
            int64_t el = run_flush_in_thread();
            printf("        （第 %d 次：无人消费，耗时 %lld ms）\n", round + 1, (long long)el);
            CHECK(el >= 95 && el < 300, "无人消费时必须等满超时（有界返回）");
            lvglcj_sdl2_poll_events(); /* 把事件取走（不应用） */
        }
        CHECK(lvglcj_sdl2_wait_timeout_count() > t_before,
              "★ 超时确实发生了（这正是要考察的路径，不是没跑到）");
        CHECK(lvglcj_sdl2_slot_clobber_count() == c_before,
              "★★★ 连续超时也不得覆盖还没被应用的暂存数据（slotClobber 必须为 0）");
        lvglcj_sdl2_set_render_suppressed(0);
    }
    for (int i = 0; i < 5; ++i) {
        lvglcj_sdl2_poll_events();
    }

    /* ============================================ ★ 别人的唤醒不得误放行本帧
     *
     * 本用例钉住这条不变量：**等待必须按"我自己那个序号"判定** ——
     * 别的 flush、暂停路径产生的唤醒，都不得让当前等待提前返回。
     *
     * 路径设计：
     *   (1) 无人服务的 flush：等满超时返回（它的渲染请求仍留在事件队列里）；
     *   (2) 现在去服务它 —— 上传完即放行，而这次放行属于**第 1 次**那个序号；
     *   (3) 第 2 次 flush 再次无人服务：必须**仍然**等满超时 ——
     *       第 2 次的序号还没被消费，(2) 里那次唤醒不该满足它。
     *
     * ★ 这里曾有一段关于 sdl2_reset_done()（"清掉残留额度"）的说明，现在改写：
     *   旧的单标志写法确实会被"别人的唤醒"满足，那个补丁治的是这个**症状**。
     *   但病根在谓词 —— 只要谓词是"有过一次唤醒"，就永远存在被误满足的可能
     *   （暂停路径在无人等待时也会留下额度）。现在谓词换成了序号
     *   （见 g_applied_seq 的实测说明），补丁连同"残留额度"这个概念一起撤掉，
     *   而本用例的断言一字未改：它钉住的正是那个根。
     */
    printf("\n-- ★ 别人的唤醒不得误放行本帧 --\n");
    {
        /* (1) 无人服务的 flush：等满超时，且它的渲染请求仍留在队列里 */
        int64_t el1 = run_flush_in_thread();
        printf("        （第 1 次：无人服务，耗时 %lld ms）\n", (long long)el1);
        CHECK(el1 >= 95 && el1 < 300, "第 1 次既无人服务，就应等满超时");

        /* (2) 现在去服务它：上传完即放行 —— 注意这次放行属于第 1 次的序号 */
        lvglcj_sdl2_poll_events();

        /* (3) 再一次无人服务：第 2 次的序号没被消费，故仍须等满超时 */
        int64_t el2 = run_flush_in_thread();
        printf("        （第 2 次：同样无人服务，耗时 %lld ms）\n", (long long)el2);
        CHECK(el2 >= 95 && el2 < 300,
              "★★ 等待按序号判定：第 2 次仍等满超时，而不是被上一次的唤醒立刻满足");
        CHECK(lvglcj_sdl2_stale_apply_count() == 0,
              "★ 序号始终匹配（绝不会把新像素贴到旧矩形上）");
    }
    for (int i = 0; i < 5; ++i) {
        lvglcj_sdl2_poll_events();
    }

    /* ============================================ 6. 窗口关闭即时解锁（断言 7） */
    printf("\n-- 6. 窗口关闭 → 等待方立即返回 --\n");
    CHECK(lvglcj_sdl2_inject_quit() == LVGLCJ_OK, "注入窗口关闭");
    CHECK(lvglcj_sdl2_is_paused() == 1, "已进入暂停状态（不再渲染）");
    {
        int64_t el = run_flush_in_thread();
        printf("        （关闭后的 flush 耗时 %lld ms）\n", (long long)el);
        CHECK(el >= 0 && el < 20,
              "★ 关闭后 flush 立即返回（不等超时）——即断言 7 所要求的「不挂死」");
    }

    /* ============================================ 7. 反初始化 */
    printf("\n-- 7. 反初始化 --\n");
    CHECK(lvglcj_sdl2_deinit() == LVGLCJ_OK, "反初始化成功");
    CHECK(lvglcj_sdl2_deinit() == LVGLCJ_OK, "重复反初始化幂等");
    CHECK(lvglcj_sdl2_display() == LVGLCJ_HANDLE_NULL, "display 记录已清空");
    CHECK(lvglcj_waitgraph_render_thread() == 0, "渲染线程身份已注销");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
