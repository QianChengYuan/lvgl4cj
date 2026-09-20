/*
 * test_null_backend.c —— C 侧单测：headless 后端（对应探针结论 G1）
 *
 * 本用例存在的原因是 G1 这条**硬前置**：
 *   无 display 时 `lv_screen_active()` 返回 NULL，而 `lv_obj_create(NULL)`
 *   会触发 LVGL 断言并**直接终止进程**。
 *   所以「必须先有 display」不是风格问题，而是能不能建对象的前提。
 *
 * 因此这里按顺序验证：
 *   1. 初始化前：screen_active() 为 0（可安全检测，不触发断言）
 *   2. headless 初始化后：screen_active() 有效 → 对象可以创建
 *   3. headless 下 flush **仍然经过 T4 trampoline**（不是「没窗口就跳过回调」）
 *   4. pump(n) 能确定性推进帧并让刷新周期到期
 *   5. 反初始化幂等，且之后句柄确实回收
 *
 * 用 CTest 注册：bash scripts/build_native.sh --tests
 */
#include "lvglcj_internal.h"
#include "lvglcj_backend_null.h"
#include "callback.h"

#include <stdio.h>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        g_total++;                                    \
        if (cond) {                                   \
            printf("  [ok]   %s\n", (msg));           \
        } else {                                      \
            printf("  [FAIL] %s\n", (msg));            \
            g_fail++;                                 \
        }                                             \
    } while (0)

#define CID_FLUSH 1

static int     g_flush_calls = 0;
static int64_t g_flush_arg = -1;

static int32_t stub_dispatch(int32_t cid, int64_t arg)
{
    if (cid == CID_FLUSH) {
        g_flush_calls++;
        g_flush_arg = arg;
        lvglcj_display_flush_ready(arg); /* 真实后端在此上传后放行 */
    }
    return LVGLCJ_OK;
}

int main(void)
{
    printf("=== test_null_backend：headless 后端与 G1 前置 ===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    lvglcj_set_dispatch(stub_dispatch);

    /* ============================================== 1. G1：无 display 时屏幕为空 */
    printf("\n-- 1. G1 前置：无 display 时没有活动屏幕 --\n");
    /*
     * ★ 这里只断言 screen_active() 为 0，**不能**去调 lvglcj_obj_create(0) 试探：
     *   那会触发 LVGL 断言并直接终止本测试进程（G1 实测结论）。
     *   用「能不能安全地问出来」代替「撞上去看看」，是这类前置的正确测法。
     */
    CHECK(lvglcj_screen_active() == LVGLCJ_HANDLE_NULL,
          "★ 未建 display 时 screen_active() 为 0（这正是不能建对象的原因）");

    /* ============================================== 2. headless 初始化 */
    printf("\n-- 2. headless 初始化 --\n");
    CHECK(lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) == LVGLCJ_OK,
          "headless 初始化成功");
    CHECK(lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) == LVGLCJ_OK,
          "重复初始化幂等");

    int64_t disp = lvglcj_null_display();
    CHECK(disp != LVGLCJ_HANDLE_NULL, "取得 headless display 句柄");

    int64_t scr = lvglcj_screen_active();
    CHECK(scr != LVGLCJ_HANDLE_NULL,
          "★ 初始化后 screen_active() 有效（G1 前置已满足）");

    /* ============================================== 3. 可以正常建对象 */
    printf("\n-- 3. 建对象（G1 的实际目的）--\n");
    int64_t obj = lvglcj_obj_create(scr);
    CHECK(obj != LVGLCJ_HANDLE_NULL, "在 headless 屏幕上创建对象成功");
    lvglcj_obj_set_pos(obj, 5, 5);
    lvglcj_obj_set_size(obj, 40, 30);

    int64_t child = lvglcj_obj_create(obj);
    CHECK(child != LVGLCJ_HANDLE_NULL, "创建子对象成功（对象树可用）");
    CHECK(lvglcj_obj_get_child_count(obj) == 1, "子对象计数为 1");

    /* ============================================== 4. flush 仍走 T4 全链路 */
    printf("\n-- 4. headless 下 flush 仍经过 T4 trampoline --\n");
    CHECK(lvglcj_display_set_flush_cb(disp, CID_FLUSH) == LVGLCJ_OK,
          "注册 flush 回调（headless 也占用正常 cid 机制）");

    g_flush_calls = 0;
    int32_t prc = lvglcj_null_pump(30); /* 30 帧 × 5ms ≈ 150ms，足够跨过刷新周期 */
    CHECK(prc == LVGLCJ_OK, "pump(30) 正常返回");
    CHECK(g_flush_calls > 0, "★ headless 下 flush 回调被真的调用（非「无窗口即跳过」）");
    CHECK(g_flush_arg == disp, "★ flush 收到的 arg 是 display 句柄");

    /* ============================================== 5. pump 的边界与语义 */
    printf("\n-- 5. pump 语义 --\n");
    CHECK(lvglcj_null_pump(0) == LVGLCJ_OK, "pump(0) 合法（推进 0 帧）");
    CHECK(lvglcj_null_pump(-1) == LVGLCJ_ERR_INVALID_ARGUMENT, "pump(-1) 被拒");

    /* ============================================== 6. 反初始化 */
    printf("\n-- 6. 反初始化 --\n");
    CHECK(lvglcj_obj_delete(obj) == LVGLCJ_OK, "删除父对象（级联子对象）");
    CHECK(lvglcj_handle_state(child) == LVGLCJ_HSTATE_INVALIDATED,
          "★ 子句柄因父删除而 INVALIDATED（四态语义在 headless 下一致）");

    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化成功");
    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "重复反初始化幂等");
    CHECK(lvglcj_null_display() == LVGLCJ_HANDLE_NULL, "反初始化后句柄记录已清空");
    CHECK(lvglcj_handle_state(disp) == LVGLCJ_HSTATE_UNINIT,
          "display 句柄表项已回收");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_set_dispatch(NULL);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
