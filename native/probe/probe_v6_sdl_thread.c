/*
 * probe_v6_sdl_thread.c —— V6：SDL2 双线程渲染同步 + 超时（设计文档 §8.1.2）
 *
 * 要验证的机制（方案 A 下才需要，方案 C 下跳过）：
 *   SDL 的事件循环必须在**创建窗口的线程**（通常主线程）；
 *   LVGL 主循环在另一条线程。两者分离，用事件 + 信号量握手：
 *
 *     LVGL 线程                             主线程（SDL 事件循环）
 *     ---------                              --------------------
 *     flush_cb: SDL_UpdateTexture
 *               SDL_PushEvent(渲染请求) ──►
 *                                            SDL_PollEvent 收到请求
 *                                            SDL_RenderCopy + RenderPresent
 *                                       ◄──  sem_post(完成)
 *     flush_wait_cb: sem_timedwait(100ms)
 *
 * 三个必须验证的子项（§8.1.2 的「强制要求」）：
 *   A 正常往返：等待被满足，且耗时远小于超时值
 *   B 超时生效：渲染方不响应时，等待方在 ~100ms 后返回**而不是挂死**
 *   C 关闭即时解锁：窗口关闭/最小化时，主线程主动 sem_post + 标记暂停，
 *                   等待方立即返回（不等超时）
 *
 * B 与 C 是 §17 断言 7「窗口关闭时 flush_wait_cb 立即返回，不挂死」的直接证据。
 */
#include "lvglcj_probe.h"
#include "lvglcj_bridge.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define V6_W 400
#define V6_H 300
#define V6_WAIT_TIMEOUT_MS 100 /* §8.1.2 默认值 */

/* 阶段：由主线程驱动，等待线程据此行动 */
typedef enum { PHASE_A = 0, PHASE_B = 1, PHASE_C = 2, PHASE_DONE = 3 } v6_phase_t;

static sem_t          g_sem_request;  /* 等待线程 → 主线程：请渲染 */
static sem_t          g_sem_done;     /* 主线程 → 等待线程：渲染完成 */
static _Atomic int    g_phase = PHASE_A;
static _Atomic int    g_pause_render = 0; /* 窗口关闭/最小化 → 暂停渲染 */
static _Atomic int    g_render_suppressed = 0; /* 检查 B 用：故意不响应 */

/* 等待线程的记录 */
static _Atomic int64_t g_elapsed_ms[3];
static _Atomic int     g_wait_rc[3]; /* 0 = 被唤醒；-1 = 超时 */

static SDL_Window   *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture = NULL;
static uint32_t      g_render_event = 0;

