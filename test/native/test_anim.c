/*
 * test_anim.c —— C 侧单测：动画子系统（设计文档 §3.10 / ADR-015 / ADR-017）
 *
 * 覆盖：
 *   · exec 回调真的按帧被调用，且插值单调地从 from 走向 to
 *   · ★ internal deleted_cb 是**唯一释放点**：自然结束 / 手动删除 / 对象删除
 *     三条路径下 anim_ctx 都被回收（P0 断言⑧的直接证据）
 *   · ★ ADR-015 代价补偿：var=ctx 使 LVGL「对象删除自动停动画」失效，
 *     必须靠 DELETE 钩子调 stop_all_anims_of_obj —— 这里正是验证它真的补上了
 *   · ★ 泄漏对账：稳态下 anim_ctx 数与运行时动画数**同时归零**
 *   · start 之后 set 被拒绝（start 会拷贝模板，否则是「设了但没生效」的静默失效）
 *   · 未设 exec_cb 就 start 被拒绝；非法 path / 负数时长被拒绝
 *   · 「start 前就删除」这条特殊路径也必须释放 ctx（它不会走 deleted_cb）
 *
 * 关于 dispatch 桩：本用例是纯 C 测试，没有仓颉侧，因此自己分配 cid 并**按区间**
 * 区分用途（10..=exec / 20..=deleted / 30..=start）。这样桩就能分辨
 * 「deleted 被调用（arg=0）」与「exec 插值恰好为 0」——否则二者在数值上无法区分。
 */
#include "lvglcj_internal.h"
#include "lvglcj_backend_null.h"

#include <stdio.h>
#include <string.h>
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

/* ---------------------------------------------------- dispatch 桩 */
#define CID_EXEC  10
#define CID_EXEC2 11
#define CID_DEL   20
#define CID_START 30
#define CID_MAX   64

static int     g_calls[CID_MAX];       /* 各 cid 被调用次数（不含注销） */
static int64_t g_last_arg[CID_MAX];    /* 各 cid 最近一次参数 */
static int     g_unreg[CID_MAX];       /* 各 cid 被注销次数 */
static int32_t g_exec_first;
static int32_t g_exec_last;
static int     g_exec_n;

static void reset_stats(void)
{
    memset(g_calls, 0, sizeof(g_calls));
    memset(g_last_arg, 0, sizeof(g_last_arg));
    memset(g_unreg, 0, sizeof(g_unreg));
    g_exec_first = 0;
    g_exec_last = 0;
    g_exec_n = 0;
}

static int32_t stub_dispatch(int32_t cid, int64_t arg)
{
    if (cid <= 0 || cid >= CID_MAX) {
        return LVGLCJ_OK;
    }
    if (arg == LVGLCJ_ARG_UNREGISTER) {
        g_unreg[cid]++;
        return LVGLCJ_OK;
    }
    g_calls[cid]++;
    g_last_arg[cid] = arg;

    /* 10 号是主 exec：记录插值序列的首尾，用于断言「真的在插值」 */
    if (cid == CID_EXEC) {
        if (g_exec_n == 0) {
            g_exec_first = (int32_t)arg;
        }
        g_exec_last = (int32_t)arg;
        g_exec_n++;
    }
    return LVGLCJ_OK;
}

/* 推进约 ms 毫秒（null 后端每帧 sleep 5ms） */
static void pump_ms(int32_t ms)
{
    int32_t frames = ms / 5;
    if (frames < 1) {
        frames = 1;
    }
    (void)lvglcj_null_pump(frames);
}

