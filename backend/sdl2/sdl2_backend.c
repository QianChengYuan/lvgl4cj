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

/*
 * 截图：维护一份**整帧 CPU 副本**，让截图不依赖任何线程身份。
 *
 * ★ 为什么不用"请求渲染线程去读纹理"（我第一版就是这么写的，实测行不通）：
 *   实测诊断输出「注册的渲染线程 tid=113572，当前 tid=113584（不同线程）」——
 *   仓颉是 M:N 线程模型，**主线程会跨 OS 线程迁移**（示例主循环里每 2ms 一次 sleep），
 *   所以"主循环所在的线程 == 初始化时的线程"这个前提不成立；
 *   于是 g_render_thread_tid 的比对失效，截图走了"请求 + 有界等待"，
 *   而能处理该事件的恰恰是**正在等待的那个线程** → 必然等到超时
 *   （表现为"报失败，但文件随后被下一轮 poll 写了出来"）。
 * ★ 现在的做法：在应用条带时（sdl2_apply_texture_update，本来就在 poll 线程里做）
 *   把像素**顺带**镜像进整帧缓冲，截图时只做纯 CPU 的格式转换与写文件 ——
 *   不碰 SDL 渲染器，因此在任意线程上调用都安全。
 *   代价是每帧多一次 memcpy（800x480x2 ≈ 768KB/帧，可忽略）。
 */
static uint8_t      *g_frame = NULL;      /* 整帧像素，布局与纹理一致（紧凑，pitch = w*bpp） */
static int32_t       g_frame_w = 0;
static int32_t       g_frame_h = 0;
static int32_t       g_frame_pitch = 0;
static uint32_t      g_frame_fmt = 0;     /* 纹理的 SDL 像素格式，转换时要用 */
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* 帧缓冲是否已经至少被画过一次（没画过时截图应报"还不支持"而不是给一张黑图） */
static _Atomic int   g_frame_painted = 0;

static int sdl2_write_screenshot(const char *path);
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

static _Atomic uint32_t g_applied_seq = 0;

/*
 * 已被**消费**（上传进纹理）的最大序号。0 = 还没有任何一条被消费。
 *
 * ★★ 为什么是序号而不是"有一次唤醒"的标志（这是实测逼出来的改法）：
 *
 *   生产者（LVGL 线程）把一条渲染条带拷进**唯一一块**暂存区，然后必须等到
 *   「这块暂存区已经被消费」才能返回 —— 否则 LVGL 会接着渲染下一条带并
 *   **覆盖**它，那条带的像素就再也没人消费了。
 *
 *   所以等待的谓词只能是"**我自己那个序号已被消费**"。用单标志（合并语义）
 *   有一个致命性质：任何一次**替别人**的唤醒都会让后来的等待被立刻满足，
 *   于是生产者在"数据其实还没被消费"的状态下返回、随即覆盖暂存区。
 *   而唤醒来源不止"渲染完成"——sdl2_mark_paused()（窗口最小化/关闭）
 *   也会唤醒；它在**无人等待**时执行，就会把额度留在标志里。
 *
 *   实测证据（Raspberry Pi Zero 2 W + KMSDRM，1920×1080）：
 *     · 画面上出现**纯黑带**，高度恰是渲染条带高度（108 行）的**整数倍**，
 *       位置每次随机（三次跑：40% / 50% / 50%）；
 *     · 同一轮 waitTimeout 4~8、staleApply 12~13（两者一起出现是特征）；
 *     · 换成 dummy 驱动（Present 几乎免费）后两者都是 0，黑带消失。
 *   根因是放行原本发生在 **Present 之后**：KMSDRM 的 Present 是 GL + KMS
 *   page flip，在 Zero 2W 上会超过 100ms 的等待上限 —— flush 超时返回、
 *   LVGL 继续跑、下一条带覆盖暂存区，而队列里那个事件的像素再没人消费。
 *
 *   两处一起改才成立：
 *     1. 谓词换成序号（本变量 + sdl2_wait_done 的重判）——
 *        "被别人唤醒"不再可能满足我，因为我的序号还没到；
 *     2. 放行点跟到**上传之后、Present 之前** —— "等待返回"的含义是
 *        "暂存区可以复用了"，与 Present 无关：Present 慢不该拖累生产者。
 *   于是 sdl2_reset_done 那种"清残留额度"的补丁不再需要（见它在 flush 里
 *   的移除），因为已经不存在的"残留额度"本就无法满足任何具体序号。
 */

/* 复位（init/deinit 用）。静态初始化的 mutex/condvar 本身无需销毁。 */
static void sdl2_reset_done(void)
{
    pthread_mutex_lock(&g_done_lock);
    atomic_store(&g_applied_seq, 0);
    pthread_mutex_unlock(&g_done_lock);
}

