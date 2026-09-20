/*
 * test_soak_asan.c —— 纯 C 层的长跑与随机序列测试（承担 ASan 覆盖，§11.4）
 *
 * ============================================================================
 * 【为什么需要它：§11.4 与 §9.5 互斥，已决策为「方案 a」】
 * ============================================================================
 * §11.4 要求 24h 运行满足「零崩溃、**零 ASan 报告**、内存收敛」，
 * 但 §9.5 实测确立「ASan 与仓颉运行时硬不兼容」：混合二进制在 ASan 下会崩在
 * 退出路径（CJ_ScheduleStop 的线程簿记），`detect_leaks=0` 也避不开。
 *
 * 于是「24h 混跑 + 零 ASan 报告」不存在可实现路径。决策为方案 a：
 *   · 主进程 24h（scripts/soak.sh）**不带 ASan**，判 RSS 与四类计数收敛；
 *   · ASan 覆盖**由本文件承载**：同样形状的负载，纯 C，完整 ASan 检测。
 *
 * ============================================================================
 * 【覆盖边界 —— 必须说清，不许含糊成「ASan 也测了」】
 * ============================================================================
 *   ✅ 完整 ASan 检测：越界读写 / use-after-free / 泄漏
 *   ✅ C 层长跑状态：句柄表（含 INVALIDATED 有界回收）、样式使用者注册表、
 *      延迟删除队列、anim_ctx 生命周期、display、对象树
 *
 *   ❌ **回调链路**：closure_id 由仓颉侧分配（callback.h 明确约定），
 *      纯 C 进程里没有任何闭包，因此：
 *        · lvglcj_obj_add_event(obj, code, cid)  无 cid 可用
 *        · lvglcj_anim_set_exec_cb(a, cid)       无 cid 可用
 *                                 → 而没有 exec_cb 时 start 会被 C 侧拒绝
 *      所以本文件**无法**驱动事件回调与动画回调，也就覆盖不到
 *      「回调里分配/释放的资源」那一类缺陷。
 *      那部分只能靠仓颉侧 soak（无 ASan）用四类计数器兜住。
 *
 *   两者互补，缺一不可 —— 单看任何一边都不能声称「长跑已验证」。
 *
 * ============================================================================
 * 【两种模式】
 * ============================================================================
 *   （默认）确定序列：固定操作序列，可复现、可用于 bisect。
 *   --fuzz          随机序列：随机 op + 随机参数，**打印 seed 以便复现**。
 *
 * ★ 为什么 fuzz 模式值得单独做：t8 抓到的两个缺陷是同一类 ——
 *   SDL2 deinit 漏复位会话计数器、内置字体登记表「容量 8 + 静默溢出」——
 *   **单独跑都不暴露，只在长序列 / 多轮 init-deinit 后才现形**。
 *   两次命中同一类，说明这类漏洞没有系统性防线，而随机序列正是它的克星。
 *
 * 用法：
 *   test_soak_asan                          # 8 秒确定性序列（CI 用）
 *   test_soak_asan --seconds 3600           # 1 小时确定序列
 *   test_soak_asan --fuzz --seconds 7200    # 2 小时随机序列（§11.4 的 2h 模糊）
 *   test_soak_asan --fuzz --seed 12345      # 复现某个 seed 的序列
 */

#include "lvgl.h"
#include "lvglcj_bridge.h"
#include "lvglcj_backend_null.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================ 测试框架片段 */

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                          \
    do {                                          \
        g_checks++;                               \
        if (cond) {                               \
            printf("  [ok]   %s\n", (msg));       \
        } else {                                  \
            printf("  [FAIL] %s\n", (msg));       \
            g_fail++;                             \
        }                                         \
    } while (0)

/* 便于阅读的常量：主部件 + 默认状态 */
#define SEL_MAIN ((int32_t)LV_PART_MAIN | (int32_t)LV_STATE_DEFAULT)
/* ★ 与 SEL_MAIN 不同的 selector：用于验证样式使用者注册表**按三元组记账**。
   若注册表只记 (obj, style)，用另一个 selector 移除会误删记录，
   守卫就会误判为"无人引用"而放行释放 —— 那是 UAF。 */
#define SEL_PRESSED ((int32_t)LV_PART_MAIN | (int32_t)LV_STATE_PRESSED)