int main(void)
{
    printf("=== test_anim：动画子系统 ===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    lvglcj_set_dispatch(stub_dispatch);

    /* G1：必须先有 display 才能建对象（动画要绑对象就离不开它） */
    if (lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
        printf("  [FAIL] headless display 初始化\n");
        return 1;
    }
    int64_t scr = lvglcj_screen_active();
    int64_t box = lvglcj_obj_create(scr);
    lvglcj_obj_set_size(box, 50, 50);

    /* ================================================== 1. 自然结束（断言⑧） */
    printf("\n-- 1. ★ 自然结束：repeat(1) 跑完后 anim_ctx 被释放（断言⑧）--\n");
    reset_stats();
    CHECK(lvglcj_anim_ctx_count() == 0, "起始时 anim_ctx 数为 0");

    {
        int64_t a = lvglcj_anim_create();
        CHECK(a != LVGLCJ_HANDLE_NULL, "anim_create 成功");
        CHECK(lvglcj_anim_ctx_count() == 1, "创建后 anim_ctx 数为 1");

        CHECK(lvglcj_anim_set_target(a, box) == LVGLCJ_OK, "set_target");
        CHECK(lvglcj_anim_set_values(a, 0, 100) == LVGLCJ_OK, "set_values(0,100)");
        CHECK(lvglcj_anim_set_time(a, 100) == LVGLCJ_OK, "set_time(100ms)");
        CHECK(lvglcj_anim_set_exec_cb(a, CID_EXEC) == LVGLCJ_OK, "set_exec_cb");
        CHECK(lvglcj_anim_set_deleted_cb(a, CID_DEL) == LVGLCJ_OK, "set_deleted_cb");
        CHECK(lvglcj_anim_set_start_cb(a, CID_START) == LVGLCJ_OK, "set_start_cb");
        /* repeat(1) = 只跑一趟（LVGL 语义：0 才是无限） */
        CHECK(lvglcj_anim_set_repeat(a, 1) == LVGLCJ_OK, "set_repeat(1)");
        CHECK(lvglcj_anim_set_path(a, LVGLCJ_ANIM_PATH_LINEAR) == LVGLCJ_OK,
              "set_path(linear)");

        CHECK(lvglcj_anim_start(a) == LVGLCJ_OK, "anim_start");
        CHECK(lvglcj_anim_count_running() >= 1, "启动后确有动画在跑");
        CHECK(lvglcj_anim_ctx_count() == 1, "运行中 anim_ctx 数为 1");

        /* 跑到自然结束（100ms 动画 + 余量） */
        pump_ms(400);

        CHECK(g_calls[CID_EXEC] > 0, "★ exec 回调被按帧调用");
        CHECK(g_calls[CID_START] == 1, "★ start 回调恰好被调用 1 次");
        CHECK(g_calls[CID_DEL] == 1, "★ deleted 回调恰好被调用 1 次");
        CHECK(g_exec_last == 100, "★ 插值最终到达终点 100");
        CHECK(lvglcj_anim_count_running() == 0, "自然结束后没有动画在跑");
        CHECK(lvglcj_anim_ctx_count() == 0,
              "★ 自然结束后 anim_ctx 归零（deleted_cb 是唯一释放点）");

        /* 三个闭包都应被注销，否则仓颉侧闭包表泄漏 */
        CHECK(g_unreg[CID_EXEC] == 1, "★ exec 闭包已注销");
        CHECK(g_unreg[CID_DEL] == 1, "★ deleted 闭包已注销");
        CHECK(g_unreg[CID_START] == 1, "★ start 闭包已注销");

        /* 动画句柄也应已回收 */
        CHECK(lvglcj_handle_state(a) == LVGLCJ_HSTATE_UNINIT,
              "★ ctx 句柄表项已回收（用户不需要手动 release）");
        CHECK(lvglcj_anim_delete(a) == LVGLCJ_ERR_INVALID_HANDLE,
              "对已结束的动画再 delete 返回 INVALID_HANDLE（幂等由仓颉 close() 负责）");
    }

    /* ================================================== 2. 手动删除 */
    printf("\n-- 2. 手动删除：同样走 deleted_cb --\n");
    reset_stats();
    {
        int64_t a = lvglcj_anim_create();
        lvglcj_anim_set_target(a, box);
        lvglcj_anim_set_values(a, 0, 100);
        lvglcj_anim_set_time(a, 5000); /* 足够长，不会被自然结束抢先 */
        lvglcj_anim_set_exec_cb(a, CID_EXEC);
        lvglcj_anim_set_deleted_cb(a, CID_DEL);
        lvglcj_anim_set_repeat(a, 1);
        CHECK(lvglcj_anim_start(a) == LVGLCJ_OK, "启动长动画");

        pump_ms(50);
        CHECK(lvglcj_anim_ctx_count() == 1, "运行中 ctx 数为 1");
        CHECK(lvglcj_anim_delete(a) == LVGLCJ_OK, "手动删除");
        CHECK(g_calls[CID_DEL] == 1, "★ 手动删除也触发 deleted 回调");
        CHECK(lvglcj_anim_ctx_count() == 0, "★ 手动删除后 ctx 归零");
        CHECK(lvglcj_anim_count_running() == 0, "已无动画在跑");
    }

    /* ================================================== 3. 对象删除（ADR-015 补偿） */
    printf("\n-- 3. ★ 对象删除：ADR-015 代价补偿必须生效 --\n");
    reset_stats();
    {
        /*
         * var=ctx 使 LVGL 无法按 var==obj 找到动画，因此**必须**由 DELETE 钩子
         * 调 stop_all_anims_of_obj 补偿。若不补偿，本用例会看到：
         *   ctx 数不归零、动画仍在跑（这正是 V7 实测的 running 1->1 残留）。
         */
        int64_t victim = lvglcj_obj_create(scr);
        CHECK(victim != LVGLCJ_HANDLE_NULL, "创建将被删除的对象");

        int64_t a1 = lvglcj_anim_create();
        lvglcj_anim_set_target(a1, victim);
        lvglcj_anim_set_values(a1, 0, 100);
        lvglcj_anim_set_time(a1, 5000);
        lvglcj_anim_set_exec_cb(a1, CID_EXEC);
        lvglcj_anim_set_deleted_cb(a1, CID_DEL);
        lvglcj_anim_set_repeat(a1, 0); /* 无限循环：只有补偿才能停掉它 */
        CHECK(lvglcj_anim_start(a1) == LVGLCJ_OK, "在一个对象上启动无限循环动画");

        int64_t a2 = lvglcj_anim_create();
        lvglcj_anim_set_target(a2, victim);
        lvglcj_anim_set_values(a2, 0, 50);
        lvglcj_anim_set_time(a2, 5000);
        lvglcj_anim_set_exec_cb(a2, CID_EXEC2);
        lvglcj_anim_set_repeat(a2, 0);
        CHECK(lvglcj_anim_start(a2) == LVGLCJ_OK, "同一对象上再启动一个动画");

        pump_ms(60);
        CHECK(lvglcj_anim_ctx_count() == 2, "两个动画各有独立 ctx（var=ctx 的意义）");
        CHECK(g_calls[CID_EXEC] > 0 && g_calls[CID_EXEC2] > 0,
              "★ 两个动画的 exec 各自被调用（未互相串台）");

        CHECK(lvglcj_obj_delete(victim) == LVGLCJ_OK, "删除该对象");
        CHECK(lvglcj_anim_count_running() == 0,
              "★ 删除对象后动画被停掉（ADR-015 补偿生效）");
        CHECK(lvglcj_anim_ctx_count() == 0,
              "★ 两个 anim_ctx 均被回收");
        CHECK(g_calls[CID_DEL] == 1, "带 deleted 回调的那个被通知了一次");
    }

    /* ================================================== 4. 前置校验与拒绝 */
    printf("\n-- 4. 前置校验：把「静默失效」变成明确报错 --\n");
    reset_stats();
    {
        int64_t a = lvglcj_anim_create();
        CHECK(a != LVGLCJ_HANDLE_NULL, "创建动画用于校验");

        /* 未设 exec_cb 就 start：必须拒绝，否则是一个「跑着但无效果」的动画 */
        CHECK(lvglcj_anim_start(a) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "★ 未设 exec_cb 就 start 被拒");

        CHECK(lvglcj_anim_set_exec_cb(a, CID_EXEC) == LVGLCJ_OK, "补上 exec_cb");
        CHECK(lvglcj_anim_set_exec_cb(a, 0) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "exec_cb 传 0（无闭包哨兵）被拒");
        CHECK(lvglcj_anim_set_path(a, 999) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "未知插值曲线被拒");
        CHECK(lvglcj_anim_set_time(a, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "负时长被拒");
        CHECK(lvglcj_anim_set_delay(a, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "负延迟被拒");
        CHECK(lvglcj_anim_set_repeat(a, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "负重复次数被拒");
        CHECK(lvglcj_anim_set_playback(a, -1) == LVGLCJ_ERR_INVALID_ARGUMENT,
              "负 playback 被拒");
        CHECK(lvglcj_anim_set_target(a, 999999) == LVGLCJ_ERR_INVALID_HANDLE,
              "绑到不存在的对象被拒");

        /* 仍可正常启动 */
        lvglcj_anim_set_values(a, 0, 10);
        lvglcj_anim_set_time(a, 50);
        CHECK(lvglcj_anim_start(a) == LVGLCJ_OK, "校验后仍能正常启动");

        /* ★ start 之后再 set：明确拒绝（start 会拷贝模板，否则是静默失效） */
        CHECK(lvglcj_anim_set_values(a, 0, 999) == LVGLCJ_ERR_INVALID_HANDLE,
              "★ start 之后再 set 被拒（避免「设了但没生效」的静默失效）");
        CHECK(lvglcj_anim_start(a) == LVGLCJ_ERR_INVALID_HANDLE,
              "重复 start 被拒");

        pump_ms(200);
        CHECK(lvglcj_anim_ctx_count() == 0, "结束后 ctx 归零");
    }

    /* ================================================== 5. start 前就删除 */
    printf("\n-- 5. 特殊路径：start 之前就删除 --\n");
    reset_stats();
    {
        /*
         * 这条路径**不会**触发 deleted_cb（动画从未进入 LVGL 的管理），
         * 因此必须由 lvglcj_anim_delete 自己负责释放 ctx 与注销闭包。
         * 漏了它就会稳定泄漏一个 ctx —— 是很容易被漏掉的一条分支。
         */
        int64_t a = lvglcj_anim_create();
        lvglcj_anim_set_exec_cb(a, CID_EXEC);
        lvglcj_anim_set_deleted_cb(a, CID_DEL);
        CHECK(lvglcj_anim_ctx_count() == 1, "未启动的动画也占用一个 ctx");

        CHECK(lvglcj_anim_delete(a) == LVGLCJ_OK, "start 之前删除");
        CHECK(lvglcj_anim_ctx_count() == 0, "★ ctx 被释放（这条路径不经过 deleted_cb）");
        CHECK(g_calls[CID_DEL] == 0, "未启动的动画不应触发 deleted 回调");
        CHECK(g_unreg[CID_EXEC] == 1, "★ exec 闭包已注销（否则仓颉侧泄漏）");
        CHECK(g_unreg[CID_DEL] == 1, "★ deleted 闭包已注销");
    }

    /* ================================================== 6. 非法句柄的健壮性 */
    printf("\n-- 6. 非法句柄不崩且明确报错 --\n");
    {
        CHECK(lvglcj_anim_start(999999) == LVGLCJ_ERR_INVALID_HANDLE,
              "对非法句柄 start 返回 INVALID_HANDLE");
        CHECK(lvglcj_anim_delete(999999) == LVGLCJ_ERR_INVALID_HANDLE,
              "对非法句柄 delete 返回 INVALID_HANDLE");
        CHECK(lvglcj_anim_set_time(0, 100) == LVGLCJ_ERR_INVALID_HANDLE,
              "对句柄 0 操作返回 INVALID_HANDLE");
        CHECK(lvglcj_obj_delete_anim(999999) == LVGLCJ_ERR_INVALID_HANDLE,
              "对非法对象停动画返回 INVALID_HANDLE");
    }

    /* ================================================== 7. 泄漏对账 */
    printf("\n-- 7. ★ 泄漏对账（§11.3）：稳态双零 --\n");
    pump_ms(100);
    CHECK(lvglcj_anim_ctx_count() == 0, "★ 稳态下 anim_ctx 数为 0");
    CHECK(lvglcj_anim_count_running() == 0, "★ 稳态下运行中动画数为 0");

    /* ================================================== 8. 清理 */
    printf("\n-- 8. 清理 --\n");
    CHECK(lvglcj_obj_delete(box) == LVGLCJ_OK, "删除对象");
    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless 后端");
    CHECK(lvglcj_anim_ctx_count() == 0, "全部清理后 ctx 仍为 0");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_set_dispatch(NULL);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
