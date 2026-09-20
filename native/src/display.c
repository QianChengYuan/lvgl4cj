/*
 * display.c —— 显示子系统（设计文档 §5.3 / §3.4）
 *
 * ==================== 本文件最重要的两条约束 ====================
 *
 * 1) ★ 绘制缓冲区必须由 **C 侧分配并由桥接层持有**（ADR-002）。
 *    绝不能把仓颉 Array<UInt8> 的底层指针交给 LVGL：
 *    仓颉的 GC 可能移动对象或回收内存，而 LVGL 会在后续多个帧里持续写这块缓冲，
 *    结果就是间歇性花屏甚至段错误 —— 而且极难复现。
 *    因此这里用 calloc 分配，缓冲的生命周期与 display 句柄绑死，
 *    释放顺序固定为「先 lv_display_delete()，再 free(buf)」。
 *
 * 2) ★ 尺寸一律走 lv_draw_buf_width_to_stride()，绝不自己算（§3.4.2）。
 *    自己算 stride 会漏掉 LVGL 的对齐要求（LV_DRAW_BUF_STRIDE_ALIGN），
 *    在 RGB565 且宽度为奇数时必错，表现为隔行错位。
 *    本文件不出现任何 "w * bpp / 8" 形式的计算。
 *
 * ==================== 其他已固化的决定 ====================
 * · PARTIAL 为 MVP 默认；DIRECT/FULL 下 buf_lines **被忽略**（仅记 WARN，不报错）。
 * · PARTIAL 至少 1/10 屏（LVGL 官方建议），请求小于此值时**抬升到下限并告警**，
 *   而不是照原样传给 LVGL（否则 LVGL 内部断言/渲染异常）。
 * · 未注册 flush 回调时必须由 trampoline 自己调 lv_display_flush_ready()，
 *   否则 LVGL 会停在「等待 flush 完成」上，表现为画面只出第一帧。
 * · 改 color_format / 分辨率会改变 stride，与已分配的缓冲不再匹配，
 *   因此在缓冲已分配时**拒绝**这两个操作并提示重建 display —— 静默接受会造成花屏。
 */
#include "lvglcj_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* P0 场景下 display 数量极少（通常 1 个，多屏 P1）；线性表足够且无动态分配失败面 */
#define LVGLCJ_DISP_MAX 8

/* LVGL 官方建议 PARTIAL 缓冲不小于 1/10 屏 */
#define LVGLCJ_PARTIAL_MIN_DIVISOR 10

typedef struct {
    int32_t  used;
    int64_t  handle;
    uint8_t *buf;        /* ★ C 侧分配，桥接层持有（ADR-002） */
    uint32_t buf_bytes;
    uint32_t stride;
    int32_t  buf_mode;
    int32_t  color_format;
    int32_t  width;
    int32_t  height;
    int32_t  flush_cid;
    int32_t  flush_wait_cid;
    /* 后端 sink（C 函数指针，不经闭包表）：见 lvglcj_internal.h 的说明 */
    lvglcj_flush_sink_t      flush_sink;
    void                    *flush_sink_user;
    lvglcj_flush_wait_sink_t flush_wait_sink;
    void                    *flush_wait_sink_user;
} lvglcj_disp_t;

static lvglcj_disp_t g_disp[LVGLCJ_DISP_MAX];

/* ------------------------------------------------------------ 内部查表 */

static lvglcj_disp_t *disp_of(int64_t h)
{
    if (h == LVGLCJ_HANDLE_NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < LVGLCJ_DISP_MAX; ++i) {
        if (g_disp[i].used && g_disp[i].handle == h) {
            return &g_disp[i];
        }
    }
    return NULL;
}

static lvglcj_disp_t *disp_of_ptr(lv_display_t *d)
{
    if (d == NULL) {
        return NULL;
    }
    return disp_of(lvglcj_handle_of(d));
}