/* ============================================================ 计时与参数 */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int      g_seconds = 8;
static int      g_fuzz = 0;
static uint64_t g_seed = 20260921u;

/* xorshift64：自带实现，避免依赖 rand() 的可移植性差异 */
static uint64_t g_rng = 1;

static uint64_t rnd(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static int rnd_range(int lo, int hi) /* [lo, hi] 闭区间 */
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(rnd() % (uint64_t)(hi - lo + 1));
}

/* ============================================================ 基线对账 */

static int32_t g_base_alive = 0;
static int32_t g_base_obj = 0;

/* 计数用的观测点：让报告里有几条**有内容的**断言，而不只是"没崩" */
static int g_text_ok = 0;        /* label_set_text 成功次数 */
static int g_anims_made = 0;     /* 动画创建成功次数 */
static int g_guard_rejected = 0; /* 样式引用守卫拒绝释放的次数（**期望 > 0**）*/

static void snapshot_baseline(void)
{
    g_base_alive = lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE);
    g_base_obj = lvglcj_obj_count();
}

/*
 * 稳态断言：所有临时对象都已删除之后，三个计数必须回到基线。
 *
 * ★ 这是本文件真正的判据。ASan 只能抓"用坏了内存"，抓不到
 *   "内存没坏但状态没回收"（句柄表项、样式记录、anim_ctx 都会是这样）。
 *   两者一起才构成完整的长跑判据。
 */
static void check_steady_state(const char *where, int verbose)
{
    int32_t alive = lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE);
    int32_t obj = lvglcj_obj_count();
    int32_t actx = lvglcj_anim_ctx_count();

    if (!verbose && alive == g_base_alive && obj == g_base_obj && actx == 0) {
        return; /* 常态：静默通过，避免刷屏淹没真正的失败 */
    }

    char buf[192];
    snprintf(buf, sizeof(buf), "%s：稳态计数归位（alive %d→%d，obj %d→%d，anim_ctx=%d）",
             where, g_base_alive, alive, g_base_obj, obj, actx);
    CHECK(alive == g_base_alive && obj == g_base_obj && actx == 0, buf);

    if (alive != g_base_alive) {
        printf("         ★ ALIVE 句柄漂移 %d：对象删了但表项没回收\n",
               alive - g_base_alive);
    }
    if (obj != g_base_obj) {
        printf("         ★ LVGL 对象漂移 %d：对象树里有残留\n", obj - g_base_obj);
    }
    if (actx != 0) {
        printf("         ★ anim_ctx 残留 %d：动画上下文没被释放\n", actx);
    }
}

/* ============================================================ 负载：一个工作单元 */

/*
 * 建一个容器 + 若干子对象 + 一个样式 + 一个动画，然后**全部拆掉**。
 *
 * 之所以每轮都以「拆干净」结束，是为了让稳态对账在**每一轮之后**都能成立；
 * 否则计数会随轮数累积，漂移与"还没拆"就分不清了 ——
 * 而本文件最想抓的恰恰是"拆了但没回收"。
 */