/*
 * 唤醒等待方。**它只是唤醒**：能不能结束等待，由 sdl2_wait_done 用序号重新判断 ——
 * 所以"替别人做的这次唤醒"不会误放行任何等待方（这正是旧标志写法的病根）。
 *
 * ★ 窗口关闭/最小化路径上的这一句是**必需**的（§8.1.2 强制要求 3）：
 *   没有它，正在 flush 里等待的 LVGL 线程会一直等到超时。
 */
static void sdl2_signal_done(void)
{
    pthread_mutex_lock(&g_done_lock);
    pthread_cond_broadcast(&g_done_cond);
    pthread_mutex_unlock(&g_done_lock);
}

/*
 * 在**渲染线程**上调用：声明"某个序号的数据已经被消费"，并唤醒等待方。
 * 这是放行的唯一入口 —— 它只该在 sdl2_apply_texture_update 之后调用。
 */
static void sdl2_mark_applied(uint32_t seq)
{
    pthread_mutex_lock(&g_done_lock);
    atomic_store(&g_applied_seq, seq);
    pthread_cond_broadcast(&g_done_cond);
    pthread_mutex_unlock(&g_done_lock);
}

static _Atomic int g_paused = 0;          /* 窗口关闭或最小化 */
static _Atomic int g_quit_requested = 0;  /* 请求退出主循环 */
static _Atomic int g_pending = 0;         /* 有渲染请求在途 */
static _Atomic int g_render_suppressed = 0; /* 测试注入：忽略渲染请求 */

/*
 * 测试注入：让 Present 人为变慢（毫秒）。
 *
 * ★ 它存在的理由很具体：KMSDRM 上"纯黑带"的成因是 **Present 慢于等待上限**，
 *   而 Present 快慢取决于驱动/硬件（dummy 上几乎免费，GL + page flip 上可能
 *   超过 100ms）。没有这个注入就没法在 CI 里复现那条路径 —— 也就没法证明
 *   "放行只等上传、不等 Present"这个修改真的解决了它（见 tests 里的用例）。
 */
static _Atomic int32_t g_present_delay_ms = 0;

static _Atomic int g_paused_drop = 0;     /* 诊断用：暂停路径丢掉的帧数 */
static _Atomic int32_t g_flush_count = 0;
static _Atomic int32_t g_present_count = 0;
static _Atomic int32_t g_wait_timeout_count = 0;

/* 输入状态：主线程写，LVGL 线程读（全部原子，无需锁） */
static _Atomic int32_t g_in_x = 0;
static _Atomic int32_t g_in_y = 0;
static _Atomic int32_t g_in_pressed = 0;
static _Atomic int32_t g_in_key = 0;
static _Atomic int32_t g_in_key_pressed = 0;

/*
 * 滚轮累计格数（SDL 约定：正值 = 滚轮向上/远离用户）。
 *
 * 为什么是"累计 + 取走"而不是"直接喂给 indev"：
 *   LVGL 的 encoder 型 indev 在**导航模式**下把 enc_diff 送去 lv_group_focus_prev/next
 *   （移动焦点），并不会滚动视图（见 lv_indev.c）。所以滚轮不能走 indev，
 *   只能由调用方取走后调用 lv_obj_scroll_by。这里负责把两次读之间的多格累积起来，
 *   否则快速滚动会丢格。
 */
static _Atomic int32_t g_in_wheel_steps = 0;

/* 最近一次按下的键（SDL keysym）；被 take_key 取走后清零。见后端头里的说明。 */
static _Atomic uint32_t g_in_key_taken = 0;

/* ------------------------------------------------------------ 工具 */

/*
 * 有界等待：等到**指定序号**被消费为止。
 * 返回 0 = 该序号已消费（暂存区可以复用），-1 = 超时或窗口已暂停。
 */
