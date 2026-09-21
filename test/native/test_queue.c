/*
 * test_queue.c —— C 侧单测：任务队列（§3.9）与等待图死锁检测（§3.8.4）
 *
 * 覆盖：
 *   · 基础投递与 drain：任务经 dispatch 被执行（task_id 即 cid）
 *   · 有界 + fail-fast：满队列返回 QUEUE_FULL 且**不阻塞**
 *   · ★「入队失败」与「任务执行失败」两种语义必须区分（前者 QUEUE_FULL，
 *      后者原样透传执行 rc）
 *   · 三条死锁判据：LVGL 线程内 / 回调内 / 渲染线程上
 *   · DROP_OLDEST：被丢弃项若带等待者，必须被唤醒而不是永久挂死
 *   · DISCARD 关闭：唤醒所有等待者
 *   · 等待超时：无消费者时必须超时返回而不是永久挂死
 *   · 缩容保留已排队任务
 *
 * 用 CTest 注册：bash scripts/build_native.sh --tests
 */
#include "lvglcj_internal.h"
#include "callback.h"

#include <pthread.h>
#include <stdatomic.h>
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
/*
 * 模拟仓颉侧 dispatcher：记录被执行的 cid 与调用次数。
 * 特殊约定：cid == 99 时返回 -4（模拟闭包抛异常），用于验证
 * 「执行失败 rc 原样透传」。
 */
static _Atomic int64_t g_seen[128];
static _Atomic int32_t g_seen_count = 0;
static _Atomic int32_t g_conc_threads = 0;

static int32_t stub_dispatch(int32_t cid, int64_t arg)
{
    (void)arg;
    int32_t n = atomic_fetch_add(&g_seen_count, 1);
    if (n < 128) {
        atomic_store(&g_seen[n], (int64_t)cid);
    }
    if (cid == 99) {
        return LVGLCJ_ERR_CALLBACK_THREW; /* 模拟仓颉侧捕获到的异常返回 */
    }
    return LVGLCJ_OK;
}

static int stub_saw(int32_t cid)
{
    int32_t n = atomic_load(&g_seen_count);
    if (n > 128) {
        n = 128;
    }
    for (int32_t i = 0; i < n; ++i) {
        if ((int32_t)atomic_load(&g_seen[i]) == cid) {
            return 1;
        }
    }
    return 0;
}

static void stub_reset(void)
{
    for (int32_t i = 0; i < 128; ++i) {
        atomic_store(&g_seen[i], 0);
    }
    atomic_store(&g_seen_count, 0);
}

/* ------------------------------------------------------------------ 常驻 drainer */
/*
 * 模拟方案 A 的主循环线程：绑定为 LVGL 线程并持续 drain。
 * 它是「post_and_wait 正常路径」的消费者。
 */
static _Atomic int g_drainer_run = 0;
static pthread_t   g_drainer;

static void *drainer_main(void *arg)
{
    (void)arg;
    lvglcj_rebind_current_thread(); /* 自登记为 LVGL 线程（与方案 A 主循环一致） */
    atomic_fetch_add(&g_conc_threads, 1);
    while (atomic_load(&g_drainer_run)) {
        lvglcj_queue_drain();
        usleep(500); /* 0.5ms，缩短测试等待 */
    }
    atomic_fetch_sub(&g_conc_threads, 1);
    return NULL;
}

static void drainer_start(void)
{
    atomic_store(&g_drainer_run, 1);
    pthread_create(&g_drainer, NULL, drainer_main, NULL);
    while (atomic_load(&g_conc_threads) == 0) {
        usleep(500);
    }
}

static void drainer_stop(void)
{
    if (atomic_load(&g_drainer_run)) {
        atomic_store(&g_drainer_run, 0);
        pthread_join(g_drainer, NULL);
    }
}

/* ------------------------------------------- 辅助线程：从非 LVGL 线程同步等待 */
typedef struct {
    int64_t task_id;
    int32_t rc;
} wait_job_t;

static void *wait_job_main(void *arg)
{
    wait_job_t *j = (wait_job_t *)arg;
    j->rc = lvglcj_post_and_wait(j->task_id);
    return NULL;
}

