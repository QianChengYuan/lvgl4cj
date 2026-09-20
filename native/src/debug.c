/*
 * debug.c —— 调试与可观测性（设计文档 §7.3 / §5.11）
 *
 * 提供四类能力：
 *   · 对象计数  lvglcj_obj_count()      —— 与句柄数对比，是「句柄泄漏」的交叉判据
 *   · 内存监控  lvglcj_mem_monitor()    —— 封装 lv_mem_monitor
 *   · 性能采样  lvglcj_perf_sample()    —— FPS / 忙碌占比 / 刷新耗时 / 对象数
 *   · 对象树 dump lvglcj_debug_dump_tree()
 *
 * ==================== 为什么 obj_count 要自己遍历 ====================
 * LVGL v9.2 **没有**全局对象计数 API（v8 的 lv_obj_count_children 之类已移除，
 * 也没有 lv_obj_count_recursive）。而 §7.3 要求「handleCount(ALIVE) 与 objCount
 * 长期差值恒定」—— 这个判据的意义恰恰在于**两个数是独立测量的**：
 *   句柄数来自我们的句柄表，对象数必须来自 LVGL 自己的对象树。
 * 若用「句柄表里 type==obj 的条数」来充当 objCount，就变成了自己和自己比，
 * 判据完全失效。因此这里老老实实遍历对象树。
 *
 * ==================== 性能指标的口径（必须写清楚）====================
 * LVGL 官方的 FPS/CPU 显示属于 LV_USE_PERF_MONITOR，且**没有公开的取数 API**
 * （只有 show/hide 覆盖层的接口）。因此这里的指标由本层自己埋点计算，
 * 口径如下（与官方 perf monitor 的数值不保证逐位相同，故明确写出）：
 *
 *   fps          每 1s 结算一次：完成的刷新次数 / 经过的秒数。
 *                「一次刷新完成」的判据是 lv_display_flush_is_last() 为真。
 *   cpu_percent  最近 1s 内**在 lv_timer_handler() 里消耗的墙钟时间**占比。
 *                注意口径：这是「主循环忙碌比」，不是进程 CPU 占用率。
 *                在 LV_USE_OS=LV_OS_NONE 的单线程模型下二者接近；
 *                多线程后端（SDL2）下 SDL 渲染线程的时间不计入。
 *   refr_time_ms 最近一次 lv_timer_handler() 的耗时（内部包含渲染与 flush）。
 *   draw_time_ms 最近一帧中 flush 回调内累计耗时（像素搬运部分）。
 *   obj_count    见 lvglcj_obj_count()。
 *
 * 首个 1s 窗口未结算前，fps / cpu_percent 返回 0 —— 返回 0 而不是编造值，
 * 是为了让「还没测出来」与「真的是 0」在数值上不可区分时，调用方能靠
 * 「等一个窗口」来消除歧义（基线记录脚本正是这么做的）。
 */
#include "lvglcj_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------ 单调时钟 */

uint64_t lvglcj_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

/* ------------------------------------------------------------ 性能埋点 */

#define PERF_WINDOW_MS 1000u

static uint32_t g_window_start_ms   = 0;   /* 0 表示窗口尚未开始 */
static uint32_t g_frames_in_window  = 0;
static uint32_t g_busy_us_in_window = 0;

static uint32_t g_fps               = 0;
static uint32_t g_cpu_percent       = 0;
static uint32_t g_last_handler_us   = 0;

static uint32_t g_cur_frame_flush_us  = 0;
static uint32_t g_last_frame_flush_us = 0;

/*
 * 一次 flush 调用的埋点。is_last 来自 lv_display_flush_is_last()。
 * 只有 is_last 为真才计一「帧」——局部刷新会多次进 flush，那是同一帧。
 */
void lvglcj_perf_on_flush(uint32_t flush_us, int32_t is_last)
{
    g_cur_frame_flush_us += flush_us;
    if (!is_last) {
        return;
    }
    g_last_frame_flush_us = g_cur_frame_flush_us;
    g_cur_frame_flush_us = 0;
    g_frames_in_window++;
}