static int sdl2_wait_done(uint32_t seq, int timeout_ms)
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
     * 用 while 而不是 if，两个理由：
     *   · condvar 允许**虚假唤醒**；
     *   · 唤醒**可能是替别人做的**（暂停路径、或另一个序号被消费）。
     * 所以条件必须是"我自己那个序号到了没有"。若写成 if、或去检查一个
     * 合并语义的标志，就会在别人的唤醒下误判为"已消费" —— flush 提前返回
     * 并覆盖还没被消费的暂存数据，那条带的像素从此丢失。
     * 这正是 KMSDRM 上"纯黑带"的成因（见 g_applied_seq 上方的实测说明）。
     */
    while (atomic_load(&g_applied_seq) < seq) {
        if (pthread_cond_timedwait(&g_done_cond, &g_done_lock, &deadline) != 0) {
            break; /* 超时或出错：与旧实现 sem_timedwait 非 0 时同样返回 -1 */
        }
        if (atomic_load(&g_paused)) {
            break; /* 窗口已关闭/最小化：立即返回（§8.1.2 强制要求 3） */
        }
    }
    if (atomic_load(&g_applied_seq) >= seq) {
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
/*
 * ===================== 暂存区：**多槽轮转**，而不是一块共享缓冲 =====================
 *
 * ★★ 这是"kmsdrm 上出现纯黑带"的真正解法，来龙去脉写清楚：
 *
 *   生产者（LVGL 线程）每写一条带，都要把它拷进一块**后端自己的**缓冲，再让渲染
 *   线程贴上纹理。若等待超时返回（KMSDRM 上 Present 是 GL + KMS page flip，
 *   Zero 2W 上可能超过 100ms 的等待上限），LVGL 会立刻渲染**下一条带**；
 *   而只有一块暂存区时它就把上一块**覆盖**掉了 —— 队列里那个事件的像素再没人消费，
 *   画面上那条带永远没被写过：**纯黑**（旧纹理上则是残留旧内容 = 重影）。
 *
 *   实测（1920×1080 连跑三次）：黑带高度恰是渲染条带高度的整数倍、位置每次随机；
 *   同一轮 waitTimeout 4~15、staleApply 10~14；换 dummy（Present 免费）两者全为 0、
 *   黑带消失。也就是说：**超时 ≠ 丢数据**这个等式必须被打断。
 *
 *   修法就是给若干槽轮转：生产者取下一个空槽。只要当时还有槽可用，超时就只意味着
 *   "消费者晚了"，而不意味着数据没了。槽数取 3 —— 单缓冲的 LVGL 同时最多只有一个
 *   flush 在途，再加"超时后队列里可能还留着一个"，3 个槽留有余量。
 *
 *   两个计数必须分开看，它们的含义完全不同：
 *     · staleApply —— 事件序号 ≠ 最新 staged 序号。只说明"生产者跑在消费者前面"，
 *                     在慢消费者上出现是**正常**的，不表示会画错（事件自带槽号与矩形）。
 *     · slotClobber —— 取到的槽**还没被应用**就又被写。这才是**真正丢数据**，
 *                     长期必须为 0。修复前只有一块暂存区，所以它等于每轮都发生。
 */
#define SDL2_STAGE_SLOTS 3

typedef struct {
    uint8_t   *buf;   /* 本槽的像素拷贝（按区域宽度紧凑排列，见 display.c 的行距说明） */
    int32_t    cap;
    int32_t    pitch;
    lv_area_t  rect;
    _Atomic int valid; /* 1 = 有数据待应用；应用之后清零 */
} sdl2_stage_slot_t;

static sdl2_stage_slot_t g_slots[SDL2_STAGE_SLOTS];
static _Atomic int      g_slot_next = 0;    /* 生产者轮转取槽 */
static _Atomic int32_t  g_slot_clobber = 0; /* 真正丢数据的次数（长期必须为 0） */

static _Atomic uint32_t g_upd_seq = 0;      /* 最近一次 stage 的序号 */
static _Atomic uint32_t g_stage_gen = 0;    /* 每次 stage 递增，作为序号来源 */
static _Atomic int32_t  g_stale_apply = 0;  /* 事件序号与最新序号不一致的次数 */

/*
 * ★ 暂存的是像素**内容**，不是 LVGL 缓冲的地址。
 *
 *   第一版保存的是 `px_map` 指针，并依赖"flush 随后会阻塞等渲染完成"来保证
 *   指针有效 —— ASan 立刻抓到 SEGV in sdl2_apply_texture_update：那个假设在
 *   超时路径上不成立（flush 超时返回后 LVGL 会复用缓冲，而渲染事件仍在队列里，
 *   主线程稍后再读就是悬空访问）。教训是：**跨线程传递要传值，不要传指向别人的指针**。
 *
 *   代价是每条带一次 memcpy：PARTIAL 下通常只有几 KB，首帧整屏 384KB 也在毫秒量级。
 *
 * 返回取到的槽号（供事件携带）；参数不合法时返回 -1（表示这条带没有人消费，
 * 调用方直接放行即可）。
 */
static int sdl2_stage_texture_update(int64_t disp, const lv_area_t *area, uint8_t *px_map,
                                     uint32_t stride)
{
    if (area == NULL || px_map == NULL || stride == 0) {
        return -1;
    }
    int32_t h = area->y2 - area->y1 + 1;
    if (h <= 0) {
        return -1;
    }
    size_t need = (size_t)stride * (size_t)h;

    uint32_t seq = atomic_fetch_add(&g_stage_gen, 1) + 1;
    atomic_store(&g_upd_seq, seq);

    int idx = (int)((uint32_t)atomic_fetch_add(&g_slot_next, 1) % SDL2_STAGE_SLOTS);
    sdl2_stage_slot_t *s = &g_slots[idx];

    if (atomic_load(&s->valid)) {
        /*
         * 取到的槽**还没被应用**就又要被写 —— 这才是真正的丢数据。
         * 修复前只有一块暂存区，所以这条分支等于每轮都走：
         * 表现就是画面上出现条带高度整数倍的纯黑带。
         */
        atomic_fetch_add(&g_slot_clobber, 1);
    }

    if ((int32_t)need > s->cap) {
        uint8_t *n = (uint8_t *)realloc(s->buf, need);
        if (n == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                                "渲染暂存槽扩容失败（内存不足）");
            atomic_store(&s->valid, 0);
            return -1;
        }
        s->buf = n;
        s->cap = (int32_t)need;
    }

    memcpy(s->buf, px_map, need);
    s->rect = *area;
    s->pitch = (int32_t)stride;
    atomic_store(&s->valid, 1);
    return idx;
}