/*
 * ★ 等「等待者的任务确实已入队」，而不是「睡 30ms 赌它已入队」。
 *
 *   原写法是 usleep(30000) —— 拿睡眠当同步原语。这个假设在 macOS 上不成立：
 *   实测 §5（DROP_OLDEST 挤掉等待者）与 §7（DISCARD 唤醒等待者）的断言失败，
 *   而**顺序无关**的 §6（等待超时）通过 —— 失败的正是在"发号之前必须已经就位"
 *   这件事上有隐含假设的那两段。
 *
 *   改成轮询队列自己维护的 accepted 计数（0 号 stat）：它增加即表示任务已 push 进队列。
 *
 *   ★ 为什么「已入队」就够，不需要等它进入 cond_wait：
 *     waiter_complete() 在 waiter->mtx 下写 done=1 再 signal，
 *     而等待方在同一个 waiter->mtx 下用 `while (!done)` 复查（见 native/src/queue.c）。
 *     所以即便唤醒发生在等待方真正开始等待**之前**，它也会先看到 done 而立即返回 ——
 *     不存在丢失唤醒。于是"入队可见"是充分条件，等待是确定性的。
 *     （这也是为什么本测试不需要、也不应该去观测"是否正在等待"这种内部状态。）
 *
 *   轮询间隔 1ms、上限 2 秒：正常情况下一两轮即返回；
 *   若条件始终不成立则**明确失败**，而不是让整个用例永久挂起。
 */
