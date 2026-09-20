/*
 * test_display_indev.c —— C 侧单测：display（§5.3/§3.4）与 indev/group（§5.4）
 *
 * 重点验证的是**那些一旦做错就会静默出错**的不变量：
 *   · ADR-002：绘制缓冲由 C 侧分配并持有，尺寸取自 LVGL 的 stride 算子
 *   · PARTIAL 的 1/10 屏下限会被抬升（而不是把非法尺寸交给 LVGL）
 *   · DIRECT/FULL 下 buf_lines 被忽略且缓冲为整屏
 *   · T4 flush trampoline 会被真的触发；且**无回调时自动放行**（否则画面只出第一帧）
 *   · 缓冲已分配后拒绝改 color_format / 分辨率（否则 stride 不匹配 → 花屏/越界）
 *   · T5：read 回调内可通过 setter 回填；**回调外调用返回 NOT_SUPPORTED**
 *   · T5：无 read 回调时 data 被复位为 RELEASED（否则出现「松开仍按下」）
 *   · indev 类型枚举已逐值对齐 LVGL（BUTTON=3 / ENCODER=4）
 *
 * 用 CTest 注册：bash scripts/build_native.sh --tests
 */
#include "lvglcj_internal.h"
#include "callback.h"

#include <stdio.h>
#include <unistd.h>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        g_total++;                                    \
        if (cond) {                                   \
            printf("  [ok]   %s\n", (msg));           \
        } else {                                      \
            printf("  [FAIL] %s\n", (msg));           \
            g_fail++;                                 \
        }                                             \
    } while (0)

/* ------------------------------------------------------------------ 分发桩 */
#define CID_FLUSH      1
#define CID_FLUSHWAIT  2
#define CID_READ       3

static int g_flush_calls = 0;
static int g_flush_wait_calls = 0;
static int g_read_calls = 0;
static int64_t g_flush_arg = -1;

/* read 回调里打算写入的值由测试设置 */
static int32_t g_want_x = 0, g_want_y = 0, g_want_pressed = 0;

static int32_t stub_dispatch(int32_t cid, int64_t arg)
{
    switch (cid) {
        case CID_FLUSH:
            g_flush_calls++;
            g_flush_arg = arg;
            /*
             * 真实后端在这里把缓冲上传，然后放行。
             * 测我们自己的 trampoline 链路时必须真的放行，
             * 否则 LVGL 会一直认为刷新未完成。
             */
            lvglcj_display_flush_ready(arg);
            return LVGLCJ_OK;
        case CID_FLUSHWAIT:
            g_flush_wait_calls++;
            return LVGLCJ_OK;
        case CID_READ:
            g_read_calls++;
            /* ★ 只能借 setter 回填，不能直接碰 lv_indev_data_t */
            lvglcj_indev_set_point(g_want_x, g_want_y, g_want_pressed);
            return LVGLCJ_OK;
        default:
            return LVGLCJ_OK;
    }
}

