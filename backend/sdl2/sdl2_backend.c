/*
 * sdl2_backend.c —— SDL2 桌面后端实现
 *
 * 设计说明见 lvglcj_backend_sdl2.h。这里只补充实现层面必须写下来的几件事：
 *
 * 1) 像素格式映射**必须一一对应**（§8.1.3）。
 *    映射不上就**明确拒绝**，绝不「猜一个相近的格式」—— 猜错的后果是花屏，
 *    而花屏不会报错、不会崩溃，只会让人以为是渲染逻辑写错了，极难定位。
 *    因此 RGB888 有意列为「不支持」：LVGL 的 RGB888 与 SDL 的 RGB24
 *    字节序可能不同，P0 用 RGB565（既定目标格式）不涉及该风险，
 *    要支持 RGB888 必须先实测字节序再打开。
 *
 * 2) 渲染与「谁调用它」解耦。
 *    do_render_now() 是唯一的渲染实现，单线程与双线程路径都调它，
 *    因此**不会出现「测试路径通过但生产路径不同」**的分叉。
 *
 * 3) 每次等待都有界，且**无论成功还是超时都必须 flush_ready**。
 *    超时不放行会让 LVGL 永久停在「等 flush 完成」，表现为界面卡死；
 *    而这比丢一帧严重得多。宁可丢帧 + 记 BACKEND_FAILURE，也不卡死。
 *
 * 4) 窗口关闭/最小化必须**主动唤醒等待方**（§8.1.2 强制要求 3），
 *    否则等待方要等到超时才返回 —— 这正是 P0 断言 7 要测的东西。
 */
#include "lvglcj_backend_sdl2.h"
#include "lvglcj_internal.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* §8.1.2 默认超时；两个等待点共用 */
#define SDL2_WAIT_TIMEOUT_MS 100

static SDL_Window   *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture = NULL;
static uint32_t      g_render_event = 0;
static int32_t       g_tex_w = 0;
static int32_t       g_tex_h = 0;
static int64_t       g_display = LVGLCJ_HANDLE_NULL;

/* 创建窗口的线程 = 渲染线程 = SDL 事件循环线程 */
static _Atomic int64_t g_render_thread_tid = 0;

/*
 * 「渲染完成 / 主动解锁」信号：用 **mutex + condvar + 标志** 实现，
 * 而不是 POSIX 未命名信号量（sem_t）。
 *
 * ★ 为什么必须换掉（由 macOS CI 作业实测给出，不是预防性改动）：
 *   · `sem_timedwait` 在 macOS 上**根本没有声明** ——
 *     编译直接失败：call to undeclared function 'sem_timedwait'；
 *   · 更根本的是：macOS **不支持未命名 POSIX 信号量** ——
 *     `sem_init(..., pshared=0, ...)` 在运行期返回 ENOSYS。
 *     也就是说即使绕过编译错误，这里的等待在 macOS 上也**永远不会被唤醒**，
 *     只会在每次 flush 时白等到超时（表现为帧率塌掉而非报错）。
 *   pthread 的 mutex/condvar 在 Linux / macOS / arm64 上都完整支持，
 *   所以这不是"为了让 macOS 编过"的权宜改动，而是换成一个**真正可移植**的原语。
 *
 * 语义差异（已核对本处协议后确认可接受）：
 *   · 信号量**计数**，标志**合并**：生产者若在无人等待时连续 post 多次，
 *     信号量会攒下多个额度，而标志只记住"有一次"。
 *   · 但本处的协议是「flush → 等渲染完成」的**请求/应答**，
 *     同一时刻最多只有一个未决额度（等待方拿到即清零）。
 *     因此二者在本协议下等价 —— 这也就是标志写法能成立的前提。
 *     ★ 若将来改成"多生产者并发 post"，必须回到计数语义。
 */
static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_done_cond = PTHREAD_COND_INITIALIZER;
static int             g_done_flag = 0;

/* 复位额度（init/deinit 用）。静态初始化的 mutex/condvar 本身无需销毁。 */
static void sdl2_reset_done(void)
{
    pthread_mutex_lock(&g_done_lock);
    g_done_flag = 0;
    pthread_mutex_unlock(&g_done_lock);
}

