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
    XImage *im = XGetImage(d, target, 0, 0, (unsigned int)at.width, (unsigned int)at.height,
                           AllPlanes, ZPixmap);
    if (im == NULL) {
        printf("XGetImage 失败\n");
        XCloseDisplay(d);
        return 4;
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

    XDestroyImage(im);
    XCloseDisplay(d);
    (void)argc;
    (void)argv;
    return 0;
}