/*
 * 每次 lvglcj_timer_handler() 后调用一次。
 * 窗口结算放在这里而不是 flush 里：定时器处理每轮都会跑，
 * 因此即使**完全没有刷新**（例如空闲期），FPS 也会被正确结算为 0，
 * 而不会停留在上一次窗口的旧值上。
 */
void lvglcj_perf_on_timer_handler(uint32_t handler_us)
{
    g_last_handler_us = handler_us;
    g_busy_us_in_window += handler_us;

    uint32_t now_ms = (uint32_t)(lvglcj_now_us() / 1000u);
    if (g_window_start_ms == 0) {
        g_window_start_ms = now_ms;
        return;
    }
    uint32_t elapsed = now_ms - g_window_start_ms;
    if (elapsed < PERF_WINDOW_MS) {
        return;
    }

    g_fps = (uint32_t)(((uint64_t)g_frames_in_window * 1000u) / elapsed);
    uint64_t window_us = (uint64_t)elapsed * 1000u;
    g_cpu_percent = (uint32_t)(((uint64_t)g_busy_us_in_window * 100u) / window_us);

    g_window_start_ms = now_ms;
    g_frames_in_window = 0;
    g_busy_us_in_window = 0;
}

int32_t lvglcj_perf_sample(lvglcj_perf_t *out)
{
    if (out == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "out 为空");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->fps = g_fps;
    out->cpu_percent = g_cpu_percent;
    out->refr_time_ms = g_last_handler_us / 1000u;
    out->draw_time_ms = g_last_frame_flush_us / 1000u;
    out->obj_count = (uint32_t)lvglcj_obj_count();
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 内存监控 */

int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag, uint32_t *max_used,
                           uint32_t *free_size)
{
    /*
     * lv_mem_monitor 只在内置分配器（LV_STDLIB_BUILTIN）下才有意义。
     * 换成标准 malloc 后 LVGL 无从知道自己用了多少 —— 那时应改看进程 RSS。
     * 这里不猜测，直接按编译期开关给出明确结论。
     */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    lv_mem_monitor_t m;
    memset(&m, 0, sizeof(m));
    lv_mem_monitor(&m);

    if (used != NULL) {
        *used = (uint32_t)m.used_pct;          /* 百分比 */
    }
    if (frag != NULL) {
        *frag = (uint32_t)m.frag_pct;          /* 百分比 */
    }
    if (max_used != NULL) {
        *max_used = (uint32_t)m.max_used;      /* 字节 */
    }
    if (free_size != NULL) {
        *free_size = (uint32_t)m.free_size;    /* 字节 */
    }
    return LVGLCJ_OK;
#else
    (void)used;
    (void)frag;
    (void)max_used;
    (void)free_size;
    lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, __func__,
                        "内存监控需要 LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN");
    return LVGLCJ_ERR_NOT_SUPPORTED;
#endif
}

/* ------------------------------------------------------------ 对象计数 */

/* 遍历深度上限：防御被破坏的对象树导致栈溢出（正常布局远达不到） */
#define OBJ_COUNT_MAX_DEPTH 64

/*
 * 递归计数，depth 超过 max_depth 即停。
 * 参数化 max_depth 而不是写死常量：dump_tree 需要按调用方给的深度截断，
 * 若两处各写一份遍历，将来改一处就会让「计数」与「dump」的口径悄悄分叉。
 */
static uint32_t count_subtree(const lv_obj_t *obj, int32_t depth, int32_t max_depth)
{
    if (obj == NULL || depth > max_depth) {
        return 0;
    }
    uint32_t n = 1;
    uint32_t kids = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < kids; i++) {
        n += count_subtree(lv_obj_get_child(obj, (int32_t)i), depth + 1, max_depth);
    }
    return n;
}