/*
 * 唤醒正在等待的渲染方。
 *
 * ★ 窗口关闭/最小化路径上的这一句是**必需**的（§8.1.2 强制要求 3）：
 *   没有它，正在 flush 里等待的 LVGL 线程会一直等到超时。
 */
static void sdl2_signal_done(void)
{
    pthread_mutex_lock(&g_done_lock);
    g_done_flag = 1;
    pthread_cond_signal(&g_done_cond);
    pthread_mutex_unlock(&g_done_lock);
}

static _Atomic int g_paused = 0;          /* 窗口关闭或最小化 */
static _Atomic int g_quit_requested = 0;  /* 请求退出主循环 */
static _Atomic int g_pending = 0;         /* 有渲染请求在途 */
static _Atomic int g_render_suppressed = 0; /* 测试注入：忽略渲染请求 */

static _Atomic int32_t g_flush_count = 0;
static _Atomic int32_t g_present_count = 0;
static _Atomic int32_t g_wait_timeout_count = 0;

/* 输入状态：主线程写，LVGL 线程读（全部原子，无需锁） */
static _Atomic int32_t g_in_x = 0;
static _Atomic int32_t g_in_y = 0;
static _Atomic int32_t g_in_pressed = 0;
static _Atomic int32_t g_in_key = 0;
static _Atomic int32_t g_in_key_pressed = 0;

/* ------------------------------------------------------------ 工具 */

/* 有界等待渲染完成。返回 0 = 被唤醒，-1 = 超时 */
static int sdl2_wait_done(int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    int rc = -1;
    pthread_mutex_lock(&g_done_lock);
    /*
     * 用 while 而不是 if：condvar 允许**虚假唤醒**，
     * 被唤醒后必须重新检查条件。写成 if 会在虚假唤醒时误判为"渲染完成"，
     * 于是 flush 提前返回、画面撕裂 —— 而且只在特定平台上偶发，极难复现。
     */
    while (g_done_flag == 0) {
        if (pthread_cond_timedwait(&g_done_cond, &g_done_lock, &deadline) != 0) {
            break; /* 超时或出错：与旧实现 sem_timedwait 非 0 时同样返回 -1 */
        }
    }
    if (g_done_flag != 0) {
        g_done_flag = 0; /* 消费这一次额度 */
        rc = 0;
    }
    pthread_mutex_unlock(&g_done_lock);
    return rc;
}

static int sdl2_on_render_thread(void)
{
    int64_t tid = atomic_load(&g_render_thread_tid);
    return (tid != 0 && tid == lvglcj_current_os_tid()) ? 1 : 0;
}

/* ------------------------------------------------------------ 像素格式映射 */

uint32_t lvglcj_sdl2_pixel_format_for(int32_t lv_color_format)
{
    switch (lv_color_format) {
        case LV_COLOR_FORMAT_RGB565:
            return SDL_PIXELFORMAT_RGB565;
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            /* 两者都是 32 位、通道顺序一致；X 位对 SDL 而言无意义 */
            return SDL_PIXELFORMAT_ARGB8888;
        default:
            /*
             * RGB888 / RGB565A8 / L8 等在这里被明确拒绝，理由见文件头第 1 点。
             * 返回 UNKNOWN 而不是兜底格式，是为了让调用方**不得不**处理它。
             */
            return SDL_PIXELFORMAT_UNKNOWN;
    }
}

/* ------------------------------------------------------------ 渲染 */

/*
 * 唯一的渲染实现。单线程与双线程路径都调它 —— 保证两条路径行为一致。
 * 纹理内容由 flush sink 通过 SDL_UpdateTexture 更新，这里只做拷贝与呈现。
 */