static lvglcj_disp_t *disp_alloc_slot(void)
{
    for (int32_t i = 0; i < LVGLCJ_DISP_MAX; ++i) {
        if (!g_disp[i].used) {
            memset(&g_disp[i], 0, sizeof(g_disp[i]));
            g_disp[i].used = 1;
            g_disp[i].flush_cid = LVGLCJ_CID_NONE;
            g_disp[i].flush_wait_cid = LVGLCJ_CID_NONE;
            return &g_disp[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ trampoline */

/*
 * T4 显示刷新的实际处理体。
 * 拆出来是为了让外层 trampoline 能**在唯一出口**做性能记账 ——
 * 本函数有 4 条 return 路径，若把计时写在各分支里，迟早漏掉一条。
 */
static void flush_body(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lvglcj_disp_t *d = disp_of_ptr(disp);
    if (d == NULL) {
        /*
         * 不是经我们创建的 display（或已被删除）：仍必须让 LVGL 前进，
         * 否则刷新流程卡死。这里直接放行。
         */
        lv_display_flush_ready(disp);
        return;
    }

    /*
     * ★ (0) 本次 flush 的**行距**：必须按「本次区域」算，不能一律用 display 的 stride。
     *
     * LVGL 对三种渲染模式的行距处理不同（见 third_party/lvgl/src/core/lv_refr.c）：
     *   · PARTIAL：按 max_row 切块后，对每个子块调
     *       layer_reshape_draw_buf(layer, LV_STRIDE_AUTO)
     *     —— 用**子区域宽度**重排缓冲（lv_draw_buf_width_to_stride(区域宽, cf)）。
     *     于是区域比屏窄时行是紧凑排列的，缓冲能容纳的行数也相应变多：
     *     实测 800 宽的屏、700 宽的区域，按屏宽算的 48 行缓冲实际承载了 **54 行**。
     *   · FULL / DIRECT：传给 reshape 的是**现有 stride**，行距保持不变（= d->stride）。
     *
     * 早期实现一律把 d->stride 交给 sink，结果 PARTIAL 下窄区域被 sink 按 1600 逐行读，
     * 读到第 48 行就越过 76800 字节缓冲的末尾 —— 正是 ASan 报出的
     * heap-buffer-overflow（复现见 test/native/test_fullscreen_refresh.c）。
     * 在普通构建下它不一定会崩，只会读到相邻内存、表现为难复现的花屏。
     *
     * ★ 为什么不改用「buf_bytes / 区域高」反推行距：那只是把错误换个地方。
     *   行距是 LVGL 的**布局决定**，唯一可靠的做法是复刻它的算法 ——
     *   继续走 lv_draw_buf_width_to_stride（§3.4.2：尺寸一律走 LVGL 算子）。
     */
    /*
     * ★ area 可能为 NULL —— 只有「直接调用本 trampoline」才会这样（单测用它模拟
     *   「LVGL 触发一次刷新」而不构造真实区域）。真实 LVGL 永远传有效区域。
     *   但既然存在这条调用路径，就必须容忍：早期版本在这里无条件解引用 area，
     *   造成一个 NULL 解引用回归 —— 它先在 aarch64 上被跑到并 SIGSEGV，
     *   而 x86 侧因为改动后只跑了部分用例而没暴露（详见 docs/P0_RESULTS.md）。
     *   area 为 NULL 时我们**无从得知几何**，于是退回 display 的 stride 并跳过越界检查
     *   （无法检查就不要假装检查过）。
     */
    int32_t area_w = 0;
    int32_t area_h = 0;
    uint32_t pitch = d->stride;
    if (area != NULL) {
        area_w = area->x2 - area->x1 + 1;
        area_h = area->y2 - area->y1 + 1;
        if (d->buf_mode == LVGLCJ_BUF_PARTIAL) {
            pitch = lv_draw_buf_width_to_stride((uint32_t)area_w,
                                                (lv_color_format_t)d->color_format);
        }
    }

    /*
     * ★ 越界防护：把「静默的内存破坏」变成明确报错。
     *   sink 按 pitch 逐行读，第 h 行起点是 px_map + h×pitch，每行读 w×bpp 字节，
     *   因此要求 (h-1)×pitch + w×bpp <= buf_bytes。
     *   上面已按 LVGL 的规则复刻了 pitch，理论上必然满足；但这条不变式是
     *   「我们对 LVGL 布局的理解是否正确」的**交叉验证** ——
     *   一旦理解有偏差（就像上面那次），这里会立刻报出真实尺寸，
     *   而不是等 sink 去读越界内存。信息里带全 w/h/pitch/bpp/需求/缓冲，
     *   下次再遇到不必重新推理。
     */
    if (area_w > 0 && area_h > 0) {
        uint32_t bpp = (uint32_t)LV_COLOR_FORMAT_GET_SIZE((lv_color_format_t)d->color_format);
        uint64_t need = (uint64_t)((uint32_t)area_w * bpp) +
                        (uint64_t)(area_h - 1) * (uint64_t)pitch;
        if (need > (uint64_t)d->buf_bytes) {
            /* 日志接口固定三参数（无变参），先组装再同时交给日志与错误回调 ——
               两者共用同一条消息，避免「日志里的现象」与「错误码里的描述」不一致 */
            char detail[208];
            (void)snprintf(detail, sizeof(detail),
                           "flush 区域超出绘制缓冲：w=%d h=%d pitch=%u bpp=%u"
                           " 需要=%llu 缓冲=%u（已跳过本次上传以避免越界读）",
                           (int)area_w, (int)area_h, (unsigned)pitch, (unsigned)bpp,
                           (unsigned long long)need, (unsigned)d->buf_bytes);
            lvglcj_log(LVGLCJ_LOG_ERROR, __func__, detail);
            lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, d->handle, 0, __func__, detail);
            /* 仍然放行，否则 LVGL 会停在「等待 flush 完成」上（画面彻底不更新） */
            lv_display_flush_ready(disp);
            return;
        }
    }

    /*
     * (1) 后端 sink 优先：它负责真实的纹理上传/呈现，并**自行**决定何时
     *     flush_ready（同步后端在 sink 内放行；异步后端在渲染完成后放行）。
     *     像素指针只在这里交给 C 侧后端，绝不进入仓颉。
     */
    if (d->flush_sink != NULL) {
        /* ★ 传 pitch（本次区域的行距），不是 d->stride —— 见上面的说明 */
        d->flush_sink(d->handle, area, px_map, pitch, d->flush_sink_user);
        /* 有 sink 时不再走「无回调自动放行」分支：放行时机归 sink 掌握 */
        if (d->flush_cid == LVGLCJ_CID_NONE) {
            return;
        }
    }

    if (d->flush_cid == LVGLCJ_CID_NONE) {
        /* 既无 sink 也无回调（headless）：必须自己放行，
           否则 LVGL 停在等 flush 的状态，画面只出第一帧 */
        lv_display_flush_ready(disp);
        return;
    }

    /*
     * (2) arg = display 句柄，仓颉侧据此调用 lvglcj_display_flush_ready()
     *     —— 放行时机由用户决定（异步后端必须异步放行）。
     */
    (void)lvglcj_call_closure(d->flush_cid, d->handle);
}

/*
 * T4 显示刷新（§3.2.5）。
 * v9 第三参是 uint8_t*（不是 v8 的 lv_color_t*），px_map 指向**我们分配**的缓冲。
 * 仓颉侧只应把数据上传到后端（SDL_UpdateTexture 等），不得保存该指针。
 *
 * ★ 性能记账放在这里而不是 flush_body 里（§7.3）：
 *   一次「刷新」可能由多次 flush 组成（局部刷新），只有
 *   lv_display_flush_is_last() 为真的那一次才算一帧。
 *   把它放在唯一出口，意味着**任何**返回路径都会被正确记账 ——
 *   包括 headless 的自动放行路径，所以 FPS 在无窗口环境下同样可测。
 */
void lvglcj_flush_trampoline(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint64_t t0 = lvglcj_now_us();

    flush_body(disp, area, px_map);

    lvglcj_perf_on_flush((uint32_t)(lvglcj_now_us() - t0),
                         lv_display_flush_is_last(disp) ? 1 : 0);
}

/*
 * T4 伴生：flush 等待回调（v9.2 签名为 void(*)(lv_display_t*)，无超时参数）。
 * 超时逻辑在仓颉侧的回调里实现（§8.1.2：必须带超时，默认 100ms）。
 */
void lvglcj_flush_wait_trampoline(lv_display_t *disp)
{
    lvglcj_disp_t *d = disp_of_ptr(disp);
    if (d == NULL) {
        /* 无上下文：立即返回等价于「不等」，这是安全的一侧 */
        return;
    }

    /* 后端 sink 必须自带超时（§8.1.2） */
    if (d->flush_wait_sink != NULL) {
        d->flush_wait_sink(d->handle, d->flush_wait_sink_user);
    }

    if (d->flush_wait_cid != LVGLCJ_CID_NONE) {
        (void)lvglcj_call_closure(d->flush_wait_cid, d->handle);
    }
}

/* ------------------------------------------------------------ 后端 sink 注册 */

int32_t lvglcj_display_set_flush_sink(int64_t disp, lvglcj_flush_sink_t sink, void *user)
{
    lvglcj_disp_t *d = disp_of(disp);
    if (d == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "该句柄不是经本层创建的 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    d->flush_sink = sink;
    d->flush_sink_user = user;
    /*
     * 注册 sink 时确保 trampoline 已挂上：否则 sink 永远不会被调用，
     * 症状是「后端初始化成功但画面永远不更新」——静默且难查。
     */
    lv_display_set_flush_cb((lv_display_t *)lvglcj_ptr_of(disp),
                            lvglcj_flush_trampoline);
    return LVGLCJ_OK;
}

int32_t lvglcj_display_set_flush_wait_sink(int64_t disp, lvglcj_flush_wait_sink_t sink,
                                          void *user)
{
    lvglcj_disp_t *d = disp_of(disp);
    if (d == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "该句柄不是经本层创建的 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    d->flush_wait_sink = sink;
    d->flush_wait_sink_user = user;
    lv_display_set_flush_wait_cb((lv_display_t *)lvglcj_ptr_of(disp),
                                 lvglcj_flush_wait_trampoline);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 创建 / 删除 */

int64_t lvglcj_display_create(int32_t w, int32_t h, int32_t color_format,
                              int32_t buf_mode, int32_t buf_lines)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (w <= 0 || h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "分辨率必须为正");
        return LVGLCJ_HANDLE_NULL;
    }
    if (buf_mode != LVGLCJ_BUF_PARTIAL && buf_mode != LVGLCJ_BUF_DIRECT &&
        buf_mode != LVGLCJ_BUF_FULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "未知的缓冲模式（0=PARTIAL 1=DIRECT 2=FULL）");
        return LVGLCJ_HANDLE_NULL;
    }

    /* ★ 一律走 LVGL 的 stride 算子，绝不自己算（§3.4.2） */
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w,
                                                 (lv_color_format_t)color_format);
    if (stride == 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "颜色格式不受支持（stride 计算为 0）");
        return LVGLCJ_HANDLE_NULL;
    }

    int32_t lines = buf_lines;
    if (buf_mode == LVGLCJ_BUF_PARTIAL) {
        int32_t min_lines = (h + LVGLCJ_PARTIAL_MIN_DIVISOR - 1) / LVGLCJ_PARTIAL_MIN_DIVISOR;
        if (min_lines < 1) {
            min_lines = 1;
        }
        if (lines <= 0) {
            lines = min_lines;
        } else if (lines < min_lines) {
            /* 抬升到下限并告警：照原样传给 LVGL 会触发内部断言 */
            lvglcj_log(LVGLCJ_LOG_WARN, __func__,
                       "PARTIAL 缓冲行数小于 1/10 屏，已抬升到下限");
            lines = min_lines;
        }
        if (lines > h) {
            lines = h;
        }
    } else {
        /* DIRECT/FULL：缓冲必须整屏，buf_lines 在此被忽略（§3.4 明确规定：只 WARN） */
        if (lines > 0) {
            lvglcj_log(LVGLCJ_LOG_WARN, __func__,
                       "buf_lines 在 DIRECT/FULL 模式下被忽略（缓冲固定为整屏）");
        }
        lines = h;
    }

    uint32_t bytes = stride * (uint32_t)lines;

    lvglcj_disp_t *d = disp_alloc_slot();
    if (d == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "display 上下文槽位已满（P0 上限 8）");
        return LVGLCJ_HANDLE_NULL;
    }

    /* ★ 缓冲在这里分配：桥接层持有，生命周期与 display 绑死（ADR-002） */
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)bytes);
    if (buf == NULL) {
        d->used = 0;
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "绘制缓冲分配失败");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_display_t *dp = lv_display_create(w, h);
    if (dp == NULL) {
        free(buf);
        d->used = 0;
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "lv_display_create 失败");
        return LVGLCJ_HANDLE_NULL;
    }

    /* 先设颜色格式，再设缓冲：set_buffers 之后不应再改格式 */
    lv_display_set_color_format(dp, (lv_color_format_t)color_format);

    /* 单缓冲：buf2 = NULL。双缓冲需扩展 ABI（P1），见文件头说明 */
    lv_display_set_buffers(dp, buf, NULL, bytes,
                           (lv_display_render_mode_t)buf_mode);

    /*
     * ★ 先挂 trampoline，再登记句柄。
     *   反过来（先登记再用）在 trampoline 里查不到上下文，会走「无回调」分支
     *   而静默刷不出画面 —— 顺序必须固定。
     */
    lv_display_set_flush_cb(dp, lvglcj_flush_trampoline);

    int64_t handle = lvglcj_handle_register(dp, "lv_display_t");
    if (handle == LVGLCJ_HANDLE_NULL) {
        lv_display_delete(dp);
        free(buf);
        d->used = 0;
        return LVGLCJ_HANDLE_NULL;
    }

    d->handle = handle;
    d->buf = buf;
    d->buf_bytes = bytes;
    d->stride = stride;
    d->buf_mode = buf_mode;
    d->color_format = color_format;
    d->width = w;
    d->height = h;

    lvglcj_log(LVGLCJ_LOG_INFO, __func__, "display 已创建（缓冲由 C 侧持有）");
    return handle;
}