static int64_t v6_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* 模拟 flush_wait_cb：带超时地等待「主线程渲染完成」 */
static int v6_wait_render_done(int timeout_ms, int64_t *elapsed_out)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    int64_t add_ms = timeout_ms;
    deadline.tv_sec += (time_t)(add_ms / 1000);
    deadline.tv_nsec += (long)((add_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    int64_t t0 = v6_now_ms();
    int rc = sem_timedwait(&g_sem_done, &deadline);
    *elapsed_out = v6_now_ms() - t0;
    return (rc == 0) ? 0 : -1;
}

/* 扮演 LVGL 线程：发起渲染请求并等待完成 */
static void *v6_waiter_thread(void *arg)
{
    (void)arg;

    for (int phase = 0; phase < 3; phase++) {
        /* 通知主线程「该第 phase 阶段了」并抛出渲染请求 */
        atomic_store(&g_phase, phase);
        sem_post(&g_sem_request);

        int64_t elapsed = 0;
        int rc = v6_wait_render_done(V6_WAIT_TIMEOUT_MS, &elapsed);

        atomic_store(&g_wait_rc[phase], rc);
        atomic_store(&g_elapsed_ms[phase], elapsed);
    }

    atomic_store(&g_phase, PHASE_DONE);
    sem_post(&g_sem_request);
    return NULL;
}

/* 主线程：模拟「渲染一方」的响应策略 */
static void v6_on_render_request(void)
{
    if (atomic_load(&g_pause_render)) {
        /* 窗口关闭/最小化：主动放行等待方，让它立即返回（§8.1.2 强制要求 3） */
        sem_post(&g_sem_done);
        return;
    }

    if (atomic_load(&g_render_suppressed)) {
        /* 检查 B：故意不响应，用来验证超时路径 */
        return;
    }

    /* 正常渲染路径：与真实后端的 flush 一致（纹理上传 + 呈现） */
    if (g_texture != NULL) {
        SDL_UpdateTexture(g_texture, NULL, NULL, 0); /* NULL 像素数据仅作调用路径验证 */
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
    }
    sem_post(&g_sem_done);
}

int32_t lvglcj_probe_v6_sdl_thread(void)
{
    printf("\n=== V6: SDL2 双线程渲染同步 + 超时 ===\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        /* 无显示环境（纯 headless CI）不算失败，但要明确标注为 SKIP */
        char buf[256];
        snprintf(buf, sizeof(buf), "SKIP（SDL_Init 失败：%s）", SDL_GetError());
        lvglcj_probe_report("V6", "sdl_init", buf);
        printf("[V6] 结论：无法验证（无可用显示环境）\n");
        return 0;
    }

    g_window = SDL_CreateWindow("lvgl4cj probe V6", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, V6_W, V6_H, SDL_WINDOW_SHOWN);
    if (g_window == NULL) {
        char buf[256];
        snprintf(buf, sizeof(buf), "SKIP（CreateWindow 失败：%s）", SDL_GetError());
        lvglcj_probe_report("V6", "sdl_window", buf);
        SDL_Quit();
        return 0;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == NULL) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (g_renderer == NULL) {
        lvglcj_probe_report("V6", "sdl_renderer", "FAIL");
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    /* ★ 像素格式必须与 LVGL 显示格式一一对应，否则花屏（§8.1.3） */
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
                                 SDL_TEXTUREACCESS_STREAMING, V6_W, V6_H);
    if (g_texture == NULL) {
        lvglcj_probe_report("V6", "sdl_texture", "FAIL");
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    g_render_event = SDL_RegisterEvents(1);
    if (g_render_event == (uint32_t)-1) {
        lvglcj_probe_report("V6", "sdl_register_events", "FAIL");
        return 1;
    }

    sem_init(&g_sem_request, 0, 0);
    sem_init(&g_sem_done, 0, 0);

    pthread_t waiter;
    if (pthread_create(&waiter, NULL, v6_waiter_thread, NULL) != 0) {
        lvglcj_probe_report("V6", "pthread_create", "FAIL");
        return 1;
    }

    /*
     * 主线程 = SDL 事件循环。
     * 阶段 B 的「不响应」与阶段 C 的「主动放行」都在这里按阶段切换。
     */
    int last_phase = -1;
    for (;;) {
        int phase = atomic_load(&g_phase);
        if (phase != last_phase) {
            last_phase = phase;
            if (phase == PHASE_B) {
                atomic_store(&g_render_suppressed, 1); /* 让等待方超时 */
            } else if (phase == PHASE_C) {
                atomic_store(&g_render_suppressed, 0);
                /* 模拟窗口关闭：标记暂停并主动放行 */
                atomic_store(&g_pause_render, 1);
                sem_post(&g_sem_done);
            }
        }
        if (phase == PHASE_DONE) {
            break;
        }

        /* 把渲染请求以 SDL 自定义事件形式投递给主线程 —— 与真实后端同一机制 */
        if (sem_trywait(&g_sem_request) == 0) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = g_render_event;
            SDL_PushEvent(&ev);
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == g_render_event) {
                v6_on_render_request();
            } else if (ev.type == SDL_QUIT) {
                atomic_store(&g_pause_render, 1);
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE ||
                    ev.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    /* §8.1.2 强制要求 3：必须主动解锁等待方 */
                    atomic_store(&g_pause_render, 1);
                    sem_post(&g_sem_done);
                }
            }
        }
        SDL_Delay(1);
    }

    pthread_join(waiter, NULL);

    /* ------------------------------------------------------------ 判读 */
    int rc_a = atomic_load(&g_wait_rc[0]);
    int rc_b = atomic_load(&g_wait_rc[1]);
    int rc_c = atomic_load(&g_wait_rc[2]);
    int64_t t_a = atomic_load(&g_elapsed_ms[0]);
    int64_t t_b = atomic_load(&g_elapsed_ms[1]);
    int64_t t_c = atomic_load(&g_elapsed_ms[2]);

    char buf[160];
    snprintf(buf, sizeof(buf), "rc=%d elapsed=%lldms", rc_a, (long long)t_a);
    lvglcj_probe_report("V6", "A_normal_roundtrip", buf);
    snprintf(buf, sizeof(buf), "rc=%d elapsed=%lldms（期望 rc=-1 且约 %dms）",
             rc_b, (long long)t_b, V6_WAIT_TIMEOUT_MS);
    lvglcj_probe_report("V6", "B_timeout_path", buf);
    snprintf(buf, sizeof(buf), "rc=%d elapsed=%lldms（期望 rc=0 且 < 20ms）",
             rc_c, (long long)t_c);
    lvglcj_probe_report("V6", "C_close_fast_release", buf);

    int ok_a = (rc_a == 0 && t_a < V6_WAIT_TIMEOUT_MS);
    int ok_b = (rc_b == -1 && t_b >= (V6_WAIT_TIMEOUT_MS - 15));
    int ok_c = (rc_c == 0 && t_c < 20);

    printf("\n[V6] 结论：A(正常往返)=%s  B(超时生效)=%s  C(关闭即时解锁)=%s\n",
           ok_a ? "PASS" : "FAIL", ok_b ? "PASS" : "FAIL", ok_c ? "PASS" : "FAIL");

    sem_destroy(&g_sem_request);
    sem_destroy(&g_sem_done);
    SDL_DestroyTexture(g_texture);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();

    return (ok_a && ok_b && ok_c) ? 0 : 1;
}

int main(void)
{
    return (lvglcj_probe_v6_sdl_thread() == 0) ? 0 : 1;
}