/* 把**所有**槽标记为无效（暂停/反初始化用：这些数据不会再有人应用） */
static void sdl2_invalidate_all_slots(void)
{
    for (int i = 0; i < SDL2_STAGE_SLOTS; ++i) {
        atomic_store(&g_slots[i].valid, 0);
    }
}

/* 在**渲染线程**上调用：把指定槽的数据上传进纹理。返回 1 = 真的上传了 */
static int sdl2_apply_texture_update(int64_t disp, int idx)
{
    if (idx < 0 || idx >= SDL2_STAGE_SLOTS) {
        return 0;
    }
    sdl2_stage_slot_t *s = &g_slots[idx];
    if (!atomic_load(&s->valid)) {
        return 0;
    }
    if (s->buf == NULL || g_texture == NULL) {
        /*
         * ★ 贴不上去时**不能**把"待上传"清掉。
         *
         *   早先这里是无条件清掉的（而且连错误都不记）：于是这一帧永远丢失，
         *   而 LVGL 早已被放行、不会重画那条条带 —— 结果是**永久黑带**。
         *   现在保留标记，等纹理就绪后下一次再由渲染侧贴上去。
         */
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                            "暂存槽或纹理尚未就绪：本帧保留待上传（不丢弃）");
        return 0;
    }

    SDL_Rect r;
    r.x = s->rect.x1;
    r.y = s->rect.y1;
    r.w = s->rect.x2 - s->rect.x1 + 1;
    r.h = s->rect.y2 - s->rect.y1 + 1;
    if (SDL_UpdateTexture(g_texture, &r, s->buf, s->pitch) != 0) {
        /* 同理：失败也保留，让下一次再试，而不是静默丢帧 */
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__, SDL_GetError());
        return 0;
    }

    /*
     * ★ 顺带把这一条带镜像进整帧副本（截图用）。
     *   暂存槽里是**按区域宽度紧凑排列**的（见 display.c 的行距说明），
     *   而整帧是按屏宽排列的，所以这里必须逐行搬，不能整块 memcpy。
     */
    if (g_frame != NULL && g_frame_w > 0 && g_frame_h > 0) {
        int32_t bpp = (int32_t)SDL_BYTESPERPIXEL((SDL_PixelFormatEnum)g_frame_fmt);
        if (r.x >= 0 && r.y >= 0 && r.x + r.w <= g_frame_w && r.y + r.h <= g_frame_h) {
            pthread_mutex_lock(&g_frame_lock);
            for (int32_t i = 0; i < r.h; i++) {
                memcpy(g_frame + (size_t)(r.y + i) * (size_t)g_frame_pitch +
                           (size_t)r.x * (size_t)bpp,
                       s->buf + (size_t)i * (size_t)s->pitch,
                       (size_t)r.w * (size_t)bpp);
            }
            atomic_store(&g_frame_painted, 1);
            pthread_mutex_unlock(&g_frame_lock);
        }
    }

    atomic_store(&s->valid, 0); /* 本槽已消费：生产者可以复用它了 */
    return 1;
}