static void one_unit(int round)
{
    int64_t scr = lvglcj_screen_active();

    /* ---- 容器 + 子对象（深度 2，数量随轮数起伏） */
    int64_t box = lvglcj_obj_create(scr);
    if (box == LVGLCJ_HANDLE_NULL) {
        return;
    }
    int kids = 3 + (round % 8);
    int64_t first_label = LVGLCJ_HANDLE_NULL;
    for (int i = 0; i < kids; i++) {
        int64_t c = (i % 3 == 0) ? lvglcj_label_create(box) : lvglcj_obj_create(box);
        if (c == LVGLCJ_HANDLE_NULL) {
            continue;
        }
        if (i % 3 == 0) {
            /*
             * ★ 文本刻意用 **ASCII**。
             *
             * 不是偷懒：默认字体（Montserrat）没有中文字形，写中文会让每帧刷
             * 「glyph dsc. not found」警告，把本用例的输出淹掉；
             * 而这里要测的是**对象/样式/句柄的生命周期**，不是文本渲染。
             * 中文覆盖由 font_test.cj 与 scripts/cjk_audit.py 负责 ——
             * 让每个用例只验证一件事，失败时才定位得动。
             */
            (void)lvglcj_label_set_text(c, (i % 2 == 0) ? "hello" : "clicked 3");
            g_text_ok++;
            if (first_label == LVGLCJ_HANDLE_NULL) {
                first_label = c;
            }
        }
    }

    /* ---- 样式：挂到两个不同 selector 上，再移除其中一个
       这一小段专门覆盖「注册表按 (obj, style, selector) 三元组记账」这个不变式 */
    int64_t st = lvglcj_style_create();
    if (st != LVGLCJ_HANDLE_NULL) {
        (void)lvglcj_style_set_bg_color(st, 0x101820u);
        (void)lvglcj_style_set_radius(st, 12);
        (void)lvglcj_style_set_width(st, 200);

        (void)lvglcj_obj_add_style(box, st, SEL_MAIN);
        if (first_label != LVGLCJ_HANDLE_NULL) {
            (void)lvglcj_obj_add_style(first_label, st, SEL_PRESSED);
            /* 只移除 PRESSED 那一份：MAIN 那份**仍在**，因此此时释放样式必须被拒 */
            (void)lvglcj_obj_remove_style(first_label, st, SEL_PRESSED);

            /*
             * 此时 box 仍以 SEL_MAIN 引用 st，因此释放**必须被拒**。
             * 这一段同时钉住两个不变式：
             *   · 引用守卫生效（不会放行释放正在被用的样式 → 不会 UAF）；
             *   · 注册表按 (obj, style, selector) **三元组**记账 ——
             *     移除 PRESSED 那一份**不能**让守卫误判为"无人引用"。
             *     若只记 (obj, style)，上面那次 remove_style 就会把 MAIN 那份
             *     的记录一起删掉，守卫放行 → 这里得到的不是 INVALID_ARGUMENT。
             */
            if (lvglcj_style_delete(st) == LVGLCJ_ERR_INVALID_ARGUMENT) {
                g_guard_rejected++;
            }
        }
    }

    /* ---- 动画：建好、配置好，然后在 start 之前删掉
       「start 之前删除」是一条**不会走 deleted_cb** 的路径
       （动画从未进入 LVGL 管理），必须有单独的释放处理，
       否则每轮稳定泄漏一个 anim_ctx。这里刻意反复走它。 */
    int64_t a = lvglcj_anim_create();
    if (a != LVGLCJ_HANDLE_NULL) {
        g_anims_made++;
        (void)lvglcj_anim_set_target(a, box);
        (void)lvglcj_anim_set_values(a, 0, 100);
        (void)lvglcj_anim_set_time(a, 30);
        (void)lvglcj_anim_set_path(a, LVGLCJ_ANIM_PATH_EASE_IN_OUT);
        (void)lvglcj_anim_set_repeat(a, 1);
        (void)lvglcj_anim_delete(a);
    }

    /* ---- 推进若干帧（走 timer_handler 与刷新路径） */
    (void)lvglcj_timer_handler();
    (void)lvglcj_timer_handler();

    /* ---- 拆：删父（级联失效子对象），再释放样式 */
    (void)lvglcj_obj_delete(box);

    if (st != LVGLCJ_HANDLE_NULL) {
        /* 级联删除的 DELETE 钩子应已解除该对象对样式的引用，因此这次必须成功。
           失败说明钩子漏了这一步 —— 守卫会从"防 UAF"退化成"永久死锁"。 */
        int32_t r = lvglcj_style_delete(st);
        if (round == 0 && r != LVGLCJ_OK) {
            CHECK(0, "★ 对象删除后样式应可释放（DELETE 钩子解除了引用）");
        }
    }
}

/*
 * 长跑进度：每隔一段时间打一行。
 *
 * ★ 为什么必须有它 —— 与 CI 里 ASan 步骤"先回显再做"是同一个教训：
 *   本用例可以跑几小时（CI 手动触发、或本机 24h），
 *   而在这之前它的输出是**开始时一行、结束时一行**。
 *   于是「长时间没有输出」既可能是"正在正常跑"，也可能是"卡住了"，
 *   看日志的人无从区分，只能干等 —— 而这恰恰是 24h 长跑最不能接受的。
 *   打一行进度是极低成本，换来的是「在不在动」变成可观测事实。
 *
 * 间隔取 60 秒：3 分钟的 CI 冒烟只多 3 行，5 小时的长跑约 300 行，都可接受。
 */
