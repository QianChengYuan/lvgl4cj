/*
 * 用 X11 直接读窗口像素，回答一个用日志回答不了的问题：**画面上到底有没有东西**。
 *
 * 为什么需要它：排查黑屏时，"窗口全黑"与"窗口其实有内容但主题太暗"从日志上分不出
 * （flush/present 计数两种情况下都可能是正常的）。而本机没有任何截图工具
 * （xwd / import / scrot / ffmpeg / PIL 都没有），所以这里直接调 Xlib 取像素。
 *
 * 用法：先让 gallery_cj 跑起来，再执行本程序。
 *   ./x11_window_probe            → 报告窗口尺寸、非黑像素占比、若干取样点的颜色
 *   ./x11_window_probe 63232 ...  → 额外把给定的 RGB565 值作为"期望色"去匹配
 *
 * 判读：
 *   非黑像素≈0            → 确实什么都没画（呈现/纹理/窗口层的问题）
 *   非黑像素很多且取样点色值正确 → 渲染是好的，"黑"只是深色主题造成的观感
 *   非黑像素很多但色值不对     → 像素格式/通道顺序不对
 */
#include <X11/Xlib.h>
#include <unistd.h>
/* ★ XGetPixel / XDestroyImage 声明在 Xutil.h，不在 Xlib.h。
 *   少这个头只会得到一条 implicit declaration 告警，然后在**链接**阶段失败 ——
 *   与之前 C 侧踩过的「隐式声明截断指针」是同一类问题。 */
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window find_named(Display *d, Window w, const char *name, int depth)
{
    if (depth > 8) {
        return 0;
    }
    char *wn = NULL;
    if (XFetchName(d, w, &wn) && wn != NULL) {
        int hit = (strstr(wn, name) != NULL) ? 1 : 0;
        XFree(wn);
        if (hit) {
            return w;
        }
    }
    Window root, parent, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &n)) {
        return 0;
    }
    Window found = 0;
    for (unsigned int i = 0; i < n && found == 0; i++) {
        found = find_named(d, kids[i], name, depth + 1);
    }
    if (kids != NULL) {
        XFree(kids);
    }
    return found;
}

static void report_point(XImage *im, int x, int y, const char *label)
{
    if (x < 0 || y < 0 || x >= im->width || y >= im->height) {
        printf("  %-22s (out of range)\n", label);
        return;
    }
    unsigned long p = XGetPixel(im, x, y);
    int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
    printf("  %-22s (%3d,%3d) rgb=(%3d,%3d,%3d) 0x%06lX\n", label, x, y, r, g, b, p & 0xFFFFFF);
}

/*
 * 通道取值：按 XImage 自带的掩码取，而不是假定 BGRA 的字节序。
 * 掩码在 XGetImage 的结果里是权威描述，28/30 位深也能对。
 */
static unsigned int chan_of(unsigned long p, unsigned long mask)
{
    if (mask == 0) {
        return 0;
    }
    unsigned int s = 0;
    while (((mask >> s) & 1UL) == 0) {
        s++;
    }
    return (unsigned int)((p & mask) >> s);
}

/*
 * 端到端注入滚轮用。XTest 在 **X 服务端**注入，等价于真的动了滚轮 ——
 * 注意不能用 XSendEvent：SDL2 走 XInput2 收输入，合成的客户端事件它收不到。
 * 手写原型是为了不依赖 libXtst 的开发头（本机只有运行库 libXtst.so.6）。
 */
extern int XTestFakeButtonEvent(Display *dpy, unsigned int button, int is_press,
                                unsigned long delay);