static int32_t seen_contains(const lv_obj_t **seen, int32_t n, const lv_obj_t *o)
{
    for (int32_t i = 0; i < n; i++) {
        if (seen[i] == o) {
            return 1;
        }
    }
    return 0;
}

/*
 * 对象总数 = 每个 display 的 active screen 子树 + 三个 layer 子树，**去重**。
 *
 * 为什么要去重：layer（bottom/top/sys）在某些形态下与 active screen 指向同一
 * 对象，直接相加会把整棵屏幕子树数两遍，objCount 凭空翻倍 ——
 * 而「handleCount - objCount 恒定」这条判据会被这种系统性偏差掩盖。
 */
int32_t lvglcj_obj_count(void)
{
    /*
     * seen 容量：display 数 + 3 个 layer，正常远小于 16。
     * 溢出时宁可不数（返回已计部分）也不要越界写。
     */
#define OBJ_COUNT_MAX_SCREENS 16
    const lv_obj_t *seen[OBJ_COUNT_MAX_SCREENS];
    int32_t n_seen = 0;
    uint32_t total = 0;

    lv_display_t *d = lv_display_get_default();
    for (; d != NULL; d = lv_display_get_next(d)) {
        lv_obj_t *s = lv_display_get_screen_active(d);
        if (s != NULL && !seen_contains(seen, n_seen, s) && n_seen < OBJ_COUNT_MAX_SCREENS) {
            seen[n_seen++] = s;
            total += count_subtree(s, 0, OBJ_COUNT_MAX_DEPTH);
        }
        /*
         * layer 只对默认 display 可取（lv_layer_*() 不带 display 参数）。
         * 非默认 display 的 layer 无法通过公开 API 拿到，这里不假装能拿到 ——
         * 多 display 场景下 objCount 会少算那些 layer，属已知口径限制。
         */
        if (d == lv_display_get_default()) {
            lv_obj_t *layers[3];
            layers[0] = lv_layer_bottom();
            layers[1] = lv_layer_top();
            layers[2] = lv_layer_sys();
            for (int32_t i = 0; i < 3; i++) {
                lv_obj_t *L = layers[i];
                if (L == NULL || seen_contains(seen, n_seen, L) || n_seen >= OBJ_COUNT_MAX_SCREENS) {
                    continue;
                }
                seen[n_seen++] = L;
                total += count_subtree(L, 0, OBJ_COUNT_MAX_DEPTH);
            }
        }
    }
#undef OBJ_COUNT_MAX_SCREENS
    return (int32_t)total;
}

/* ------------------------------------------------------------ 对象树 dump */

static void free_dump_arrays(lvglcj_tree_dump_t *dump)
{
    if (dump->handles != NULL) {
        free(dump->handles);
    }
    if (dump->depths != NULL) {
        free(dump->depths);
    }
    if (dump->names != NULL) {
        for (int32_t i = 0; i < dump->count; i++) {
            free(dump->names[i]);
        }
        free(dump->names);
    }
    dump->handles = NULL;
    dump->depths = NULL;
    dump->names = NULL;
    dump->count = 0;
}

/*
 * 把句柄表里登记的名字复制一份出来；没登记过就返回 "?"。
 *
 * 为什么不直接返回句柄表内部的指针：那会让 dump 的生命周期受句柄表影响，
 * 而 dump 的用途恰恰是「在对象被删除之后回溯当时的树形」——
 * 那时句柄表项可能已经释放。所以这里拷贝。
 */
static char *dup_name(int64_t h)
{
    char buf[LVGLCJ_HANDLE_NAME_MAX];
    buf[0] = '\0';
    lvglcj_handle_get_name(h, buf, (int32_t)sizeof(buf));
    if (buf[0] == '\0') {
        (void)snprintf(buf, sizeof(buf), "?");
    }
    size_t n = strlen(buf);
    char *p = (char *)malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, buf, n + 1);
    return p;
}