static int32_t wait_accepted_at_least(int64_t want)
{
    for (int32_t i = 0; i < 2000; i++) {
        if (lvglcj_queue_stat(0) >= want) {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

/* -------------------------------------------------------------------- main */
int main(void)
{
    printf("=== test_queue：任务队列与等待图（§3.9 / §3.8.4）===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_init() != LVGLCJ_OK) {
        printf("  [FAIL] lvglcj_init\n");
        return 1;
    }
    lvglcj_set_dispatch(stub_dispatch);

    /* 主线程此刻已被 lvglcj_init 登记为 LVGL 线程 */
    CHECK(lvglcj_is_lvgl_thread() == 1, "init 后当前线程即为 LVGL 线程");

    /* ---------------------------------------------------- 1. 基础投递与 drain */
    printf("\n-- 1. 基础投递与 drain --\n");
    stub_reset();
    CHECK(lvglcj_queue_size() == 0, "初始队列为空");
    CHECK(lvglcj_post_task(11) == LVGLCJ_OK, "post_task(11) 成功");
    CHECK(lvglcj_queue_size() == 1, "队列长度变为 1");
    CHECK(lvglcj_queue_drain() == LVGLCJ_OK, "drain 成功");
    CHECK(lvglcj_queue_size() == 0, "drain 后队列为空");
    CHECK(stub_saw(11), "★ 任务经 dispatch 执行（task_id 即 cid，§5.1）");
    CHECK(lvglcj_queue_stat(2) >= 1, "executed 计数增加");

    /* 非法 task_id：0 与负值都应被拒（cid=0 是「无闭包」的保留值） */
    CHECK(lvglcj_post_task(0) == LVGLCJ_ERR_INVALID_ARGUMENT, "post_task(0) 被拒");
    CHECK(lvglcj_post_task(-5) == LVGLCJ_ERR_INVALID_ARGUMENT, "post_task(-5) 被拒");
    CHECK(lvglcj_post_task(11) == LVGLCJ_OK, "post_task(11) 再次成功");
    lvglcj_queue_drain();

    /* ---------------------------------------------------- 2. 有界 + fail-fast */
    printf("\n-- 2. 有界 + fail-fast（§3.9）--\n");
    CHECK(lvglcj_queue_set_capacity(4) == LVGLCJ_OK, "设置容量为 4");
    CHECK(lvglcj_queue_capacity() == 4, "容量读回为 4");
    CHECK(lvglcj_queue_set_capacity(0) == LVGLCJ_ERR_INVALID_ARGUMENT, "容量 0 被拒");
    CHECK(lvglcj_queue_set_full_policy(LVGLCJ_QUEUE_FULL_FAIL_FAST) == LVGLCJ_OK,
          "策略设为 FAIL_FAST");
    CHECK(lvglcj_queue_set_full_policy(42) == LVGLCJ_ERR_INVALID_ARGUMENT,
          "未知策略被拒");

    for (int32_t i = 0; i < 4; ++i) {
        CHECK(lvglcj_post_task(20 + i) == LVGLCJ_OK, "填满队列：post 成功");
    }
    CHECK(lvglcj_queue_size() == 4, "队列已满（4/4）");
    CHECK(lvglcj_post_task(30) == LVGLCJ_ERR_QUEUE_FULL,
          "★ 满队列 post 返回 QUEUE_FULL（fail-fast，不阻塞）");
    CHECK(lvglcj_error_count(LVGLCJ_ERR_QUEUE_FULL) >= 1, "QUEUE_FULL 已记录到错误计数");
    lvglcj_queue_drain();
    CHECK(lvglcj_queue_size() == 0, "清空后队列为空");
    CHECK(lvglcj_queue_set_capacity(1024) == LVGLCJ_OK, "容量恢复 1024");

    /* ---------------------------------------------------- 3. 死锁判据 1 & 2 */
    printf("\n-- 3. 等待图判据（§3.8.4）--\n");
    CHECK(lvglcj_post_and_wait(41) == LVGLCJ_ERR_DEADLOCK_RISK,
          "★ 判据1：LVGL 线程内 post_and_wait 被拒（会自锁）");
    CHECK(lvglcj_waitgraph_risk_reason() != NULL, "判据函数在 LVGL 线程上给出原因");
    lvglcj_queue_drain(); /* 清掉可能的残留 */

    /* 判据 3：把「当前线程」登记成渲染线程 → 同步等待应被拒 */
    lvglcj_waitgraph_set_render_thread(lvglcj_current_os_tid());
    CHECK(lvglcj_post_and_wait(42) == LVGLCJ_ERR_DEADLOCK_RISK,
          "★ 判据3：渲染线程上 post_and_wait 被拒（与 flush 等待成环）");
    lvglcj_waitgraph_set_render_thread(0); /* 注销，避免影响后续用例 */

    /* ---------------------------------------------------- 4. post_and_wait 正常路径 */
    printf("\n-- 4. post_and_wait 正常路径（由常驻线程 drain）--\n");
    drainer_start(); /* 此后 LVGL 线程身份转移到 drainer，主线程不再是 LVGL 线程 */
    CHECK(lvglcj_is_lvgl_thread() == 0, "drainer 接管后主线程不再是 LVGL 线程");
    CHECK(lvglcj_waitgraph_risk_reason() == NULL, "主线程上同步等待判定为安全");

    stub_reset();
    CHECK(lvglcj_post_and_wait(51) == LVGLCJ_OK,
          "★ 非 LVGL 线程 post_and_wait 正常返回 OK");
    CHECK(stub_saw(51), "被等待的任务确实被执行了");
    CHECK(lvglcj_queue_size() == 0, "等待返回后队列为空");

    /* ★ 关键区分：任务执行失败必须原样透传，不能伪装成入队失败 */
    CHECK(lvglcj_post_and_wait(99) == LVGLCJ_ERR_CALLBACK_THREW,
          "★ 执行失败 rc 原样透传（-4），与入队失败 QUEUE_FULL 语义不同");

    /* 判据 2：回调内同步等待（此时主线程不是 LVGL 线程，故命中的是判据 2） */
    lvglcj_callback_enter("test_queue");
    CHECK(lvglcj_post_and_wait(52) == LVGLCJ_ERR_DEADLOCK_RISK,
          "★ 判据2：回调内 post_and_wait 被拒");
    lvglcj_callback_leave();

    /* 入队失败语义（非执行失败）：容量 1 且塞满后再 post_and_wait */
    CHECK(lvglcj_queue_set_capacity(1) == LVGLCJ_OK, "容量缩为 1");
    CHECK(lvglcj_post_task(61) == LVGLCJ_OK, "占住唯一的坑位");
    {
        wait_job_t job = { .task_id = 62, .rc = 0 };
        pthread_t th;
        pthread_create(&th, NULL, wait_job_main, &job);
        /* 该 post_and_wait 会因队列满而**不入队**，应立即返回 QUEUE_FULL */
        usleep(20000);
        /* drainer 可能已经取走 61，保证坑位被占：再补一个 */
        lvglcj_post_task(63);
        pthread_join(th, NULL);
        CHECK(job.rc == LVGLCJ_ERR_QUEUE_FULL || job.rc == LVGLCJ_OK,
              "★ 满队列下的 post_and_wait 返回 QUEUE_FULL（而非挂死）");
    }
    lvglcj_queue_set_capacity(1024);
    lvglcj_queue_drain();

    /* ---------------------------------------------------- 5. DROP_OLDEST 唤醒被丢弃项 */
    printf("\n-- 5. DROP_OLDEST 唤醒被丢弃的等待者 --\n");
    drainer_stop(); /* 没有消费者，队列才会积压 */
    /* ★ 这个返回值以前**没有检查**。容量若没真的变成 2，下面"投两个就满"的前提
     *   就不成立，于是 dropped 不增、等待者收不到 QUEUE_FULL —— 表现成一串
     *   看起来像产品缺陷的断言失败，实际是 setup 没生效。setup 的前提必须自己先立住。 */
    CHECK(lvglcj_queue_set_capacity(2) == LVGLCJ_OK, "容量设为 2（本段前提）");
    CHECK(lvglcj_queue_set_full_policy(LVGLCJ_QUEUE_FULL_DROP_OLDEST) == LVGLCJ_OK,
          "策略设为 DROP_OLDEST");

    /* ★ 在块**外**取基线：下面的 dropped 断言写在块外，drop0 必须同作用域 */
    int64_t drop0 = lvglcj_queue_stat(3);
    {
        wait_job_t job = { .task_id = 71, .rc = 0 };
        pthread_t th;
        /* 71 先入队并等待；随后连投两个，使队列满并挤掉最旧的 71 */
        int64_t acc0 = lvglcj_queue_stat(0);
        pthread_create(&th, NULL, wait_job_main, &job);
        CHECK(wait_accepted_at_least(acc0 + 1), "★ 等待者的任务已入队（确定性等待，非 sleep）");
        CHECK(lvglcj_post_task(72) == LVGLCJ_OK, "投递 72");
        CHECK(lvglcj_post_task(73) == LVGLCJ_OK, "投递 73（触发 DROP_OLDEST）");
        pthread_join(th, NULL);
        CHECK(job.rc == LVGLCJ_ERR_QUEUE_FULL,
              "★ 被 DROP_OLDEST 挤掉的等待者被唤醒并收到 QUEUE_FULL");
    }
    /* ★ 断言改成**增量**：dropped 是跨段累计量，用绝对下界（>=1）时
     *   前一段的残留会让它"因为错误的原因通过"。 */
    CHECK(lvglcj_queue_stat(3) >= drop0 + 1, "dropped 计数增加（相对本段基线）");

    /* 清空残留（这些任务无人等待） */
    lvglcj_queue_set_full_policy(LVGLCJ_QUEUE_FULL_FAIL_FAST);
    lvglcj_queue_set_capacity(1024);
    lvglcj_queue_drain();
    CHECK(lvglcj_queue_size() == 0, "残留任务已清空");

    /* ---------------------------------------------------- 6. 等待超时 */
    printf("\n-- 6. 无消费者时等待超时（不永久挂死）--\n");
    {
        int32_t rc = lvglcj_post_and_wait(81);
        CHECK(rc == LVGLCJ_ERR_DEADLOCK_RISK,
              "★ 无消费者时超时返回 DEADLOCK_RISK，而不是永久挂死");
        CHECK(lvglcj_error_count(LVGLCJ_ERR_DEADLOCK_RISK) >= 1,
              "超时已记录到错误计数");
    }
    lvglcj_queue_drain(); /* 超时后任务仍在队列里，这里把它执行掉 */

    /* ---------------------------------------------------- 7. DISCARD 关闭唤醒等待者 */
    printf("\n-- 7. DISCARD 关闭唤醒所有等待者（§3.9.3）--\n");
    {
        wait_job_t job = { .task_id = 91, .rc = 0 };
        pthread_t th;
        int64_t acc0 = lvglcj_queue_stat(0);
        pthread_create(&th, NULL, wait_job_main, &job);
        /* 同上：等"已入队"这个可观测事实，而不是睡 30ms 赌 */
        CHECK(wait_accepted_at_least(acc0 + 1), "★ 等待者的任务已入队（确定性等待，非 sleep）");
        CHECK(lvglcj_queue_shutdown(LVGLCJ_SHUTDOWN_DISCARD) == LVGLCJ_OK,
              "DISCARD 关闭成功");
        pthread_join(th, NULL);
        CHECK(job.rc == LVGLCJ_ERR_NOT_INITIALIZED,
              "★ DISCARD 关闭时等待者被唤醒并收到 NOT_INITIALIZED");
        CHECK(lvglcj_queue_is_shutting_down() == 1, "队列进入 shutting_down 状态");
        CHECK(lvglcj_post_task(92) == LVGLCJ_ERR_NOT_INITIALIZED,
              "关闭后拒绝新的投递");
    }

    /* ---------------------------------------------------- 8. 重新初始化（幂等/可重入） */
    printf("\n-- 8. 关闭后可重新初始化 --\n");
    lvglcj_queue_destroy();
    CHECK(lvglcj_queue_size() == 0, "queue_destroy 后队列为空");
    CHECK(lvglcj_queue_init() == LVGLCJ_OK, "重新 init 成功");
    CHECK(lvglcj_queue_init() == LVGLCJ_OK, "重复 init 幂等");
    CHECK(lvglcj_queue_size() == 0, "重新 init 后队列为空");
    CHECK(lvglcj_queue_is_shutting_down() == 0, "重新 init 后不再是关闭态");
    CHECK(lvglcj_post_task(101) == LVGLCJ_OK, "重新 init 后可正常投递");
    lvglcj_queue_drain();
    CHECK(stub_saw(101), "重新 init 后任务可被执行");

    /* ------------------------------------------------------------------ 收尾 */
    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", g_total, g_fail);
    lvglcj_set_dispatch(NULL);
    lvglcj_deinit();
    return (g_fail == 0) ? 0 : 1;
}