static void sdl2_render_now(void)
{
    if (g_renderer == NULL || g_texture == NULL) {
        return;
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    atomic_fetch_add(&g_present_count, 1);
}

/*
 * 标记暂停并解锁等待方。
 * ★ 这次唤醒是必需的：没有它，正在 flush 里等待的 LVGL 线程
 *   要等到 100ms 超时才返回（断言 7 要的正是「立即返回」）。
 */
static void sdl2_mark_paused(int quit)
{
    atomic_store(&g_paused, 1);
    if (quit) {
        atomic_store(&g_quit_requested, 1);
    }
    sdl2_signal_done();
}

/* ------------------------------------------------------------ flush sink */

/*
 * ★★ 上传必须在**渲染线程**上做，不能在 LVGL 线程上做。
 *
 *   实测结论（探针：probe/sdl_thread_texture_probe.c，三组对照）：
 *     · 直接在 renderer 上画 + 回读        → 63488（红，说明回读通路是好的）
 *     · 主线程 SDL_UpdateTexture + 回读    → 63488（红，说明主线程上传能到屏幕）
 *     · 换一个线程 SDL_UpdateTexture + 回读 → 仍是旧的红（期望蓝 31）
 *   也就是说：**跨线程的 SDL_UpdateTexture 被静默丢弃**，不报错、不生效。
 *   SDL2 的 renderer 本来就要求只由创建它的线程使用，这是它的既有约束。
 *
 *   这正是「窗口全黑、但 flush==present、waitTimeout==0」的原因：
 *   账簿全对（事件投递、等待、唤醒都正常），只是贴上去的纹理从来没被填过。
 *
 *   做法：flush 时只把区域参数存下来，由渲染线程在上传+渲染时一并应用。
 *   安全性来自既有的等待协议 —— flush 随后会阻塞等渲染完成信号，
 *   这段时间里 LVGL 不会复用那块像素缓冲（要等 flush_ready 才复用）。
 */
static lv_area_t   g_upd_rect;
static uint8_t    *g_upd_stage = NULL;
static int32_t     g_upd_stage_cap = 0;
static int32_t     g_upd_pitch = 0;
static _Atomic int g_upd_valid = 0;

/*
 * ★ 暂存区是**后端自己的**，拷的是像素内容，不是 LVGL 缓冲的地址。
 *
 *   第一版保存的是 `px_map` 指针，并依赖"flush 随后会阻塞等渲染完成"来保证
 *   指针有效 —— ASan 立刻抓到 SEGV in sdl2_apply_texture_update：那个假设在
 *   超时路径上不成立（flush 超时返回后 LVGL 会复用缓冲，而渲染事件仍在队列里，
 *   主线程稍后再读就是悬空访问）。教训是：**跨线程传递要传值，不要传指向别人的指针**。
 *
 *   代价是每帧一次 memcpy：PARTIAL 下通常只有几 KB，首帧整屏 384KB 也在毫秒量级。
 */
static void sdl2_stage_texture_update(int64_t disp, const lv_area_t *area, uint8_t *px_map,
                                      uint32_t stride)
{
    if (area == NULL || px_map == NULL) {
        atomic_store(&g_upd_valid, 0);
        return;
    }
    int32_t h = area->y2 - area->y1 + 1;
    if (h <= 0 || stride == 0) {
        atomic_store(&g_upd_valid, 0);
        return;
    }

    size_t need = (size_t)stride * (size_t)h;
    if ((int32_t)need > g_upd_stage_cap) {
        uint8_t *n = (uint8_t *)realloc(g_upd_stage, need);
        if (n == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                                "渲染暂存区扩容失败（内存不足）");
            atomic_store(&g_upd_valid, 0);
            return;
        }
        g_upd_stage = n;
        g_upd_stage_cap = (int32_t)need;
    }

    memcpy(g_upd_stage, px_map, need);
    g_upd_rect = *area;
    g_upd_pitch = (int32_t)stride;
    atomic_store(&g_upd_valid, 1);
}

/* 在**渲染线程**上调用：把暂存区里的区域上传进纹理 */
static void sdl2_apply_texture_update(int64_t disp)
{
    if (!atomic_load(&g_upd_valid)) {
        return;
    }
    if (g_upd_stage != NULL && g_texture != NULL) {
        SDL_Rect r;
        r.x = g_upd_rect.x1;
        r.y = g_upd_rect.y1;
        r.w = g_upd_rect.x2 - g_upd_rect.x1 + 1;
        r.h = g_upd_rect.y2 - g_upd_rect.y1 + 1;
        if (SDL_UpdateTexture(g_texture, &r, g_upd_stage, g_upd_pitch) != 0) {
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                                SDL_GetError());
        }
    }
    atomic_store(&g_upd_valid, 0);
}