static int32_t dump_subtree(const lv_obj_t *obj, int32_t depth, int32_t max_depth,
                            lvglcj_tree_dump_t *out, int32_t cap)
{
    if (obj == NULL || depth > max_depth || out->count >= cap) {
        return 0;
    }
    int32_t idx = out->count;
    int64_t h = lvglcj_handle_of((void *)obj);

    out->handles[idx] = h;
    out->depths[idx] = depth;
    out->names[idx] = dup_name(h);
    out->count++;

    uint32_t kids = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < kids; i++) {
        dump_subtree(lv_obj_get_child(obj, (int32_t)i), depth + 1, max_depth, out, cap);
    }
    return 0;
}

/*
 * 树形 dump：前序展开成三个平行数组（handles / depths / names）。
 *
 * cap 由调用方给出的 out->count 传入（见头文件契约）：
 *   out->count == 0  → 由本函数按需统计后分配（方便调用方，代价是一次额外遍历）
 *   out->count >  0  → 视为容量上限，超出部分丢弃（不报错，返回实际条数）
 */
int32_t lvglcj_debug_dump_tree(int64_t root, int32_t max_depth, lvglcj_tree_dump_t *out)
{
    if (out == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, root, 0, __func__, "out 为空");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    free_dump_arrays(out);

    if (max_depth < 0) {
        max_depth = OBJ_COUNT_MAX_DEPTH;
    }

    lv_obj_t *start = (root == LVGLCJ_HANDLE_NULL) ? lv_screen_active()
                                                   : (lv_obj_t *)lvglcj_ptr_of(root);
    if (start == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, root, 0, __func__,
                            "root 为空且没有活动屏幕");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    /* 先数一遍，按需分配。子树可能很大，但 dump 本就是诊断用途，可接受。 */
    int32_t cap = (int32_t)count_subtree(start, 0, max_depth);
    if (cap <= 0) {
        return LVGLCJ_OK; /* 空树：count 保持 0，数组保持 NULL，free 时安全 */
    }

    lvglcj_tree_dump_t tmp;
    tmp.handles = (int64_t *)calloc((size_t)cap, sizeof(int64_t));
    tmp.depths = (int32_t *)calloc((size_t)cap, sizeof(int32_t));
    tmp.names = (char **)calloc((size_t)cap, sizeof(char *));
    tmp.count = 0;

    if (tmp.handles == NULL || tmp.depths == NULL || tmp.names == NULL) {
        free_dump_arrays(&tmp);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, root, 0, __func__,
                            "dump 数组分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    dump_subtree(start, 0, max_depth, &tmp, cap);

    *out = tmp;
    return out->count;
}

void lvglcj_debug_free_tree_dump(lvglcj_tree_dump_t *dump)
{
    if (dump == NULL) {
        return;
    }
    free_dump_arrays(dump);
}

/* ------------------------------------------------------------ fs（POSIX） */

/*
 * 初始化 LVGL 内置 POSIX 文件系统驱动。
 *
 * ★ 关于 root 参数的限制（不假装支持）：
 *   内置驱动的根路径 LV_FS_POSIX_PATH 是**编译期常量**（本工程配成 "."），
 *   LVGL 只提供 lv_fs_posix_init()，没有「运行时换根」的接口。
 *   要支持任意 root，必须自己注册一个 lv_fs_drv_t —— 那是 P2 的
 *   「自定义 fs 驱动（§3.13.1）」，不在 P0 范围。
 *   所以这里对不支持的 root 明确报错，而不是静默忽略后让用户面对
 *   「文件读不到但也不报错」的谜题。
 */
int32_t lvglcj_fs_init_posix(const char *root)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    if (root != NULL && root[0] != '\0') {
        if (strcmp(root, ".") != 0) {
            lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, __func__,
                                "运行时指定 fs 根路径需自定义驱动（P2）；"
                                "当前根路径由 LV_FS_POSIX_PATH 编译期决定");
            return LVGLCJ_ERR_NOT_SUPPORTED;
        }
    }

    lv_fs_posix_init();
    return LVGLCJ_OK;
}
