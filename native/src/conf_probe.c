/*
 * conf_probe.c —— 编译期宏 → 运行时查询函数（设计文档 §3.5）
 *
 * 为什么需要这一层：
 *   lv_conf.h 里的 LV_* 宏是**编译期**常量，仓颉侧读不到。把它们暴露成查询函数，
 *   再在启动时比对哈希（§3.5.3），就能把「换机器构建导致配置不一致」这类
 *   隐蔽的花屏/崩溃，前置为一条明确的启动报错（ADR-008）。
 */
#include "lvglcj_bridge.h"

#include <string.h>
#include <time.h>

#include "lv_version.h"

/* ------------------------------------------------------------------ 版本串 */
#define LVGLCJ_STR_(x) #x
#define LVGLCJ_STR(x) LVGLCJ_STR_(x)

static const char LVGLCJ_VERSION_STR[] =
    LVGLCJ_STR(LVGL_VERSION_MAJOR) "." LVGLCJ_STR(LVGL_VERSION_MINOR) "." LVGLCJ_STR(LVGL_VERSION_PATCH)
    " (lvgl4cj)";

const char *lvglcj_version(void)
{
    return LVGLCJ_VERSION_STR;
}

/* ------------------------------------------------------------ 配置查询函数 */
int32_t lvglcj_conf_color_depth(void)
{
    return (int32_t)LV_COLOR_DEPTH;
}

int32_t lvglcj_conf_use_log(void)
{
#if LV_USE_LOG
    return 1;
#else
    return 0;
#endif
}

int32_t lvglcj_conf_mem_size(void)
{
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    return (int32_t)(LV_MEM_SIZE);
#else
    /* 使用系统分配器时无固定池大小 */
    return 0;
#endif
}

int32_t lvglcj_conf_stride_align(void)
{
#ifdef LV_DRAW_BUF_STRIDE_ALIGN
    return (int32_t)LV_DRAW_BUF_STRIDE_ALIGN;
#else
    return 1;
#endif
}

int32_t lvglcj_conf_use_fs_posix(void)
{
#if LV_USE_FS_POSIX
    return 1;
#else
    return 0;
#endif
}

int32_t lvglcj_conf_def_refr_period(void)
{
    return (int32_t)LV_DEF_REFR_PERIOD;
}

int32_t lvglcj_conf_version_major(void) { return (int32_t)LVGL_VERSION_MAJOR; }
int32_t lvglcj_conf_version_minor(void) { return (int32_t)LVGL_VERSION_MINOR; }
int32_t lvglcj_conf_version_patch(void) { return (int32_t)LVGL_VERSION_PATCH; }

/* ------------------------------------------------------------------ 配置哈希 */
/*
 * FNV-1a：把「会影响 ABI / 内存布局 / 渲染结果」的编译期配置混进一个 32 位哈希。
 * 仓颉侧持有构建时由 scripts/gen_conf_const.py 写入的常量，
 * 启动时不一致即抛 InvalidConfig（§3.5.3）。
 */
uint32_t lvglcj_conf_hash(void)
{
    uint32_t h = 2166136261u;

#define LVGLCJ_HASH_MIX(v)                 \
    do {                                   \
        h ^= (uint32_t)(v);                \
        h *= 16777619u;                    \
    } while (0)

    LVGLCJ_HASH_MIX(LV_COLOR_DEPTH);
    LVGLCJ_HASH_MIX(LV_USE_LOG);
    LVGLCJ_HASH_MIX(LV_DRAW_BUF_STRIDE_ALIGN);
    LVGLCJ_HASH_MIX(LV_USE_FS_POSIX);
    LVGLCJ_HASH_MIX(LV_USE_OS);
    LVGLCJ_HASH_MIX(LV_USE_STDLIB_MALLOC);
    LVGLCJ_HASH_MIX(LV_DEF_REFR_PERIOD);
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    LVGLCJ_HASH_MIX(LV_MEM_SIZE);
#endif
    LVGLCJ_HASH_MIX(LVGL_VERSION_MAJOR);
    LVGLCJ_HASH_MIX(LVGL_VERSION_MINOR);
    LVGLCJ_HASH_MIX(LVGL_VERSION_PATCH);

#undef LVGLCJ_HASH_MIX

    return h;
}

/* ------------------------------------------------------------------ tick */
/*
 * tick 由 C 侧接管（不依赖仓颉时钟），用 CLOCK_MONOTONIC：
 * 单调递增、不受系统时间调整影响，适合 LVGL 的定时/动画计时。
 */
static uint32_t lvglcj_tick_cb_internal(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

int32_t lvglcj_set_tick_cb(void)
{
    lv_tick_set_cb(lvglcj_tick_cb_internal);
    return LVGLCJ_OK;
}

uint32_t lvglcj_tick_get(void)
{
    return lv_tick_get();
}

uint32_t lvglcj_tick_elaps(uint32_t prev)
{
    return lv_tick_elaps(prev);
}

/* ------------------------------------------------------ FFI 契约测试支持 */
int32_t lvglcj_probe_offsets(lvglcj_offsets_t *out)
{
    if (out == NULL) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    lv_area_t area;
    memset(&area, 0, sizeof(area));

    out->x1_offset = (int32_t)((char *)&area.x1 - (char *)&area);
    out->y1_offset = (int32_t)((char *)&area.y1 - (char *)&area);
    out->x2_offset = (int32_t)((char *)&area.x2 - (char *)&area);
    out->y2_offset = (int32_t)((char *)&area.y2 - (char *)&area);
    out->sizeof_area = (int32_t)sizeof(lv_area_t);

    out->sizeof_indev_data = (int32_t)sizeof(lv_indev_data_t);
    out->sizeof_perf = (int32_t)sizeof(lvglcj_perf_t);
    out->sizeof_error_ctx = (int32_t)sizeof(lvglcj_error_t);
    out->sizeof_tree_dump = (int32_t)sizeof(lvglcj_tree_dump_t);

    return LVGLCJ_OK;
}
