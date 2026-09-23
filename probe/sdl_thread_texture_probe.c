/*
 * 探针：SDL2 的「纹理更新」到底能不能在非创建线程里生效。
 *
 * 起因：lvgl4cj 的 SDL2 后端把 SDL_UpdateTexture 放在 LVGL 线程（flush 回调），
 * 把 SDL_RenderPresent 放在主线程（渲染事件）。现象是 flush==present、零超时，
 * 但真实窗口全黑 —— 连纯色矩形都没有。SDL2 的 renderer 不是线程安全的，
 * 这是第一嫌疑。
 *
 * ★ 只测「黑不黑」是不够的：必须同时证明**这个回读方法本身是好的**，
 *   否则"读到黑"可能只是回读不工作。所以这里设三组对照：
 *     ① 直接在 renderer 上画一个红矩形 + 回读      → 验证回读通路与绘制通路
 *     ② 主线程更新纹理 + 回读                      → 验证"纹理 → 屏幕"这条路
 *     ③ 另一个线程更新纹理 + 回读                  → 这才是要验证的假设
 *   判读：
 *     ① 红、② 红、③ 黑 → 跨线程更新确实丢失，假设成立
 *     ① 黑            → 回读方法不可用，本探针**没有结论**（不要拿它当证据）
 *     ①②③ 全红        → 跨线程没问题，黑屏要另找原因
 */
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 120
#define H 60
#define RED565 0xF800

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static Uint16 *pix;
static int pitch;

/* 回读中心像素；返回 -1 表示回读调用失败 */
static int probe_center(void)
{
    Uint16 v = 0;
    SDL_Rect r;
    r.x = W / 2;
    r.y = H / 2;
    r.w = 1;
    r.h = 1;
    if (SDL_RenderReadPixels(ren, &r, SDL_PIXELFORMAT_RGB565, &v, 2) != 0) {
        return -1;
    }
    return (int)v;
}

static void *cross_thread_updater(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20; i++) {
        SDL_UpdateTexture(tex, NULL, pix, pitch);
        SDL_Delay(20);
    }
    return NULL;
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                           SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (ren == NULL) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
    if (win == NULL || ren == NULL || tex == NULL) {
        printf("init failed: %s\n", SDL_GetError());
        return 1;
    }
    pitch = W * 2;
    pix = malloc((size_t)pitch * H);
    for (int i = 0; i < W * H; i++) {
        pix[i] = RED565;
    }
    printf("driver=%s\n", SDL_GetCurrentVideoDriver());

    /* ① 直接在 renderer 上画红矩形 → 回读 */
    SDL_SetRenderDrawColor(ren, 255, 0, 0, 255);
    SDL_RenderClear(ren);
    printf("case1 (draw red rect)        readback=%d\n", probe_center());
    SDL_RenderPresent(ren);

    /* ② 主线程更新纹理 → 回读 */
    SDL_UpdateTexture(tex, NULL, pix, pitch);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    printf("case2 (main-thread update)    readback=%d  (期望 63488)\n", probe_center());
    SDL_RenderPresent(ren);

    /* ③ 换一个线程更新纹理 → 回读 */
    for (int i = 0; i < W * H; i++) {
        pix[i] = 0x001F; /* 蓝：与②的红区分开，避免"看到旧内容"被当成功 */
    }
    pthread_t t;
    pthread_create(&t, NULL, cross_thread_updater, NULL);
    pthread_join(t, NULL);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    printf("case3 (cross-thread update)   readback=%d  (期望 31 = 蓝)\n", probe_center());
    SDL_RenderPresent(ren);

    free(pix);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