extern int XTestFakeMotionEvent(Display *dpy, int screen, int x, int y, unsigned long delay);
extern int XTestFakeKeyEvent(Display *dpy, unsigned int keycode, int is_press,
                             unsigned long delay);

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (d == NULL) {
        printf("XOpenDisplay 失败（没有可用的 X 显示）\n");
        return 1;
    }
    Window target = find_named(d, DefaultRootWindow(d), "lvgl4cj", 0);
    if (target == 0) {
        printf("没找到标题含 lvgl4cj 的窗口（示例是否在运行？）\n");
        XCloseDisplay(d);
        return 2;
    }

    XWindowAttributes at;
    if (!XGetWindowAttributes(d, target, &at)) {
        printf("XGetWindowAttributes 失败\n");
        XCloseDisplay(d);
        return 3;
    }
    /*
     * 可选：注入滚轮（argv[2] = 格数，负 = 向上；argv[3] = 注入后等待毫秒，默认 400）。
     * 用于端到端验证"滚轮是否真的让画面滚动"：注入后等一会儿再抓图，
     * 用 scripts/check_ui.py 比较卡片带的位置即可判断（位置动了就是真的滚了）。
     */
    if (argc >= 3) {
        int clicks = atoi(argv[2]);
        int wait_ms = (argc >= 4) ? atoi(argv[3]) : 400;
        int n = clicks < 0 ? -clicks : clicks;
        unsigned int btn = (clicks > 0) ? 5u : 4u; /* X: 5 = 向下滚，4 = 向上滚 */

        /*
         * 先把指针移进窗口：XTest 的按键事件落在**指针当前位置**，
         * 指针若在窗口外，滚轮就滚到别处去了。
         */
        Window child;
        int rx = 0;
        int ry = 0;
        if (XTranslateCoordinates(d, target, DefaultRootWindow(d), at.width / 2, at.height / 2,
                                  &rx, &ry, &child)) {
            XTestFakeMotionEvent(d, -1, rx, ry, 0);
        }
        XSetInputFocus(d, target, RevertToParent, CurrentTime);
        XFlush(d);
        usleep(60 * 1000);

        for (int i = 0; i < n; i++) {
            XTestFakeButtonEvent(d, btn, 1, 0);
            XTestFakeButtonEvent(d, btn, 0, 0);
            XFlush(d);
        }
        printf("已注入 %d 次滚轮（%s），等待 %d ms 后抓图\n", n, clicks > 0 ? "向下" : "向上",
               wait_ms);
        if (wait_ms > 0) {
            usleep(wait_ms * 1000);
        }
    }

    /*
     * 可选：注入一次按键（argv[4] = X keysym，例如 0x73 = 's'）。
     * 用于端到端验证"按键 -> 应用动作"这条链（例如示例的存图快捷键）。
     */
    if (argc >= 5) {
        unsigned int ks = (unsigned int)strtoul(argv[4], NULL, 0);
        KeyCode kc = XKeysymToKeycode(d, (KeySym)ks);
        if (kc != 0) {
            XTestFakeKeyEvent(d, (unsigned int)kc, 1, 0);
            XTestFakeKeyEvent(d, (unsigned int)kc, 0, 0);
            XFlush(d);
            printf("已注入按键 keysym=0x%x（keycode=%u）\n", ks, (unsigned int)kc);
            usleep(300 * 1000);
        } else {
            printf("keysym=0x%x 没有对应 keycode，未注入\n", ks);
        }
    }

    XImage *im = XGetImage(d, target, 0, 0, (unsigned int)at.width, (unsigned int)at.height,
                           AllPlanes, ZPixmap);
    if (im == NULL) {
        printf("XGetImage 失败\n");
        XCloseDisplay(d);
        return 4;
    }

    /*
     * 导出 P6 PPM（若给了参数 1 作为路径）。
     *
     * 为什么需要它：scripts/check_ui.py 的现场抓图依赖 import / xwd / xwininfo /
     * python-xlib，而本机这四个都没有 —— 只有 Xlib 直连是可靠的（本探针已证明）。
     * 于是绕一步：探针导出 PPM，再由 Python 侧做体检。
     * 这样"能不能自己看见画面"就不再取决于装了哪些工具。
     */
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "wb");
        if (f == NULL) {
            printf("打不开输出文件 %s\n", argv[1]);
        } else {
            fprintf(f, "P6\n%d %d\n255\n", im->width, im->height);
            unsigned char *row = (unsigned char *)malloc((size_t)im->width * 3u);
            if (row != NULL) {
                for (int y = 0; y < im->height; y++) {
                    for (int x = 0; x < im->width; x++) {
                        unsigned long p = XGetPixel(im, x, y);
                        row[x * 3 + 0] = (unsigned char)chan_of(p, im->red_mask);
                        row[x * 3 + 1] = (unsigned char)chan_of(p, im->green_mask);
                        row[x * 3 + 2] = (unsigned char)chan_of(p, im->blue_mask);
                    }
                    fwrite(row, 1, (size_t)im->width * 3u, f);
                }
                free(row);
            }
            fclose(f);
            printf("已导出 %s（%dx%d）\n", argv[1], im->width, im->height);
        }
    }

    long total = (long)im->width * im->height;
    long nonblack = 0;
    for (int y = 0; y < im->height; y++) {
        for (int x = 0; x < im->width; x++) {
            unsigned long p = XGetPixel(im, x, y);
            if ((p & 0xFFFFFF) != 0) {
                nonblack++;
            }
        }
    }
    printf("窗口尺寸 %dx%d，像素 %ld，非黑 %ld（%.1f%%）\n", im->width, im->height, total,
           nonblack, total > 0 ? 100.0 * (double)nonblack / (double)total : 0.0);

    /*
     * 取样点按 gallery_cj 的实际布局算：卡片从 y=8 起，颜色条在卡片内 y=26 处、
     * 高 22、第一个方块 x=16 宽 60，之后步进 66。
     */
    printf("颜色条 6 个方块中心：\n");
    for (int i = 0; i < 6; i++) {
        report_point(im, 16 + i * 66 + 30, 8 + 26 + 11, "colour-bar");
    }
    printf("其它取样点：\n");
    report_point(im, 400, 8 + 26 + 11, "bar-tail(空)");
    report_point(im, 400, 240, "screen-middle");
    report_point(im, 16, 8 + 64 + 30, "stats-card-bg");

    /*
     * ★ 与位置无关的色彩普查：在整幅画面里找那 6 个测试色。
     *
     *   为什么需要它：上一轮按固定坐标取样，结果全黑 —— 不是没画，而是**画面滚动了**
     *   （内容超出屏幕、容器可滚动，指针事件一来就可能位移）。固定坐标的结论会随
     *   滚动状态翻转，那就不算判据。普查只问"这些颜色在不在画面里"，与布局无关。
     *   容差 ±8：RGB565 量化会带来几个 LSB 的偏差，实测约 2~3。
     */
    {
        static const int want[6][3] = {
            {0xFF, 0x3B, 0x30}, {0x34, 0xC7, 0x59}, {0x00, 0x7A, 0xFF},
            {0xFF, 0xCC, 0x00}, {0xFF, 0x2D, 0x55}, {0xFF, 0xFF, 0xFF},
        };
        static const char *names[6] = {"red", "green", "blue", "yellow", "pink", "white"};
        printf("色彩普查（容差 ±8）：\n");
        for (int k = 0; k < 6; k++) {
            long hit = 0;
            for (int y = 0; y < im->height; y++) {
                for (int x = 0; x < im->width; x++) {
                    unsigned long p = XGetPixel(im, x, y);
                    int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
                    int dr = r - want[k][0], dg = g - want[k][1], db = b - want[k][2];
                    if (dr < 0) dr = -dr;
                    if (dg < 0) dg = -dg;
                    if (db < 0) db = -db;
                    if (dr <= 8 && dg <= 8 && db <= 8) {
                        hit++;
                    }
                }
            }
            printf("  %-7s 期望 rgb=(%3d,%3d,%3d)  命中 %ld 像素\n", names[k], want[k][0],
                   want[k][1], want[k][2], hit);
        }
    }

    XDestroyImage(im);
    XCloseDisplay(d);
    (void)argc;
    (void)argv;
    return 0;
}