static void sdl2_flush_sink(int64_t disp, const lv_area_t *area, uint8_t *px_map,
                            uint32_t stride, void *user)
{
    (void)user;
    atomic_fetch_add(&g_flush_count, 1);

    /*
     * ★ 一次性诊断（只打前若干次）：把每次 flush 的**区域**与**像素是否真的是黑的**打出来。
     *
     *   这三种答案对应完全不同的原因，靠推理分不开，只能量：
     *     · 某条带压根没出现在这里 → LVGL 没判它需要重画（invalidate 的问题）；
     *     · 出现了、但像素全黑 → LVGL 渲染时那块本身就是空的（布局/裁剪的问题）；
     *     · 像素是彩色的、屏幕上却是黑的 → 是我们贴错了位置（后端的问题）。
     *   起因：全控件示例里出现了**整条条带纯黑**（48 行渲染条带的整数倍），
     *   而且每次运行黑的条带不同。已排除的两条路（都由实测否掉）：
     *   paused 分支从未走到；flush/present/waitTimeout/staleApply 计数都很干净。
     */
    static int diag_flush = -1;
    if (diag_flush < 0) {
        const char *e = getenv("LVGLCJ_DIAG_FLUSH");
        diag_flush = (e != NULL && e[0] != '0') ? 1 : 0;
    }
    if (diag_flush && area != NULL && px_map != NULL && atomic_load(&g_flush_count) <= 24) {
        int32_t aw = area->x2 - area->x1 + 1;
        int32_t ah = area->y2 - area->y1 + 1;
        long zero = 0;
        long tot = 0;
        for (int32_t r = 0; r < ah; r++) {
            for (int32_t c = 0; c < aw; c++) {
                const uint8_t *p = px_map + (size_t)r * stride + (size_t)c * 2u;
                if (p[0] == 0 && p[1] == 0) {
                    zero++;
                }
                tot++;
            }
        }
        fprintf(stderr, "[sdl2] flush#%d area y=%d..%d x=%d..%d 黑像素 %ld/%ld (%.0f%%)\n",
                (int)atomic_load(&g_flush_count), (int)area->y1, (int)area->y2,
                (int)area->x1, (int)area->x2, zero, tot,
                tot > 0 ? 100.0 * (double)zero / (double)tot : 0.0);
    }

    if (atomic_load(&g_paused)) {
        /* 窗口已关闭/最小化：不再渲染，但仍要放行，否则 LVGL 卡住。
         * 放在暂存之前：此时连拷贝都不必做 —— 顺手把已有槽标记为无效（不会有人再应用）。 */
        sdl2_invalidate_all_slots();
        /* 诊断：这条路径会**丢弃**这一帧，而 LVGL 以为它已显示（见下方 flush_ready）。
         * 只报第一次：这条路径本就不该频繁出现，打太多会把日志淹掉。 */
        if ((int)atomic_fetch_add(&g_paused_drop, 1) + 1 == 1) {
            fprintf(stderr, "[sdl2] !! paused 路径丢弃了一帧：y=%d..%d（后续同路径不再重复打印）\n",
                    (int)area->y1, (int)area->y2);
        }
        lvglcj_display_flush_ready(disp);
        return;
    }

    /* 拷进后端自己的暂存槽，交给渲染线程上传
     * （见上方长注释：跨线程上传会被静默丢弃；也不要传指向 LVGL 缓冲的指针） */
    int slot = sdl2_stage_texture_update(disp, area, px_map, stride);
    /*
     * ★ 序号由 stage 生成，这里**取回来用**，不能自己再自增一次 ——
     *   自增就会出现"等待的序号"与"暂存的序号"不一致，结果是每次 flush 都等满超时
     *   （因为等的是一个永远不会被消费的序号）。这个坑只有把生成点收敛到一处才能避免。
     */
    uint32_t seq = atomic_load(&g_upd_seq);

    if (sdl2_on_render_thread()) {
        /*
         * ★ 单线程（方案 C / 测试）：内联渲染。
         *   这里绝不能用「投递事件 + 等待」的路径 —— 唯一能处理该事件的
         *   线程就是当前线程，它会等到超时，每帧白等 100ms。
         *   本线程就是渲染线程，所以这里也顺带完成上传。
         */
        (void)sdl2_apply_texture_update(disp, slot);
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
        ev.user.code = (int32_t)seq;            /* 序号：用于等待与失配统计 */
        ev.user.data2 = (void *)(intptr_t)slot; /* 槽号：消费方据此取数据 */

        /*
         * ★ 这里原本有一句 sdl2_reset_done()，用来清掉"可能残留的额度"，现已移除。
         *
         *   它当年的理由是：额度是单标志，暂停路径与迟到的渲染都可能留下无人认领的
         *   额度，而后来的等待会被它**立刻**满足 —— 于是 flush 提前返回、回报
         *   flush_ready，LVGL 继续下一帧，覆盖掉还没被消费的条带。
         *   那个理由本身是对的（实测也确认暂停路径会留下额度），但它只治症状：
         *   只要等待的含义仍是"有过一次唤醒"，就永远存在被别人唤醒满足的可能。
         *
         *   现在等待按**序号**判定（见 g_applied_seq 的实测说明），别人的唤醒满足不了
         *   任何具体序号 —— "残留额度"这个概念随之消失，所以补丁可以撤掉。
         */
        if (SDL_PushEvent(&ev) != 1) {
            /* 事件队列满：主线程已跟不上。记录后仍要走放行路径，不能卡住 */
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                                "SDL_PushEvent 失败（事件队列可能已满）");
        }
    }
    /*
     * 等的是**本次的序号**被消费（上传进纹理），而不是"画面已呈现"。
     * 这个区别是这次修改的要害：KMSDRM 上 Present 是 GL + page flip，可能超过
     * 100ms 的等待上限，而"暂存区能不能复用"只取决于上传 —— 让 Present 拖累
     * 生产者，就会导致超时返回 + 覆盖未消费数据 = 画面上那条带变纯黑。
     *
     * ★ 超时仍要放行（否则 LVGL 永久停在等 flush），但此时那条带的像素确实没进
     *   纹理 —— waitTimeoutCount / staleApplyCount 就是这条路径的判据，长跑里
     *   应当恒为 0。
     */
    if (sdl2_wait_done(seq, SDL2_WAIT_TIMEOUT_MS) != 0) {
        atomic_fetch_add(&g_wait_timeout_count, 1);
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, disp, 0, __func__,
                            "flush 等待本帧被消费超时（100ms），本帧可能未呈现");
    }

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
    /*
     * 等的是"最新提交的那个序号"被消费。这里取 g_stage_gen（最新序号）而非某个具体
     * 请求的序号：调用方（flush_wait）的语义是"把在途的渲染做完再继续"。
     * 错误文案保持原样（可能有测试按文案匹配）。
     */
    if (sdl2_wait_done(atomic_load(&g_stage_gen), SDL2_WAIT_TIMEOUT_MS) != 0) {
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

    /*
     * ★★ 默认用**软件**渲染器，而不是加速渲染器。
     *
     *   这是实测结论，不是保守选择：在 WSLg 上走加速（GL）渲染器时，对纹理做
     *   **子矩形**更新会把内容写错 —— 画面上出现**整条的纯黑带**
     *   （48 行渲染条带的整数倍，每次运行位置还不同）以及同一块内容错位重复。
     *
     *   判定链条（每一步都是量出来的，不是推的）：
     *     · 首帧 10 条条带全部冲刷，每条全宽、像素 **0% 黑**（C 侧探针实测）；
     *     · 相隔 1 秒连抓两张，黑带位置完全一致 → 抓图可信，不是抖动；
     *     · 同一份二进制加 SDL_RENDER_DRIVER=software，黑带**完全消失**，
     *       卡片带恢复成 5 段正常高度（56/96/96/96/88）。
     *   ⇒ 问题落在渲染后端，不在 LVGL、不在丢帧、不在抓图。
     *
     *   代价很小：LVGL 本来就是软件绘制、纹理也在 CPU 侧，末端这次合成拷贝
     *   在 800x480x30fps 下只是几十 MB/s 的内存带宽。
     *   要回到加速渲染：设 SDL_RENDER_DRIVER=opengl（SDL 会优先该驱动）。
     */
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (g_renderer == NULL) {
        /* 个别平台/驱动下 software 不可用，再退回加速 —— 也是 dummy 驱动下的路径 */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
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
    /* 为截图准备整帧副本（尺寸取渲染输出，格式与纹理一致） */
    {
        int fw = 0;
        int fh = 0;
        if (SDL_GetRendererOutputSize(g_renderer, &fw, &fh) == 0 && fw > 0 && fh > 0) {
            g_frame_w = fw;
            g_frame_h = fh;
            g_frame_fmt = (uint32_t)sdl_fmt;
            g_frame_pitch = fw * (int32_t)SDL_BYTESPERPIXEL(sdl_fmt);
            free(g_frame);
            g_frame = (uint8_t *)calloc(1, (size_t)g_frame_pitch * (size_t)fh);
            atomic_store(&g_frame_painted, 0);
        }
    }
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

    /* 复位等待协议状态：不要继承上一次 init 的"已消费序号" */
    sdl2_reset_done();
    atomic_store(&g_paused, 0);
    atomic_store(&g_quit_requested, 0);
    atomic_store(&g_pending, 0);
    atomic_store(&g_present_delay_ms, 0); /* 注入钩子不跨会话残留 */

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
    atomic_store(&g_stale_apply, 0);
    atomic_store(&g_upd_seq, 0);
    atomic_store(&g_stage_gen, 0);
    atomic_store(&g_slot_clobber, 0);
    atomic_store(&g_slot_next, 0);

    /* 渲染暂存槽归后端所有，随会话释放 —— 不清掉就是每次 init/deinit 漏一块 */
    for (int i = 0; i < SDL2_STAGE_SLOTS; ++i) {
        free(g_slots[i].buf);
        g_slots[i].buf = NULL;
        g_slots[i].cap = 0;
        g_slots[i].pitch = 0;
        atomic_store(&g_slots[i].valid, 0);
    }
    /* 截图用的整帧副本同理 */
    free(g_frame);
    g_frame = NULL;
    g_frame_w = 0;
    g_frame_h = 0;
    atomic_store(&g_frame_painted, 0);

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
            /*
             * ★★ 顺序是这个协议的核心，而这次是**实测逼出来的**：
             *      上传 → **声明该序号已被消费（放行）** → 再渲染/呈现。
             *
             *   为什么放行必须紧跟上传、而不是等 Present 完成：
             *   对生产者而言，"等待返回"的含义是「我那块暂存数据已经被消费，
             *   可以写下一块了」。所以放行的充分条件就是**上传完成** ——
             *   Present 属于另一件事，它与"暂存区能否复用"无关。
             *
             *   KMSDRM 上 Present 是 GL + KMS page flip，在 Zero 2W 上会超过 100ms
             *   的等待上限：原先放行在 Present 之后，于是每轮都有 4~8 次超时、
             *   12~13 次 staleApply，画面上出现**条带高度整数倍的纯黑带**，
             *   位置每次随机（超时发生在哪条带上是随机的）。
             *   改到这里之后同一场景重测：waitTimeout 0、staleApply 0、黑带消失。
             *
             * ★ 事件带的是投递时暂存数据的序号。若当前序号更大，说明这期间又 stage 过
             *   一次 —— 也就是**上一次等待提前返回过**。sdl2_apply_texture_update 永远
             *   按暂存区**自带的矩形**应用，所以"新像素贴到旧矩形上"这种画错在结构上
             *   不可能发生；但那条带原来的像素确实丢了，所以这个计数值得长期盯。
             */
            if (ev.user.code != (int32_t)atomic_load(&g_upd_seq)) {
                atomic_fetch_add(&g_stale_apply, 1);
            }

            sdl2_apply_texture_update(LVGLCJ_HANDLE_NULL, (int)(intptr_t)ev.user.data2);

            /* 数据已被消费 → 立刻放行（生产者的等待只等这件事，见上方说明） */
            atomic_store(&g_pending, 0);
            sdl2_mark_applied((uint32_t)ev.user.code);

            /* 测试注入：让 Present 变慢，用来复现"消费慢于等待上限"那条路径 */
            int32_t delay_ms = atomic_load(&g_present_delay_ms);
            if (delay_ms > 0) {
                SDL_Delay((Uint32)delay_ms);
            }

            sdl2_render_now();
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
            /*
             * ★ 坐标钳到窗口内。实测在 WSLg 上会收到**窗口外的坐标**
             *   （LVGL 侧日志里成百条 "X is 2479 > 800"），那会让画面自己滚动 ——
             *   表现是"界面在抖"。坐标本来就该在窗口内，越界值只可能是
             *   平台/缩放造成的，钳制比照单全收安全。
             */
            int mx = ev.motion.x;
            int my = ev.motion.y;
            int ww = 0;
            int wh = 0;
            SDL_GetWindowSize(g_window, &ww, &wh);
            if (ww > 0 && wh > 0) {
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= ww) mx = ww - 1;
                if (my >= wh) my = wh - 1;
            }
            atomic_store(&g_in_x, mx);
            atomic_store(&g_in_y, my);
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
            /* 另存一份给应用侧的"全局快捷键"取用（含重复触发：按住不放会连发，
             * 这与 LvIndev 那条路无关，后者仍是按下=1/抬起=0 的状态量） */
            atomic_store(&g_in_key_taken, (uint32_t)ev.key.keysym.sym);
        } else if (ev.type == SDL_KEYUP) {
            atomic_store(&g_in_key, (int32_t)ev.key.keysym.sym);
            atomic_store(&g_in_key_pressed, 0);
        } else if (ev.type == SDL_MOUSEWHEEL) {
            /*
             * 滚轮：只做**累积**，不做解释。
             *   · SDL 约定 y > 0 = 滚轮向上（远离用户）；
             *   · 某些平台/触控板的"自然滚动"会置 FLIPPED，此时要取反，
             *     否则同一台机器上滚轮方向会和系统设置相反；
             *   · 累积而不是覆盖：两次读之间来了两格就记两格，不然快速滚会丢。
             * 由调用方（示例的读回调）取走后调用 lv_obj_scroll_by —— 原因见
             * g_in_wheel_steps 上方的说明（LVGL 的滚轮通路只切焦点）。
             */
            int32_t dy = ev.wheel.y;
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                dy = -dy;
            }
            if (dy != 0) {
                atomic_fetch_add(&g_in_wheel_steps, dy);
            }
        }
    }

    return atomic_load(&g_quit_requested) ? 1 : 0;
}

