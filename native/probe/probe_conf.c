/*
 * probe_conf.c —— 导出编译期配置与 FFI 布局（设计文档 §3.5.2 + §10.2）
 *
 * 输出一份 JSON 到 stdout，由 scripts/gen_conf_const.py 消费，
 * 生成 src/generated/conf_const.cj。仓颉侧据此：
 *   1. 在启动时校验配置哈希（§3.5.3，不一致即抛 InvalidConfig）
 *   2. 在单测里断言 C/仓颉两侧的结构体布局一致（§10.2 FFI 契约测试）
 *
 * 用 JSON 而不是直接把值写进 .cj：
 *   构建流程解耦 —— 换 LVGL 配置只需重跑探针与生成器，不用改 C 代码。
 */
#include "lvglcj_bridge.h"
#include "lvglcj_probe.h"

#include <stdio.h>

int main(void)
{
    lvglcj_offsets_t off;
    int32_t rc = lvglcj_probe_offsets(&off);

    printf("{\n");
    printf("  \"lvgl_version_string\": \"%s\",\n", lvglcj_version());
    printf("  \"lvgl_version_major\": %d,\n", lvglcj_conf_version_major());
    printf("  \"lvgl_version_minor\": %d,\n", lvglcj_conf_version_minor());
    printf("  \"lvgl_version_patch\": %d,\n", lvglcj_conf_version_patch());
    printf("  \"conf_hash\": %u,\n", lvglcj_conf_hash());
    printf("  \"color_depth\": %d,\n", lvglcj_conf_color_depth());
    printf("  \"use_log\": %d,\n", lvglcj_conf_use_log());
    printf("  \"mem_size\": %d,\n", lvglcj_conf_mem_size());
    printf("  \"stride_align\": %d,\n", lvglcj_conf_stride_align());
    printf("  \"use_fs_posix\": %d,\n", lvglcj_conf_use_fs_posix());
    printf("  \"def_refr_period\": %d,\n", lvglcj_conf_def_refr_period());

    /* ---- FFI 布局契约（§10.2）：仓颉侧用 @C struct + sizeOf/alignOf 比对 ---- */
    printf("  \"offsets\": {\n");
    printf("    \"x1_offset\": %d,\n", off.x1_offset);
    printf("    \"y1_offset\": %d,\n", off.y1_offset);
    printf("    \"x2_offset\": %d,\n", off.x2_offset);
    printf("    \"y2_offset\": %d,\n", off.y2_offset);
    printf("    \"sizeof_area\": %d,\n", off.sizeof_area);
    printf("    \"sizeof_indev_data\": %d,\n", off.sizeof_indev_data);
    printf("    \"sizeof_perf\": %d,\n", off.sizeof_perf);
    printf("    \"sizeof_error_ctx\": %d,\n", off.sizeof_error_ctx);
    printf("    \"sizeof_tree_dump\": %d\n", off.sizeof_tree_dump);
    printf("  },\n");

    /* ---- 错误码表：两侧必须逐项一致（附录 B 冻结） ---- */
    printf("  \"status_codes\": {\n");
    printf("    \"OK\": %d,\n", LVGLCJ_OK);
    printf("    \"OK_DEFERRED\": %d,\n", LVGLCJ_OK_DEFERRED);
    printf("    \"ERR_INVALID_HANDLE\": %d,\n", LVGLCJ_ERR_INVALID_HANDLE);
    printf("    \"ERR_NOT_INITIALIZED\": %d,\n", LVGLCJ_ERR_NOT_INITIALIZED);
    printf("    \"ERR_INVALID_CONFIG\": %d,\n", LVGLCJ_ERR_INVALID_CONFIG);
    printf("    \"ERR_CALLBACK_THREW\": %d,\n", LVGLCJ_ERR_CALLBACK_THREW);
    printf("    \"ERR_OUT_OF_MEMORY\": %d,\n", LVGLCJ_ERR_OUT_OF_MEMORY);
    printf("    \"ERR_WRONG_THREAD\": %d,\n", LVGLCJ_ERR_WRONG_THREAD);
    printf("    \"ERR_DEADLOCK_RISK\": %d,\n", LVGLCJ_ERR_DEADLOCK_RISK);
    printf("    \"ERR_QUEUE_FULL\": %d,\n", LVGLCJ_ERR_QUEUE_FULL);
    printf("    \"ERR_BACKEND_FAILURE\": %d,\n", LVGLCJ_ERR_BACKEND_FAILURE);
    printf("    \"ERR_INVALID_ARGUMENT\": %d,\n", LVGLCJ_ERR_INVALID_ARGUMENT);
    printf("    \"ERR_CLOSURE_EXHAUSTED\": %d,\n", LVGLCJ_ERR_CLOSURE_EXHAUSTED);
    printf("    \"ERR_PENDING_DELETE\": %d,\n", LVGLCJ_ERR_PENDING_DELETE);
    printf("    \"ERR_NOT_SUPPORTED\": %d,\n", LVGLCJ_ERR_NOT_SUPPORTED);
    printf("    \"ERR_VERSION_MISMATCH\": %d,\n", LVGLCJ_ERR_VERSION_MISMATCH);
    printf("    \"ERR_DEFERRED_LOOP\": %d,\n", LVGLCJ_ERR_DEFERRED_LOOP);
    printf("    \"ERR_PIN_EXHAUSTED\": %d,\n", LVGLCJ_ERR_PIN_EXHAUSTED);
    printf("    \"ERR_ANIM_CTX_LOST\": %d\n", LVGLCJ_ERR_ANIM_CTX_LOST);
    printf("  },\n");

    /* ---- 句柄四态（§3.1.1 冻结） ---- */
    printf("  \"handle_states\": {\n");
    printf("    \"UNINIT\": %d,\n", LVGLCJ_HSTATE_UNINIT);
    printf("    \"ALIVE\": %d,\n", LVGLCJ_HSTATE_ALIVE);
    printf("    \"INVALIDATED\": %d,\n", LVGLCJ_HSTATE_INVALIDATED);
    printf("    \"RELEASED\": %d\n", LVGLCJ_HSTATE_RELEASED);
    printf("  },\n");

    printf("  \"probe_offsets_rc\": %d\n", rc);
    printf("}\n");

    return 0;
}