int32_t lvglcj_display_delete(int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    /*
     * ★ 严格校验：非 ALIVE 一律 INVALID_HANDLE（含「重复删除」）。
     *   幂等是仓颉 L1 close() 的职责，理由见 lvglcj_bridge.h §5.2 的职责划分说明。
     *   这里若写成宽容版，close() 的实现就要按子系统分支，迟早漏掉一个。
     */
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    lv_display_t *dp = (lv_display_t *)lvglcj_ptr_of(disp);
    lvglcj_disp_t *d = disp_of(disp);

    /* (a) 先注销闭包与事件登记 —— 删除 display 后就不该再有回调进来 */
    if (d != NULL) {
        if (d->flush_cid != LVGLCJ_CID_NONE) {
            lvglcj_closure_unregister(d->flush_cid);
            d->flush_cid = LVGLCJ_CID_NONE;
        }
        if (d->flush_wait_cid != LVGLCJ_CID_NONE) {
            lvglcj_closure_unregister(d->flush_wait_cid);
            d->flush_wait_cid = LVGLCJ_CID_NONE;
        }
        int32_t cids[16];
        int32_t n = lvglcj_reg_event_cids_of(disp, cids, 16);
        for (int32_t i = 0; i < n; ++i) {
            lvglcj_closure_unregister(cids[i]);
        }
        lvglcj_reg_event_drop(disp);
    }

    /*
     * (b) 先删 display，再释放缓冲 —— 顺序不可交换（§3.4）。
     *     lv_display_delete 过程中 LVGL 仍可能引用该缓冲（例如完成中的刷新），
     *     先 free 会导致 use-after-free。
     */
    lv_display_delete(dp);

    /* (c) 此时才安全释放缓冲 */
    if (d != NULL) {
        free(d->buf);
        d->buf = NULL;
        d->buf_bytes = 0;
        d->used = 0;
    }

    /* (d) 句柄推进到 RELEASED，回收表项内存（§3.1.4 步骤 2） */
    lvglcj_handle_release(disp);
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 配置 */

int32_t lvglcj_display_set_color_format(int64_t disp, int32_t fmt)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    lvglcj_disp_t *d = disp_of(disp);
    lv_display_t *dp = (lv_display_t *)lvglcj_ptr_of(disp);

    /*
     * 缓冲已按旧格式的 stride 分配完毕，改格式会使 stride 与缓冲不匹配 → 花屏。
     * 与其静默放行，不如明确拒绝并告诉用户重建 display。
     */
    if (d != NULL && d->buf != NULL && fmt != d->color_format) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "缓冲已分配，不能改颜色格式；请重建 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_display_set_color_format(dp, (lv_color_format_t)fmt);
    if (d != NULL) {
        d->color_format = fmt;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_display_get_color_format(int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    lv_display_t *dp = (lv_display_t *)lvglcj_ptr_of(disp);
    return (int32_t)lv_display_get_color_format(dp);
}

int32_t lvglcj_display_set_flush_cb(int64_t disp, int32_t cid)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    lvglcj_disp_t *d = disp_of(disp);
    if (d == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, cid, __func__,
                            "该句柄不是经本层创建的 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /* 换回调必须注销旧闭包，否则闭包表泄漏（§3.1.4 步骤 a 的同类问题） */
    if (d->flush_cid != LVGLCJ_CID_NONE && d->flush_cid != cid) {
        lvglcj_closure_unregister(d->flush_cid);
    }
    d->flush_cid = cid;

    /* 即使 cid == 0 也要挂 trampoline：它负责在无回调时自行放行 flush */
    lv_display_set_flush_cb((lv_display_t *)lvglcj_ptr_of(disp),
                            lvglcj_flush_trampoline);
    return LVGLCJ_OK;
}

int32_t lvglcj_display_set_flush_wait_cb(int64_t disp, int32_t cid)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    lvglcj_disp_t *d = disp_of(disp);
    if (d == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, cid, __func__,
                            "该句柄不是经本层创建的 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    if (d->flush_wait_cid != LVGLCJ_CID_NONE && d->flush_wait_cid != cid) {
        lvglcj_closure_unregister(d->flush_wait_cid);
    }
    d->flush_wait_cid = cid;

    lv_display_set_flush_wait_cb((lv_display_t *)lvglcj_ptr_of(disp),
                                 lvglcj_flush_wait_trampoline);
    return LVGLCJ_OK;
}

int32_t lvglcj_display_flush_ready(int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    /*
     * 不检查 LVGL 线程：flush_ready 会被仓颉侧的 flush 回调调用，
     * 而该回调可能在后端线程上被触发（异步后端）。
     * 误拦会把「正常放行」变成错误码，反而更难排查。
     */
    lv_display_t *dp = (lv_display_t *)lvglcj_ptr_of(disp);
    if (dp == NULL) {
        LVGLCJ_HANDLE_GUARD(disp, __func__);
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_display_flush_ready(dp);
    return LVGLCJ_OK;
}

int32_t lvglcj_display_flush_is_last(int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_HANDLE_GUARD(disp, __func__);
    return lv_display_flush_is_last((lv_display_t *)lvglcj_ptr_of(disp)) ? 1 : 0;
}

int32_t lvglcj_display_set_rotation(int64_t disp, int32_t rot)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    if (rot < 0 || rot > 3) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "旋转值必须是 0/1/2/3");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_display_set_rotation((lv_display_t *)lvglcj_ptr_of(disp),
                            (lv_display_rotation_t)rot);
    return LVGLCJ_OK;
}