/*
 * 取走累计的滚轮格数（取走后清零）。
 * 只在 LVGL 线程里调（示例在 indev 读回调里调）——"取走"是读-改-写，
 * 由单线程调用才不会被吞格。
 */
/*
 * 把当前纹理写成 P6 PPM（供 lvglcj_sdl2_screenshot 调用，只在渲染线程上执行）。
 * 返回 0 成功、-1 失败。
 */
static int sdl2_write_screenshot(const char *path)
{
    if (path == NULL || path[0] == '\0' || g_frame == NULL || g_frame_w <= 0 || g_frame_h <= 0) {
        return -1;
    }

    int32_t w = g_frame_w;
    int32_t h = g_frame_h;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
    if (rgb == NULL) {
        return -1;
    }

    /*
     * 纯 CPU 的格式转换：SDL_ConvertPixels 覆盖各种像素格式，
     * 比自己写 RGB565→RGB888 的位运算更不容易出错（也让"格式变了"不会静默出错图）。
     */
    pthread_mutex_lock(&g_frame_lock);
    int conv = SDL_ConvertPixels(w, h, (SDL_PixelFormatEnum)g_frame_fmt, g_frame,
                                 g_frame_pitch, SDL_PIXELFORMAT_RGB24, rgb, w * 3);
    if (conv == 0) {
        /* 复制一份再解锁：写文件可能较慢，不该占着锁 */
        pthread_mutex_unlock(&g_frame_lock);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            free(rgb);
            return -1;
        }
        fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
        size_t n = fwrite(rgb, 1, (size_t)w * (size_t)h * 3u, f);
        int ok = (n == (size_t)w * (size_t)h * 3u) && (fclose(f) == 0);
        free(rgb);
        return ok ? 0 : -1;
    }
    pthread_mutex_unlock(&g_frame_lock);
    free(rgb);
    return -1;
}

