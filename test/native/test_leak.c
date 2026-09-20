/*
 * test_leak.c —— 泄漏门禁（设计文档 §11.1 / §7.3 / §10.3）
 *
 * 覆盖三条门禁判据：
 *
 *   ① **句柄泄漏**：1 万次 create/delete 后 handleCount(ALIVE) 增量 = 0（§11.1）
 *   ② **句柄与对象一致**：handleCount(ALIVE) 与 LVGL 内部 objCount 的**差值恒定**（§7.3）
 *      —— 注意是「差值恒定」而不是「两者相等」：句柄表里还活着 display / indev /
 *      group / anim 等非对象条目，以及 layer 等未经我们创建的对象。
 *      判据的价值在于**两个数是独立测量的**（一个来自我们的表，一个来自 LVGL 的树）。
 *   ③ **删除后不可再访问**：被删句柄必须明确报错，而不是静默成功或崩溃
 *
 * ★ 另外**如实测量并打印**级联删除的 INVALIDATED 保留量（见用例 4）。
 *   设计上「父对象被删 → 子句柄进入 INVALIDATED 且**保留表项**」是为了
 *   给出明确报错与保留删除栈；但保留就意味着表会随级联删除增长。
 *   本用例把这条曲线的真实斜率打印出来，而不是含糊地断言「没有泄漏」——
 *   24h soak 的「句柄收敛」判据需要知道它到底收敛不收敛。
 */
#include "lvglcj_internal.h"
#include "lvglcj_backend_null.h"

#include <stdio.h>
#include <string.h>

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

/* 迭代轮数：门禁要求 1 万次 */
#define ITERATIONS 10000
/* 批量模式每批创建/删除的个数 */
#define BATCH 250

static int32_t alive_of(void)
{
    return lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE);
}