static void sdl2_flush_sink(int64_t disp, const lv_area_t *area, uint8_t *px_map,
                            uint32_t stride, void *user)
{
    (void)user;
    atomic_fetch_add(&g_flush_count, 1);

    if (atomic_load(&g_paused)) {
        /* 窗口已关闭/最小化：不再渲染，但仍要放行，否则 LVGL 卡住。
         * 放在暂存之前：此时连拷贝都不必做。 */
        atomic_store(&g_upd_valid, 0);
        lvglcj_display_flush_ready(disp);
        return;
    }

    /* 拷进后端自己的暂存区，交给渲染线程上传
     * （见上方长注释：跨线程上传会被静默丢弃；也不要传指向 LVGL 缓冲的指针） */
    sdl2_stage_texture_update(disp, area, px_map, stride);

    if (sdl2_on_render_thread()) {
        /*
         * ★ 单线程（方案 C / 测试）：内联渲染。
         *   这里绝不能用「投递事件 + 等待」的路径 —— 唯一能处理该事件的
         *   线程就是当前线程，它会等到超时，每帧白等 100ms。
         *   本线程就是渲染线程，所以这里也顺带完成上传。
         */
        sdl2_apply_texture_update(disp);
        sdl2_render_now();
        lvglcj_display_flush_ready(disp);
        return;
    }

    /* 双线程（方案 A）：投递渲染请求，然后有界等待 */
    atomic_store(&g_pending, 1);
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = g_render_event;
        if (SDL_PushEvent(&ev) != 1) {
            /* 事件队列满：主线程已跟不上。记录后仍要走放行路径，不能卡住 */
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                                "SDL_PushEvent 失败（事件队列可能已满）");
        }
    }
    if (sdl2_wait_done(SDL2_WAIT_TIMEOUT_MS) != 0) {
        atomic_fetch_add(&g_wait_timeout_count, 1);
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                            "flush 等待渲染完成超时（100ms），本帧可能未呈现");
    }
    atomic_store(&g_pending, 0);

    /* ★ 无论成功还是超时都要放行 —— 超时不放行会让 LVGL 永久停在等 flush */
    lvglcj_display_flush_ready(disp);
}

static void sdl2_flush_wait_sink(int64_t disp, void *user)
{
    (void)user;

    /*
     * 窗口关闭/最小化 → 立即返回（§8.1.2 强制要求 3，也是 P0 断言 7 的证据）。
     * 这条分支必须在等待之前判断，否则「立即返回」无从谈起。
     */
    if (atomic_load(&g_paused)) {
        return;
    }
    /* 没有在途渲染 → 无事可等，立即返回（正常路径下 flush 已经等过了） */
    if (!atomic_load(&g_pending)) {
        return;
    }
    if (sdl2_wait_done(SDL2_WAIT_TIMEOUT_MS) != 0) {
        atomic_fetch_add(&g_wait_timeout_count, 1);
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                            "flush_wait 等待渲染完成超时（100ms）");
    }
}

/* ------------------------------------------------------------ 初始化 */