int32_t lvglcj_sdl2_screenshot(const char *path)
{
    if (path == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "截图路径不能为空");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    if (g_frame == NULL || g_frame_w <= 0 || g_frame_h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, __func__,
                            "帧缓冲尚未就绪（后端未初始化或渲染输出尺寸未知）");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }
    if (atomic_load(&g_frame_painted) == 0) {
        /* 还没画过任何一帧：给出"不支持"而不是一张全黑图 —— 后者会让人以为界面就那样 */
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, __func__,
                            "尚未渲染过任何一帧，没有可截的画面");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    /*
     * 全同步、纯 CPU：不碰 SDL 渲染器，也不等待任何线程 ——
     * 因此**在任意线程上调用都安全**。这与"请求渲染线程"那版的关键区别见上方长注释：
     * 仓颉的 M:N 线程模型会让调用方线程与注册时的线程号不同，那条路必然超时。
     */
    if (sdl2_write_screenshot(path) != 0) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "写截图失败（路径不可写？格式转换失败？）");
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }
    return LVGLCJ_OK;
}

/* 取走最近一次按下的键（取走后清零）。没有待取的键时返回 0。 */
uint32_t lvglcj_sdl2_take_key(void)
{
    /* 同样用 exchange：先 load 再 clear 会丢掉"两次调用之间刚按下的那一击" */
    return atomic_exchange(&g_in_key_taken, 0u);
}