/* -------------------------------------------------------------------- main */
int main(void)
{
    /*
     * ★ 关掉 stdout 缓冲。
     *
     * 不是为了「看到进度」这种舒适性 —— 而是**崩溃定位的硬需求**：
     * 输出重定向到文件/管道时 stdout 是块缓冲，进程若在中途崩溃
     * （段错误、ASan 中止），最后一整块输出（最多 4KB，远超本用例全部输出）
     * 会随进程一起丢失，于是「崩在哪个断言之后」完全不可知。
     * 这一点在 aarch64 交叉验证时踩到过：qemu 下的段错误只留下
     * 两行来自 stderr 的日志，printf 的 70 条检查全部丢失。
     * 单测本身输出量很小，取消缓冲的代价可以忽略。
     */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== test_display_indev：display 与 indev/group ===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    lvglcj_set_dispatch(stub_dispatch);

    /* ================================================== 1. 枚举取值对齐 LVGL */
    printf("\n-- 1. indev 枚举取值（必须与 LVGL 逐值一致）--\n");
    /*
     * 两侧都是匿名枚举，直接比较会触发 -Wenum-compare；
     * 这里比的是**数值**（这正是本用例要保证的东西），因此显式取 int。
     */
    CHECK((int)LVGLCJ_INDEV_NONE == (int)LV_INDEV_TYPE_NONE, "NONE == LVGL 0");
    CHECK((int)LVGLCJ_INDEV_POINTER == (int)LV_INDEV_TYPE_POINTER, "POINTER == LVGL 1");
    CHECK((int)LVGLCJ_INDEV_KEYPAD == (int)LV_INDEV_TYPE_KEYPAD, "KEYPAD == LVGL 2");
    CHECK((int)LVGLCJ_INDEV_BUTTON == (int)LV_INDEV_TYPE_BUTTON,
          "★ BUTTON == LVGL 3（早期版本此处与 ENCODER 写反过）");
    CHECK((int)LVGLCJ_INDEV_ENCODER == (int)LV_INDEV_TYPE_ENCODER,
          "★ ENCODER == LVGL 4（早期版本此处与 BUTTON 写反过）");

    /* ================================================== 2. display 创建与缓冲 */
    printf("\n-- 2. display 创建：缓冲由 C 侧持有（ADR-002）--\n");
    int64_t disp = lvglcj_display_create(320, 240, LV_COLOR_FORMAT_RGB565,
                                         LVGLCJ_BUF_PARTIAL, 0);
    CHECK(disp != LVGLCJ_HANDLE_NULL, "display 创建成功（PARTIAL, buf_lines=0 → 自动）");

    uint32_t expect_stride = lv_draw_buf_width_to_stride(320, LV_COLOR_FORMAT_RGB565);
    CHECK(lvglcj_display_get_buf_stride(disp) == (int32_t)expect_stride,
          "★ stride 来自 lv_draw_buf_width_to_stride（未自行计算）");

    int64_t bytes = lvglcj_display_get_buf_bytes(disp);
    CHECK(bytes > 0, "缓冲已由 C 侧分配（字节数 > 0）");

    /* PARTIAL 且 buf_lines=0：应抬升到 1/10 屏 = 24 行 */
    int32_t min_lines = (240 + 9) / 10;
    CHECK(bytes == (int64_t)expect_stride * min_lines,
          "★ 未指定 buf_lines 时取 1/10 屏下限（§3.4 建议值）");

    CHECK(lvglcj_display_get_color_format(disp) == LV_COLOR_FORMAT_RGB565,
          "颜色格式为 RGB565");

    /* ================================================== 3. PARTIAL 下限抬升 */
    printf("\n-- 3. PARTIAL：低于 1/10 屏的 buf_lines 被抬升 --\n");
    int64_t d2 = lvglcj_display_create(320, 240, LV_COLOR_FORMAT_RGB565,
                                       LVGLCJ_BUF_PARTIAL, 2 /* 远小于 24 */);
    CHECK(d2 != LVGLCJ_HANDLE_NULL, "dialog 效果：低 buf_lines 仍能创建（不报错）");
    CHECK(lvglcj_display_get_buf_bytes(d2) == (int64_t)expect_stride * min_lines,
          "★ 被抬升到 1/10 屏（而不是把非法尺寸交给 LVGL）");

    /* ================================================== 4. DIRECT：整屏且忽略 buf_lines */
    printf("\n-- 4. DIRECT：缓冲整屏，buf_lines 被忽略 --\n");
    int64_t d3 = lvglcj_display_create(320, 240, LV_COLOR_FORMAT_RGB565,
                                       LVGLCJ_BUF_DIRECT, 7 /* 应被忽略 */);
    CHECK(d3 != LVGLCJ_HANDLE_NULL, "DIRECT 模式创建成功");
    CHECK(lvglcj_display_get_buf_bytes(d3) == (int64_t)expect_stride * 240,
          "★ DIRECT 下缓冲为整屏（buf_lines 被忽略，仅记 WARN）");

    /* ================================================== 5. 拒绝会破坏 stride 的改动 */
    printf("\n-- 5. 缓冲已分配后拒绝改格式/分辨率（防花屏与越界写）--\n");
    CHECK(lvglcj_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888) ==
              LVGLCJ_ERR_INVALID_ARGUMENT,
          "★ 改 color_format 被拒（stride 会变）");
    CHECK(lvglcj_display_set_resolution(disp, 640, 480) ==
              LVGLCJ_ERR_INVALID_ARGUMENT,
          "★ 改分辨率被拒（缓冲大小会变）");
    /* 同值设置应放行（不是无脑拒绝） */
    CHECK(lvglcj_display_set_resolution(disp, 320, 240) == LVGLCJ_OK,
          "同值设置分辨率被允许");

    /* ================================================== 6. T4 flush 真实路径 */
    printf("\n-- 6. T4 flush trampoline：真实渲染触发 --\n");
    CHECK(lvglcj_display_set_flush_cb(disp, CID_FLUSH) == LVGLCJ_OK, "注册 flush 回调");
    CHECK(lvglcj_display_set_flush_wait_cb(disp, CID_FLUSHWAIT) == LVGLCJ_OK,
          "注册 flush_wait 回调");

    /*
     * 建一个可见对象，制造真实的刷新需求。
     * 用 lvglcj_obj_create 而不是控件：控件（label/button）属 t6，
     * 本用例只验证 display/indev 链路，不应依赖尚未实现的子系统。
     */
    int64_t scr = lvglcj_screen_active();
    int64_t box = lvglcj_obj_create(scr);
    CHECK(box != LVGLCJ_HANDLE_NULL, "在活动屏幕上创建对象（制造刷新需求）");
    lvglcj_obj_set_pos(box, 10, 10);
    lvglcj_obj_set_size(box, 50, 50);

    g_flush_calls = 0;
    for (int i = 0; i < 60 && g_flush_calls == 0; ++i) {
        usleep(5000); /* 等刷新周期（默认 30ms）到期 */
        lvglcj_timer_handler();
    }
    CHECK(g_flush_calls > 0, "★ flush 回调被真的调用（经 T4 trampoline）");
    CHECK(g_flush_arg == disp, "★ flush 回调收到的 arg 就是 display 句柄");
    CHECK(lvglcj_error_count(LVGLCJ_ERR_CALLBACK_THREW) == 0,
          "flush 链路未产生 CALLBACK_THREW");

    /* 无回调的 display：trampoline 必须自己放行，否则刷新卡死 */
    printf("\n-- 6b. 未注册 flush 回调时必须自动放行 --\n");
    {
        int64_t d4 = lvglcj_display_create(64, 64, LV_COLOR_FORMAT_RGB565,
                                           LVGLCJ_BUF_PARTIAL, 0);
        CHECK(d4 != LVGLCJ_HANDLE_NULL, "创建无 flush 回调的 display");
        int before = g_flush_calls;
        /* 直接调用 trampoline：等价于 LVGL 触发刷新 */
        lvglcj_flush_trampoline((lv_display_t *)lvglcj_ptr_of(d4), NULL, NULL);
        CHECK(g_flush_calls == before,
              "★ 无回调时不进 dispatch，而是自行 flush_ready（避免卡在等刷新）");
        CHECK(lvglcj_display_delete(d4) == LVGLCJ_OK, "删除该 display");
        CHECK(lvglcj_handle_state(d4) == LVGLCJ_HSTATE_UNINIT,
              "删除后表项被回收（state 查询为 UNINIT）");
        CHECK(lvglcj_display_delete(d4) == LVGLCJ_ERR_INVALID_HANDLE,
              "★ 重复删除返回 INVALID_HANDLE（幂等由仓颉 close() 负责）");
    }

    /* ================================================== 7. T5 indev 回填 */
    printf("\n-- 7. T5 indev：read 回调内 setter 回填 --\n");
    int64_t iv = lvglcj_indev_create(LVGLCJ_INDEV_POINTER);
    CHECK(iv != LVGLCJ_HANDLE_NULL, "创建 POINTER indev");
    CHECK(lvglcj_indev_set_read_cb(iv, CID_READ) == LVGLCJ_OK, "注册 read 回调");
    CHECK(lvglcj_indev_set_display(iv, disp) == LVGLCJ_OK, "绑定 display");

    /* ★ 回调外调用 setter 必须被拒（没有「当前正在读取的设备」） */
    CHECK(lvglcj_indev_set_point(1, 2, 1) == LVGLCJ_ERR_NOT_SUPPORTED,
          "★ read 回调之外调用 setter 返回 NOT_SUPPORTED");

    lv_indev_data_t data;
    g_want_x = 11;
    g_want_y = 22;
    g_want_pressed = 1;
    g_read_calls = 0;
    lvglcj_indev_read_trampoline((lv_indev_t *)lvglcj_ptr_of(iv), &data);
    CHECK(g_read_calls == 1, "read 回调被调用");
    CHECK(data.point.x == 11 && data.point.y == 22,
          "★ setter 写入的坐标经 trampoline 落入 lv_indev_data_t");
    CHECK(data.state == LV_INDEV_STATE_PRESSED, "★ 按下状态正确");

    /* 释放：松开 */
    g_want_pressed = 0;
    lvglcj_indev_read_trampoline((lv_indev_t *)lvglcj_ptr_of(iv), &data);
    CHECK(data.state == LV_INDEV_STATE_RELEASED, "松开后状态为 RELEASED");

    /* ★ 无 read 回调时必须复位为 RELEASED，而不是保留上一轮残留 */
    printf("\n-- 7b. 无 read 回调时 data 被复位 --\n");
    {
        int64_t iv2 = lvglcj_indev_create(LVGLCJ_INDEV_KEYPAD);
        CHECK(iv2 != LVGLCJ_HANDLE_NULL, "创建 KEYPAD indev");
        lv_indev_data_t d2;
        d2.state = LV_INDEV_STATE_PRESSED; /* 故意塞入脏值 */
        d2.point.x = 999;
        d2.key = 42;
        lvglcj_indev_read_trampoline((lv_indev_t *)lvglcj_ptr_of(iv2), &d2);
        CHECK(d2.state == LV_INDEV_STATE_RELEASED,
              "★ 无回调时复位为 RELEASED（否则会出现「松开仍按下」）");
        CHECK(d2.point.x == 0 && d2.key == 0, "★ 无回调时坐标与按键一并复位");
        CHECK(lvglcj_indev_delete(iv2) == LVGLCJ_OK, "删除该 indev");
    }

    CHECK(lvglcj_indev_set_point(1, 2, 1) == LVGLCJ_ERR_NOT_SUPPORTED,
          "trampoline 返回后 setter 再次不可用（暂存已清空）");

    /* ================================================== 8. group */
    printf("\n-- 8. group 与焦点 --\n");
    int64_t grp = lvglcj_group_create();
    CHECK(grp != LVGLCJ_HANDLE_NULL, "创建 group");
    CHECK(lvglcj_group_add_obj(grp, box) == LVGLCJ_OK, "把对象加入 group");
    CHECK(lvglcj_group_focus_obj(box) == LVGLCJ_OK, "聚焦该对象");
    CHECK(lvglcj_group_get_focused(grp) == box,
          "★ get_focused 返回同一句柄（CMAP 反查正确）");
    CHECK(lvglcj_indev_set_group(iv, grp) == LVGLCJ_OK, "把 group 绑到 indev");
    CHECK(lvglcj_group_focus_next(grp) == LVGLCJ_OK, "focus_next 可用");
    CHECK(lvglcj_group_focus_prev(grp) == LVGLCJ_OK, "focus_prev 可用");
    CHECK(lvglcj_group_set_default(grp) == LVGLCJ_OK, "设为默认组");
    CHECK(lvglcj_group_set_default(0) == LVGLCJ_OK, "传 0 取消默认组（刻意允许）");
    CHECK(lvglcj_group_remove_obj(box) == LVGLCJ_OK, "从组中移除对象");

    /* ================================================== 9. 删除顺序与句柄状态 */
    printf("\n-- 9. 删除：先 display 再 free 缓冲，句柄推进 RELEASED --\n");

    /*
     * ★ 删除顺序按真实收尾顺序：先删子对象，再删 display。
     *   这也是 hello_cj 退出时必须遵守的顺序 —— 反过来的话
     *   display 被删后对象仍引用它，refresh 会拿到空 display。
     */
    CHECK(lvglcj_obj_delete(box) == LVGLCJ_OK, "先删除子对象");
    CHECK(lvglcj_obj_delete(box) == LVGLCJ_ERR_INVALID_HANDLE,
          "★ 重复删除返回 INVALID_HANDLE（幂等由仓颉 close() 负责）");

    /*
     * ★ 四态设计的关键区分（P0 断言④）：
     *   · 被**直接删除**的对象 → 表项回收（UNINIT）
     *   · 因**父对象被删**而失效的子对象 → INVALIDATED（表项保留）
     *   后者保留表项是刻意的：用户拿到子句柄再操作时能收到
     *   「父对象已删除」这类明确报错并保留删除栈，而不是一个含糊的「无效句柄」。
     */
    {
        int64_t parent = lvglcj_obj_create(scr);
        int64_t child = lvglcj_obj_create(parent);
        CHECK(parent != LVGLCJ_HANDLE_NULL && child != LVGLCJ_HANDLE_NULL,
              "创建父子对象");
        CHECK(lvglcj_obj_delete(parent) == LVGLCJ_OK, "删除父对象");
        CHECK(lvglcj_handle_state(child) == LVGLCJ_HSTATE_INVALIDATED,
              "★ 子句柄为 INVALIDATED（表项保留以便诊断，而非 UNINIT）");
        CHECK(lvglcj_handle_state(parent) == LVGLCJ_HSTATE_UNINIT,
              "★ 被直接删除的父句柄表项已回收（UNINIT）");
        CHECK(lvglcj_obj_delete(child) == LVGLCJ_ERR_INVALID_HANDLE,
              "★ 子对象已被级联失效 → 再删除返回 INVALID_HANDLE");
    }

    CHECK(lvglcj_display_delete(disp) == LVGLCJ_OK, "删除 display 成功");
    CHECK(lvglcj_handle_state(disp) == LVGLCJ_HSTATE_UNINIT,
          "删除后表项被回收");
    CHECK(lvglcj_display_get_buf_bytes(disp) == -1,
          "★ 删除后查不到缓冲（已 free，避免悬空信息）");
    CHECK(lvglcj_display_delete(disp) == LVGLCJ_ERR_INVALID_HANDLE,
          "重复删除返回 INVALID_HANDLE");

    CHECK(lvglcj_indev_delete(iv) == LVGLCJ_OK, "删除 indev");
    CHECK(lvglcj_indev_delete(iv) == LVGLCJ_ERR_INVALID_HANDLE,
          "indev 重复删除返回 INVALID_HANDLE");
    CHECK(lvglcj_group_delete(grp) == LVGLCJ_OK, "删除 group");
    CHECK(lvglcj_group_delete(grp) == LVGLCJ_ERR_INVALID_HANDLE,
          "group 重复删除返回 INVALID_HANDLE");

    lvglcj_display_delete(d2);
    lvglcj_display_delete(d3);

    /* ================================================== 10. 非本层句柄的防护 */
    printf("\n-- 10. 句柄防护 --\n");
    CHECK(lvglcj_display_get_buf_bytes(0) == -1, "句柄 0 查缓冲返回 -1");
    CHECK(lvglcj_display_get_buf_stride(999999) == -1, "不存在句柄查 stride 返回 -1");
    CHECK(lvglcj_indev_delete(999999) == LVGLCJ_ERR_INVALID_HANDLE,
          "删除不存在句柄返回 INVALID_HANDLE 且不崩");
    CHECK(lvglcj_indev_set_read_cb(999999, 1) == LVGLCJ_ERR_INVALID_HANDLE,
          "给无效句柄注册 read 回调被拒");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_set_dispatch(NULL);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