static void maybe_progress(uint64_t *last_ms, uint64_t steps, const char *what)
{
    uint64_t t = now_ms();
    if (t - *last_ms < 60000u) {
        return;
    }
    *last_ms = t;
    printf("  [%s] 已运行 %llu 步：alive=%d obj=%d anim_ctx=%d\n", what,
           (unsigned long long)steps, lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE),
           lvglcj_obj_count(), lvglcj_anim_ctx_count());
}

/* ============================================================ 负载：随机序列 */

/*
 * 随机序列。维护一个小对象池，随机地建/删/挂样式/起动画/推进帧，
 * 并且**定期全部清空**再做稳态对账。
 *
 * ★ 「定期全部清空」是关键设计：不清空的话，池子只会单调增长，
 *   稳态对账就永远没有成立的时刻，漂移也就测不出来。
 */
#define POOL_MAX 24
#define STYLE_MAX 8

static void fuzz_loop(uint64_t deadline, uint64_t *steps_out)
{
    int64_t pool[POOL_MAX];
    int64_t styles[STYLE_MAX];
    int pool_n = 0;
    int style_n = 0;
    uint64_t steps = 0;
    uint64_t prog_ms = now_ms();

    memset(pool, 0, sizeof(pool));
    memset(styles, 0, sizeof(styles));

    while (now_ms() < deadline) {
        maybe_progress(&prog_ms, steps, "fuzz");
        /* 每 5000 步彻底清空一次，制造可对账的稳态 */
        if (steps > 0 && steps % 5000 == 0) {
            for (int i = 0; i < pool_n; i++) {
                (void)lvglcj_obj_delete(pool[i]); /* 可能已被级联删掉，返回 INVALID 无害 */
            }
            for (int i = 0; i < style_n; i++) {
                (void)lvglcj_style_delete(styles[i]);
            }
            pool_n = 0;
            style_n = 0;
            (void)lvglcj_timer_handler();
            check_steady_state("fuzz 周期清空", 1);
        }

        int op = rnd_range(0, 12);
        int64_t scr = lvglcj_screen_active();

        if (op <= 3 && pool_n < POOL_MAX) {
            /* 建对象：父随机取池中一个容器，或屏幕 */
            int64_t parent = scr;
            if (pool_n > 0) {
                int64_t cand = pool[rnd_range(0, pool_n - 1)];
                if (lvglcj_handle_state(cand) == LVGLCJ_HSTATE_ALIVE) {
                    parent = cand;
                }
            }
            int64_t o = lvglcj_obj_create(parent);
            if (o != LVGLCJ_HANDLE_NULL) {
                pool[pool_n++] = o;
            }
        } else if (op == 4 && pool_n < POOL_MAX) {
            int64_t l = lvglcj_label_create(scr);
            if (l != LVGLCJ_HANDLE_NULL) {
                if (lvglcj_label_set_text(l, (rnd() & 1u) ? "hello" : "clicked 3") ==
                    LVGLCJ_OK) {
                    g_text_ok++;
                }
                pool[pool_n++] = l;
            }
        } else if (op == 5 && pool_n < POOL_MAX) {
            int64_t b = lvglcj_button_create(scr);
            if (b != LVGLCJ_HANDLE_NULL) {
                pool[pool_n++] = b;
            }
        } else if (op == 6 && pool_n > 0) {
            /* 删一个随机的（可能是容器 → 级联；也可能已被级联删掉 → 无害） */
            int idx = rnd_range(0, pool_n - 1);
            (void)lvglcj_obj_delete(pool[idx]);
            pool[idx] = pool[--pool_n]; /* 尾部交换删除，避免数组中出现空洞 */
        } else if (op == 7 && style_n < STYLE_MAX) {
            int64_t st = lvglcj_style_create();
            if (st != LVGLCJ_HANDLE_NULL) {
                (void)lvglcj_style_set_bg_color(st, (uint32_t)(rnd() & 0xFFFFFFu));
                (void)lvglcj_style_set_radius(st, rnd_range(0, 40));
                styles[style_n++] = st;
            }
        } else if (op == 8 && pool_n > 0 && style_n > 0) {
            /* 挂样式：selector 随机（覆盖三元组记账） */
            int oi = rnd_range(0, pool_n - 1);
            int si = rnd_range(0, style_n - 1);
            if (lvglcj_handle_state(pool[oi]) == LVGLCJ_HSTATE_ALIVE) {
                (void)lvglcj_obj_add_style(pool[oi], styles[si],
                                           (rnd() & 1u) ? SEL_MAIN : SEL_PRESSED);
            }
        } else if (op == 9 && pool_n > 0 && style_n > 0) {
            int oi = rnd_range(0, pool_n - 1);
            int si = rnd_range(0, style_n - 1);
            if (lvglcj_handle_state(pool[oi]) == LVGLCJ_HSTATE_ALIVE) {
                (void)lvglcj_obj_remove_style(pool[oi], styles[si],
                                              (rnd() & 1u) ? SEL_MAIN : SEL_PRESSED);
            }
        } else if (op == 10 && pool_n > 0) {
            /* 动画：建 → 配 → start 前删（不会走 deleted_cb 的那条路径） */
            int oi = rnd_range(0, pool_n - 1);
            int64_t a = lvglcj_anim_create();
            if (a != LVGLCJ_HANDLE_NULL) {
                g_anims_made++;
                (void)lvglcj_anim_set_target(a, pool[oi]);
                (void)lvglcj_anim_set_values(a, 0, rnd_range(1, 500));
                (void)lvglcj_anim_set_time(a, rnd_range(1, 200));
                (void)lvglcj_anim_set_repeat(a, rnd_range(0, 3));
                (void)lvglcj_anim_delete(a);
            }
        } else if (op == 11 && style_n > 0) {
            /*
             * ★ 刻意去撞样式**引用守卫**。
             *
             * 随机序列里大部分样式此时仍挂在某个存活对象上，因此这次释放
             * 多半会被拒 —— 那正是我们要覆盖的路径：
             * 若只测"没有引用时能释放"，就完全错过了 UAF 会发生的那条路。
             * 释放成功（确实无人引用）时把该样式从池里摘掉，避免后续拿死句柄。
             */
            int si = rnd_range(0, style_n - 1);
            int32_t r = lvglcj_style_delete(styles[si]);
            if (r == LVGLCJ_ERR_INVALID_ARGUMENT) {
                g_guard_rejected++;
            } else if (r == LVGLCJ_OK) {
                styles[si] = styles[--style_n];
            }
        } else {
            /* 推进帧 */
            (void)lvglcj_timer_handler();
        }

        steps++;
    }

    /* 收尾：全部拆掉后必须回到基线 */
    for (int i = 0; i < pool_n; i++) {
        (void)lvglcj_obj_delete(pool[i]);
    }
    for (int i = 0; i < style_n; i++) {
        (void)lvglcj_style_delete(styles[i]);
    }
    (void)lvglcj_timer_handler();

    *steps_out = steps;
}