int32_t lvglcj_display_set_resolution(int64_t disp, int32_t w, int32_t h)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    if (w <= 0 || h <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "分辨率必须为正");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lvglcj_disp_t *d = disp_of(disp);
    if (d != NULL && d->buf != NULL && (w != d->width || h != d->height)) {
        /*
         * 与 color_format 同理：缓冲大小由分辨率决定，
         * 改了分辨率而缓冲不变，LVGL 会按新分辨率写入旧缓冲 → 越界写。
         */
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "缓冲已分配，不能改分辨率；请重建 display");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_display_set_resolution((lv_display_t *)lvglcj_ptr_of(disp), w, h);
    if (d != NULL) {
        d->width = w;
        d->height = h;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(disp, __func__);

    /* §3.7.3：绘制类事件不支持（自绘走 Canvas 控件，§3.11） */
    if (code == LV_EVENT_DRAW_MAIN || code == LV_EVENT_DRAW_MAIN_BEGIN ||
        code == LV_EVENT_DRAW_MAIN_END || code == LV_EVENT_DRAW_POST ||
        code == LV_EVENT_DRAW_POST_BEGIN || code == LV_EVENT_DRAW_POST_END) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, disp, cid, __func__,
                            "LV_EVENT_DRAW_* 不支持（自绘请用 Canvas 控件，P2 落地）");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    /*
     * lv_display_add_event_cb 返回 void，拿不到 dsc；
     * 注册表允许 dsc == NULL（lvglcj_reg_event_remove_by_dsc 对 NULL 直接返回），
     * 因此用 NULL 登记 cid，删除 display 时靠 reg_event_drop 统一清理。
     */
    lv_display_add_event_cb((lv_display_t *)lvglcj_ptr_of(disp),
                            lvglcj_event_trampoline,
                            (lv_event_code_t)code, lvglcj_cid_to_ptr(cid));
    return lvglcj_reg_event_add(disp, NULL, cid);
}