int main(void)
{
    printf("=== test_leak：泄漏门禁（§11.1）===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    /* headless：泄漏门禁不应依赖窗口系统，否则 CI 上跑不起来 */
    if (lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
        printf("  [FAIL] headless display 初始化\n");
        return 1;
    }

    int64_t scr = lvglcj_screen_active();

    /* ================================================ 1. 逐个 create/delete */
    printf("\n-- 1. ★ 门禁①：%d 次 create/delete 后 ALIVE 增量 = 0 --\n", ITERATIONS);
    int32_t alive_before = alive_of();
    int32_t obj_before = lvglcj_obj_count();
    int32_t delta_before = alive_before - obj_before;
    printf("       基线：ALIVE=%d  objCount=%d  差值=%d\n",
           alive_before, obj_before, delta_before);

    int64_t last_handle = 0;
    for (int32_t i = 0; i < ITERATIONS; i++) {
        int64_t o = lvglcj_obj_create(scr);
        if (o == LVGLCJ_HANDLE_NULL) {
            printf("  [FAIL] 第 %d 次 create 失败\n", i);
            g_fail++;
            break;
        }
        if (lvglcj_obj_delete(o) != LVGLCJ_OK) {
            printf("  [FAIL] 第 %d 次 delete 失败\n", i);
            g_fail++;
            break;
        }
        last_handle = o;
    }

    int32_t alive_after = alive_of();
    CHECK(alive_after == alive_before,
          "★ ALIVE 句柄数回到基线（增量 = 0）");

    /* ================================================ 2. 句柄与对象一致 */
    printf("\n-- 2. ★ 门禁②：ALIVE 与 objCount 差值恒定 --\n");
    int32_t obj_after = lvglcj_obj_count();
    printf("       结束：ALIVE=%d  objCount=%d  差值=%d\n",
           alive_after, obj_after, alive_after - obj_after);
    CHECK(obj_after == obj_before,
          "★ objCount 回到基线（LVGL 侧也没有残留对象）");
    CHECK(alive_after - obj_after == delta_before,
          "★ ALIVE - objCount 差值恒定");

    /* ================================================ 3. 删除后不可再访问 */
    printf("\n-- 3. 删除后不可再访问（防「静默成功」）--\n");
    CHECK(lvglcj_handle_state(last_handle) == LVGLCJ_HSTATE_UNINIT,
          "已删句柄的表项已回收（UNINIT）");
    CHECK(lvglcj_obj_delete(last_handle) == LVGLCJ_ERR_INVALID_HANDLE,
          "重复删除返回 INVALID_HANDLE");
    CHECK(lvglcj_obj_set_pos(last_handle, 1, 1) == LVGLCJ_ERR_INVALID_HANDLE,
          "对已删句柄操作返回 INVALID_HANDLE");

    /* ================================================ 4. 批量模式 */
    printf("\n-- 4. 批量模式（每批 %d 个，共 %d 批）--\n", BATCH, ITERATIONS / BATCH);
    alive_before = alive_of();
    obj_before = lvglcj_obj_count();
    delta_before = alive_before - obj_before;

    for (int32_t round = 0; round < ITERATIONS / BATCH; round++) {
        int64_t batch[BATCH];
        for (int32_t i = 0; i < BATCH; i++) {
            batch[i] = lvglcj_obj_create(scr);
        }
        /* 正向、逆序、以及"中间开花"三种删除顺序都走一遍，覆盖表项回收的探测链整理 */
        int32_t mode = round % 3;
        for (int32_t i = 0; i < BATCH; i++) {
            int32_t idx = (mode == 0) ? i : ((mode == 1) ? (BATCH - 1 - i) : ((i * 7) % BATCH));
            if (batch[idx] != LVGLCJ_HANDLE_NULL) {
                (void)lvglcj_obj_delete(batch[idx]);
                batch[idx] = LVGLCJ_HANDLE_NULL; /* 防重复删（会返回 INVALID_HANDLE） */
            }
        }
    }
    CHECK(alive_of() == alive_before, "★ 批量模式后 ALIVE 增量 = 0");
    CHECK(lvglcj_obj_count() == obj_before, "★ 批量模式后 objCount 回到基线");
    CHECK(alive_of() - lvglcj_obj_count() == delta_before, "★ 批量模式后差值恒定");

    /* ================================================ 5. 级联删除的有界保留 */
    printf("\n-- 5. ★ 级联删除：INVALIDATED 保留量必须**有界** --\n");
    {
        /*
         * 每次迭代：建 1 个父 + 10 个子，删父 → 10 个子句柄进入 INVALIDATED。
         * 设计上这是**有意保留**（便于报「父对象已删除」并保留删除栈）。
         *
         * 但保留若无上限，任何「建容器 → 删容器」的循环都会让表项单调增长：
         * 实测（未加回收时）是 10 条/轮，且完全线性 ——
         * 那样 §11.1 的 soak 判据「句柄收敛」直接不成立，内存也无界。
         * 因此本层按 LVGLCJ_INVALIDATED_KEEP_MAX 做最旧优先回收，
         * 本用例断言的就是这条上界**真的成立**。
         *
         * 轮数刻意放大到 2000 轮（= 2 万个子对象），
         * 是上限的 20 倍，确保测到的是「平台」而不是「还没长到」。
         */
        const int32_t ROUNDS = 2000;
        int32_t base_invalid = lvglcj_handle_count(LVGLCJ_HSTATE_INVALIDATED);
        int32_t base_alive = alive_of();
        printf("       起点：INVALIDATED=%d  ALIVE=%d  上限=%d\n",
               base_invalid, base_alive, LVGLCJ_INVALIDATED_KEEP_MAX);

        int32_t peak = 0;
        for (int32_t round = 1; round <= ROUNDS; round++) {
            int64_t p = lvglcj_obj_create(scr);
            for (int32_t i = 0; i < 10; i++) {
                (void)lvglcj_obj_create(p);
            }
            (void)lvglcj_obj_delete(p);

            int32_t cur = lvglcj_handle_count(LVGLCJ_HSTATE_INVALIDATED);
            if (cur > peak) {
                peak = cur;
            }
            if (round % (ROUNDS / 4) == 0) {
                printf("       %5d 轮（累计 %6d 个子对象）：INVALIDATED=%d\n",
                       round, round * 10, cur);
            }
        }
        int32_t end_invalid = lvglcj_handle_count(LVGLCJ_HSTATE_INVALIDATED);
        printf("       峰值=%d  终值=%d  累计失效 %d 条\n",
               peak, end_invalid, ROUNDS * 10);

        /*
         * ★ 这条是本用例的核心断言：累计失效 2 万条，
         *   而同时在册的 INVALIDATED 始终不超过上限。
         *   「累计远大于上限而存量不超上限」正是有界回收的定义。
         */
        CHECK(peak <= LVGLCJ_INVALIDATED_KEEP_MAX,
              "★ 保留量峰值不超过上限（累计 2 万条失效而存量有界）");
        CHECK(end_invalid <= LVGLCJ_INVALIDATED_KEEP_MAX,
              "★ 终值不超过上限");

        /* 直接删除的对象不残留 ALIVE —— 这条门禁必须守住 */
        CHECK(alive_of() == base_alive,
              "★ 级联删除后 ALIVE 增量 = 0（子对象已被级联失效，不再是 ALIVE）");
        CHECK(lvglcj_obj_count() == obj_before,
              "★ 级联删除后 LVGL 侧无残留对象");
        CHECK(alive_of() - lvglcj_obj_count() == delta_before,
              "★ 级联删除后 ALIVE - objCount 差值仍恒定");
    }

    /* ================================================ 6. 事件对象（短命对象）不泄漏 */
    printf("\n-- 6. 短命对象（事件）不累积表项 --\n");
    {
        /*
         * 事件对象是每次分发都新建的，是表项增长最快的来源（§5.2 注释）。
         * 这里用一个自定义 timer 反复触发事件是不可能的（事件需要真实事件源），
         * 因此改为测「多次 dump_tree 不泄漏 dump 自身」——那是 t8 新增的接口。
         */
        int32_t base = alive_of();
        for (int32_t i = 0; i < 100; i++) {
            lvglcj_tree_dump_t dump;
            memset(&dump, 0, sizeof(dump));
            int32_t n = lvglcj_debug_dump_tree(scr, 4, &dump);
            if (n > 0) {
                /* 每次都必须释放；漏掉一次就会在 ASan 下暴露 */
                lvglcj_debug_free_tree_dump(&dump);
            }
        }
        CHECK(alive_of() == base, "100 次 dump + free 后 ALIVE 不变");
    }

    /* ================================================ 7. 清理 */
    printf("\n-- 7. 清理 --\n");
    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless 后端");
    CHECK(lvglcj_deinit() == LVGLCJ_OK, "反初始化运行时");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