/* ============================================================ 主流程 */

int main(int argc, char **argv)
{
    /*
     * 关掉 stdout 缓冲：长跑的输出是证据。
     * 若进程被 kill / 崩溃，块缓冲会把最后一整块证据一起丢掉，
     * 于是「跑到哪一步崩的」完全不可知（这个坑在 aarch64 交叉验证时踩过）。
     */
    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            g_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fuzz") == 0) {
            g_fuzz = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            g_seed = strtoull(argv[++i], NULL, 10);
        } else {
            printf("未知参数：%s\n", argv[i]);
            return 2;
        }
    }
    if (g_seconds <= 0) {
        g_seconds = 1;
    }
    g_rng = g_seed ? g_seed : 1u;

    printf("=== test_soak_asan：纯 C 层长跑 / 随机序列（ASan 覆盖承载者）===\n");
    printf("  模式：%s   时长：%ds   seed：%llu\n",
           g_fuzz ? "fuzz（随机序列）" : "确定序列",
           g_seconds, (unsigned long long)g_seed);

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    if (lvglcj_null_init(320, 240, LV_COLOR_FORMAT_RGB565, 0) != LVGLCJ_OK) {
        printf("  [FAIL] headless display 初始化\n");
        return 1;
    }

    /* ---------------------------------------------------------- 基线 */
    printf("\n-- 0. 基线（负载开始之前）--\n");
    /*
     * ★ 必须先主动取一次 screen_active()，再采基线。
     *
     * 屏幕句柄是**惰性登记**的：第一次 screen_active() 才把它登进句柄表。
     * 若在它之前采基线，循环里第一次取屏幕就会让 ALIVE +1，
     * 看起来像"删了对象但句柄没回收"。
     *
     * 实测踩到过：现象是 alive **恒定** 1→2、**不随轮数增长** ——
     * 恒定正说明它不是泄漏（泄漏会累积），而是基线取早了。
     * 这类「harness 自己的错」最容易被误读成产品缺陷，
     * 而且它长得和真缺陷一模一样（都是 ALIVE 漂移）。
     */
    int64_t screen = lvglcj_screen_active();
    CHECK(screen != LVGLCJ_HANDLE_NULL, "活动屏幕可用（并完成句柄惰性登记）");

    snapshot_baseline();
    printf("  ALIVE 句柄 = %d，LVGL 对象 = %d，anim_ctx = %d\n",
           g_base_alive, g_base_obj, lvglcj_anim_ctx_count());
    CHECK(g_base_obj > 0, "基线上已有对象（屏幕/层），因此对账必须用差值而非绝对值");

    /* ---------------------------------------------------------- 负载 */
    uint64_t deadline = now_ms() + (uint64_t)g_seconds * 1000u;
    uint64_t steps = 0;

    printf("\n-- 1. %s --\n", g_fuzz ? "随机序列负载" : "确定性负载");
    if (g_fuzz) {
        fuzz_loop(deadline, &steps);
        printf("  完成 %llu 步（seed=%llu，可用 --seed 复现同一条序列）\n",
               (unsigned long long)steps, (unsigned long long)g_seed);
    } else {
        int round = 0;
        uint64_t prog_ms = now_ms();
        while (now_ms() < deadline) {
            maybe_progress(&prog_ms, steps, "确定");
            one_unit(round);
            steps++;
            /* 每 200 轮做一次完整对账（逐步对账太慢，且失败信息会刷屏） */
            if (round % 200 == 0) {
                check_steady_state("确定序列", 0);
            }
            round++;
        }
        printf("  完成 %llu 轮（每轮：建容器+子对象 → 挂样式 → 建动画 → 全部拆掉）\n",
               (unsigned long long)steps);
    }

    /* ---------------------------------------------------------- 稳态对账（末态） */
    printf("\n-- 2. ★ 末态稳态对账 --\n");
    (void)lvglcj_timer_handler();
    check_steady_state("末态", 1);

    /* 末态必须是干净的：句柄表也不能有保留量异常增长
       （INVALIDATED 有界回收在 handle_table.c，上限 LVGLCJ_INVALIDATED_KEEP_MAX） */
    {
        int32_t inv = lvglcj_handle_count(LVGLCJ_HSTATE_INVALIDATED);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "INVALIDATED 保留量 %d 未超过上限 %d（有界回收生效）",
                 inv, LVGLCJ_INVALIDATED_KEEP_MAX);
        CHECK(inv <= LVGLCJ_INVALIDATED_KEEP_MAX, buf);
    }

    /* ---------------------------------------------------------- 驱动覆盖 */
    /*
     * ★ 这几条断言的是「本用例**真的走到了有意思的路径**」。
     *
     * 一个长跑用例最危险的失败模式不是 FAIL，而是**什么都没测却全绿**：
     *   · 样式引用守卫从未被触发 → 说明走的都是"没有引用"的平凡路径，
     *     而 UAF 恰恰只发生在"有引用"的那条路上；
     *   · 动画从未创建成功 → anim_ctx 的生命周期根本没被驱动，
     *     "anim_ctx=0" 只证明"没建过"，不证明"建了能回收"。
     * 所以「计数为 0」必须配上「确实建过」才有意义。
     */
    printf("\n-- 3. 驱动覆盖（证明真的走到了有意思的路径）--\n");
    CHECK(steps > 0, "至少完成一轮负载");
    CHECK(g_text_ok > 0, "label 文本路径被驱动过");
    CHECK(g_anims_made > 0, "anim_ctx 生命周期被驱动过（建 → 配 → start 前删）");
    CHECK(g_guard_rejected > 0,
          "★ 样式引用守卫**拒绝过**释放（否则测的是「无引用」的平凡路径）");

    /* ---------------------------------------------------------- 收尾 */
    printf("\n-- 4. 收尾 --\n");
    CHECK(lvglcj_null_deinit() == LVGLCJ_OK, "反初始化 headless 后端");
    CHECK(lvglcj_deinit() == LVGLCJ_OK, "反初始化运行时");

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_checks, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