/* ------------------------------------------------------------ 只读信息 */

int64_t lvglcj_display_get_buf_bytes(int64_t disp)
{
    lvglcj_disp_t *d = disp_of(disp);
    return (d != NULL && d->used) ? (int64_t)d->buf_bytes : -1;
}

int32_t lvglcj_display_get_buf_stride(int64_t disp)
{
    lvglcj_disp_t *d = disp_of(disp);
    return (d != NULL && d->used) ? (int32_t)d->stride : -1;
}

/*
 * 测试支持：对绘制缓冲做一次像素级体检。
 *
 * 为什么需要它：仓颉侧**看不到**绘制缓冲（ADR-002 刻意如此），
 * 因此「画面是否正确」无法从仓颉侧直接验证。而只断言「flush 被调用过」
 * 又不足以排除「整帧刷的是一块黑」——那同样是 flush 被调用了。
 *
 * 这个函数让 C 侧代替仓颉侧做体检，且**不暴露指针**：
 *   distinct 输出采样到的不同像素值个数（上限 64）→ 排除「纯色/花屏」
 *   nonzero  输出非零像素个数                        → 排除「全黑」
 * 采样是跨步的（最多约 4096 个点），因此对 800×480 也是一次廉价操作。
 */
int64_t lvglcj_display_sample_buf(int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return -1;
    }
    if (lvglcj_check_lvgl_thread(__func__) != LVGLCJ_OK) {
        return -1;
    }
    if (lvglcj_handle_require(disp, __func__) != LVGLCJ_OK) {
        return -1;
    }

    lvglcj_disp_t *d = disp_of(disp);
    if (d == NULL || d->buf == NULL || d->buf_bytes == 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "该 display 没有由本层持有的绘制缓冲");
        return -1;
    }

    uint32_t px_size = lv_color_format_get_size((lv_color_format_t)d->color_format);
    if (px_size == 0 || px_size > 4) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, disp, 0, __func__,
                            "无法判定像素字节数");
        return -1;
    }

    int32_t total_px = (int32_t)(d->buf_bytes / px_size);
    int32_t step = total_px / 4096;
    if (step < 1) {
        step = 1;
    }

    uint32_t uniq[64];
    int32_t n_uniq = 0;
    int32_t n_nonzero = 0;

    for (int32_t i = 0; i < total_px; i += step) {
        const uint8_t *p = d->buf + (size_t)i * px_size;
        uint32_t v = 0;
        for (uint32_t b = 0; b < px_size; ++b) {
            v |= ((uint32_t)p[b]) << (8u * b);
        }
        if (v != 0) {
            n_nonzero++;
        }
        bool seen = false;
        for (int32_t k = 0; k < n_uniq; ++k) {
            if (uniq[k] == v) {
                seen = true;
                break;
            }
        }
        if (!seen && n_uniq < 64) {
            uniq[n_uniq++] = v;
        }
    }

    /*
     * ★ 打包返回而不是用两个出参：
     *   出参在仓颉侧要构造 CPointer 并做指针写入，容易引入指针算法错误；
     *   而这两个值都是非负计数，打包进 int64 后「负数 = 出错」仍然无歧义。
     *   高 32 位 = distinct，低 32 位 = nonzero。
     */
    uint64_t packed = ((uint64_t)(uint32_t)n_uniq << 32) | (uint64_t)(uint32_t)n_nonzero;
    return (int64_t)packed;
}