int32_t lvglcj_sdl2_init(int32_t w, int32_t h, int32_t color_format, int32_t buf_lines)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    if (w <= 0 || h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "窗口尺寸必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    if (g_window != NULL) {
        return LVGLCJ_OK; /* 幂等 */
    }

    /*
     * 像素格式先校验再建窗口：映射不上时**不要**留下半初始化的窗口。
     * 先建窗口再发现格式不支持，就得写一段回滚代码，而回滚路径最容易漏。
     */
    uint32_t sdl_fmt = lvglcj_sdl2_pixel_format_for(color_format);
    if (sdl_fmt == SDL_PIXELFORMAT_UNKNOWN) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "该颜色格式没有对应的 SDL 像素格式（拒绝而非猜测，避免花屏）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /* ★ 登记渲染线程：本函数必须由将要跑事件循环的线程调用 */
    atomic_store(&g_render_thread_tid, lvglcj_current_os_tid());

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        atomic_store(&g_render_thread_tid, 0);
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__, SDL_GetError());
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    g_window = SDL_CreateWindow("lvgl4cj", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                w, h, SDL_WINDOW_SHOWN);
    if (g_window == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__, SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        atomic_store(&g_render_thread_tid, 0);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == NULL) {
        /* 加速渲染器不可用时退回软件渲染 —— 这也是 dummy 驱动下的正常路径 */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (g_renderer == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__, SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        atomic_store(&g_render_thread_tid, 0);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    g_texture = SDL_CreateTexture(g_renderer, sdl_fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (g_texture == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__, SDL_GetError());
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_renderer = NULL;
        g_window = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        atomic_store(&g_render_thread_tid, 0);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    g_render_event = SDL_RegisterEvents(1);
    if (g_render_event == (uint32_t)-1) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "SDL_RegisterEvents 失败");
        SDL_DestroyTexture(g_texture);
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_texture = NULL;
        g_renderer = NULL;
        g_window = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        atomic_store(&g_render_thread_tid, 0);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    /* 复位「渲染完成」额度：不要继承上一次 init 的未决额度 */
    sdl2_reset_done();
    atomic_store(&g_paused, 0);
    atomic_store(&g_quit_requested, 0);
    atomic_store(&g_pending, 0);

    g_tex_w = w;
    g_tex_h = h;

    /*
     * 建 display 并挂 sink。
     * sink 在 C 侧拿到 area/px_map，仓颉侧完全不接触像素缓冲（ADR-002）。
     */
    int64_t d = lvglcj_display_create(w, h, color_format, LVGLCJ_BUF_PARTIAL, buf_lines);
    if (d == LVGLCJ_HANDLE_NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "display 创建失败");
        SDL_DestroyTexture(g_texture);
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_texture = NULL;
        g_renderer = NULL;
        g_window = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        atomic_store(&g_render_thread_tid, 0);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }

    if (lvglcj_display_set_flush_sink(d, sdl2_flush_sink, NULL) != LVGLCJ_OK ||
        lvglcj_display_set_flush_wait_sink(d, sdl2_flush_wait_sink, NULL) != LVGLCJ_OK) {
        lvglcj_display_delete(d);
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }
    g_display = d;

    /*
     * ★ 登记渲染线程身份到等待图检测（§3.8.4 判据 3）：
     *   渲染线程同步等待 LVGL 任务会与 flush 等待成环，
     *   因此 postAndWait 必须在**进入等待之前**就被拒绝，而不是等到死锁。
     */
    lvglcj_waitgraph_set_render_thread(atomic_load(&g_render_thread_tid));

    lvglcj_log(LVGLCJ_LOG_INFO, __func__, "SDL2 后端就绪（手写 flush/read，未用内置 SDL 驱动）");
    return LVGLCJ_OK;
}

int32_t lvglcj_sdl2_deinit(void)
{
    if (g_window == NULL) {
        return LVGLCJ_OK; /* 幂等 */
    }

    /*
     * 顺序：先放行可能的等待方 → 删 display（会注销 sink 与闭包）
     *       → 销毁纹理/渲染器/窗口 → 注销渲染线程身份。
     * 先删 display 是必要的：它内部的删除流程可能仍触发一次 flush。
     */
    sdl2_mark_paused(1);
    if (g_display != LVGLCJ_HANDLE_NULL) {
        int32_t rc = lvglcj_display_delete(g_display);
        if (rc != LVGLCJ_OK && rc != LVGLCJ_ERR_INVALID_HANDLE) {
            lvglcj_log(LVGLCJ_LOG_WARN, __func__, "display 删除返回非成功（继续清理）");
        }
        g_display = LVGLCJ_HANDLE_NULL;
    }

    if (g_texture != NULL) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer != NULL) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window != NULL) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    /*
     * 静态初始化的 mutex/condvar 不需要销毁（它们没有内核资源），
     * 只把额度清零，避免下一次 init 继承本次遗留的未决额度。
     */
    sdl2_reset_done();
    lvglcj_waitgraph_set_render_thread(0);
    atomic_store(&g_render_thread_tid, 0);
    g_tex_w = 0;
    g_tex_h = 0;

    /*
     * ★ 复位会话级计数器。
     *
     * 它们是**每次会话**的统计量（flushCount/presentCount/waitTimeoutCount），
     * 语义是「本次 start 之后发生了多少次」。若不复位，同一进程里第二次
     * init/close 周期的调用方会看到上一次的累计值 —— 「我刚开始，怎么已经刷了 12 帧」。
     *
     * 实测踩到：t8 新增的性能基准在同一进程里先跑了一遍 SDL2（init → 采样 → close），
     * 随后的 sdl2_test 断言 `flushCount() == 0` 失败。
     * 那不是测试写错了，而是 deinit 漏了这一步 —— 计数器的生命周期本应与窗口绑定。
     */
    atomic_store(&g_flush_count, 0);
    atomic_store(&g_present_count, 0);
    atomic_store(&g_wait_timeout_count, 0);

    /* 渲染暂存区归后端所有，随会话释放 —— 不清掉就是每次 init/deinit 漏一块 */
    free(g_upd_stage);
    g_upd_stage = NULL;
    g_upd_stage_cap = 0;
    g_upd_pitch = 0;
    atomic_store(&g_upd_valid, 0);

    return LVGLCJ_OK;
}