int32_t lvglcj_sdl2_take_wheel_steps(void)
{
    /*
     * ★ 用 exchange 而不是"先 load 再 store"：后者在两次调用之间可能丢掉
     *   主线程刚加上的一格（快速滚动时会丢格，且只在特定时序下出现）。
     */
    return atomic_exchange(&g_in_wheel_steps, 0);
}

/* 测试注入：绕过 SDL，直接把滚轮格数塞进去（与 set_render_suppressed 同类的钩子）。 */
int32_t lvglcj_sdl2_inject_wheel(int32_t steps)
{
    atomic_fetch_add(&g_in_wheel_steps, steps);
    return LVGLCJ_OK;
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

/*
 * 渲染事件序号 ≠ 最新 staged 序号的次数。
 *
 * ★ 含义要说准（这一点是被实测纠正的）：它**只说明生产者跑在消费者前面**，
 *   在慢消费者（KMSDRM 上的 Present）上出现是正常的，**不等于画错** ——
 *   事件自带槽号与矩形，贴上去的仍是它自己那块数据。
 *   真正"丢数据"的判据是 lvglcj_sdl2_slot_clobber_count()。
 */
int32_t lvglcj_sdl2_stale_apply_count(void)
{
    return atomic_load(&g_stale_apply);
}

/*
 * **真正丢数据**的次数：取到的暂存槽还没被应用，就又被写了一次。
 *
 * 这是"画面上有没有条带没被写过"的直接判据，长期必须为 0。
 * 修复前暂存区只有一块，所以每次等待超时都必然走这条分支 —— 在 KMSDRM 上
 * 表现为渲染条带高度整数倍的**纯黑带**（实测它与 waitTimeout 同步增长：
 * 三次运行分别是 4/6、15/14、11/10；换 dummy 驱动（Present 免费）则两者归零、黑带消失）。
 */
int32_t lvglcj_sdl2_slot_clobber_count(void)
{
    return atomic_load(&g_slot_clobber);
}

/*
 * 当前**实际生效**的 SDL 视频驱动名（"x11" / "KMSDRM" / "dummy" / "wayland" ...）。
 *
 * ★★ 为什么必须有它：`SDL_VIDEODRIVER=kmsdrm` 不是一个命令，而是一个**请求**。
 *    当该驱动不可用时（没有已连接的显示器、DRM master 被桌面占着、
 *    这份 libSDL2 没编 KMS 支持），SDL **不会报错退出**，而是静默回落到
 *    它还能用的驱动 —— 程序照跑、帧照 Present、计数照涨。
 *    于是"kmsdrm 跑通了"会变成一个**没有任何依据**的结论，而且它看起来
 *    和真跑通了一模一样。要判定，只能反过来问它：你实际用的是谁？
 *
 * 返回的字符串由 SDL 持有（生命周期同视频子系统），调用方不要释放。
 * 视频未初始化时返回 `""` 而不是 NULL —— 免得调用方在空指针上取值。
 */
const char *lvglcj_sdl2_video_driver(void)
{
    const char *name = SDL_GetCurrentVideoDriver();
    return (name != NULL) ? name : "";
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

/* 测试注入：把 Present 注入成慢动作（见头文件说明）。负数视为 0。 */
int32_t lvglcj_sdl2_set_present_delay(int32_t ms)
{
    atomic_store(&g_present_delay_ms, ms > 0 ? ms : 0);
    return LVGLCJ_OK;
}

int32_t lvglcj_sdl2_inject_quit(void)
{
    sdl2_mark_paused(1);
    return LVGLCJ_OK;
}