int64_t lvglcj_sdl2_display(void)
{
    return g_display;
}

/* ------------------------------------------------------------ 事件循环 */

int32_t lvglcj_sdl2_poll_events(void)
{
    if (g_window == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, __func__,
                            "SDL2 后端未初始化");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (g_render_event != 0 && ev.type == g_render_event) {
            if (atomic_load(&g_render_suppressed)) {
                /* 测试注入：故意不响应，让等待方走超时分支 */
                continue;
            }
            /* ★ 顺序要紧：先上传（在主线程上做，见 sdl2_apply_texture_update 的说明），
             *   再渲染、再唤醒等待方 —— 唤醒意味着"本帧已经用上了这块像素"，  */
            sdl2_apply_texture_update(LVGLCJ_HANDLE_NULL);
            sdl2_render_now();
            atomic_store(&g_pending, 0);
            sdl2_signal_done();
        } else if (ev.type == SDL_QUIT) {
            sdl2_mark_paused(1);
        } else if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                sdl2_mark_paused(1);
            } else if (ev.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                /* 最小化只暂停渲染，不请求退出 */
                sdl2_mark_paused(0);
            } else if (ev.window.event == SDL_WINDOWEVENT_RESTORED) {
                atomic_store(&g_paused, 0);
            }
        } else if (ev.type == SDL_MOUSEMOTION) {
            atomic_store(&g_in_x, ev.motion.x);
            atomic_store(&g_in_y, ev.motion.y);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            atomic_store(&g_in_x, ev.button.x);
            atomic_store(&g_in_y, ev.button.y);
            atomic_store(&g_in_pressed, 1);
        } else if (ev.type == SDL_MOUSEBUTTONUP) {
            atomic_store(&g_in_x, ev.button.x);
            atomic_store(&g_in_y, ev.button.y);
            atomic_store(&g_in_pressed, 0);
        } else if (ev.type == SDL_KEYDOWN) {
            atomic_store(&g_in_key, (int32_t)ev.key.keysym.sym);
            atomic_store(&g_in_key_pressed, 1);
        } else if (ev.type == SDL_KEYUP) {
            atomic_store(&g_in_key, (int32_t)ev.key.keysym.sym);
            atomic_store(&g_in_key_pressed, 0);
        }
    }

    return atomic_load(&g_quit_requested) ? 1 : 0;
}

int32_t lvglcj_sdl2_feed_indev(void)
{
    return lvglcj_indev_set_point(atomic_load(&g_in_x), atomic_load(&g_in_y),
                                 atomic_load(&g_in_pressed));
}

/* ------------------------------------------------------------ 观测 */

int32_t lvglcj_sdl2_flush_count(void)
{
    return atomic_load(&g_flush_count);
}

int32_t lvglcj_sdl2_present_count(void)
{
    return atomic_load(&g_present_count);
}

int32_t lvglcj_sdl2_wait_timeout_count(void)
{
    return atomic_load(&g_wait_timeout_count);
}

int32_t lvglcj_sdl2_is_paused(void)
{
    return atomic_load(&g_paused);
}

int32_t lvglcj_sdl2_is_render_thread(void)
{
    return sdl2_on_render_thread();
}

int32_t lvglcj_sdl2_set_render_suppressed(int32_t on)
{
    atomic_store(&g_render_suppressed, on ? 1 : 0);
    return LVGLCJ_OK;
}

int32_t lvglcj_sdl2_inject_quit(void)
{
    sdl2_mark_paused(1);
    return LVGLCJ_OK;
}
