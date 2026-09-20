/*
 * queue.c —— 跨线程任务队列（设计文档 §3.9）+ 等待图死锁检测（§3.8.4）
 *
 * ========================== 核心约定 ==========================
 * task_id 就是**闭包表的 closure_id**。
 *
 *   仓颉侧：let tid = registerClosure { _ => body() }   // 复用同一张表
 *           lvglcj_post_task(tid)
 *   C 侧  ：drain 时 lvglcj_call_closure(tid, 0)
 *
 * 依据 §5.1 原文：「task_id 由仓颉侧分配，C 侧只透传给 dispatch」。
 * 好处是**不需要第二套分发入口**、不需要额外的注册 API，
 * 并且任务与回调共用同一套「闭包生命周期 / 异常吞并 / 注销」机制。
 *
 * ==================== 为什么必须是有界 + fail-fast ====================
 * §3.9：队列满时**默认 fail-fast 抛 QueueFull，不阻塞**。
 *   阻塞入队会形成跨线程死锁链条：生产者在等队列空位，
 *   而消费者（LVGL 线程）可能正在等该生产者（例如渲染线程）完成某个操作。
 * 因此「入队失败」（返回 QUEUE_FULL）与「任务执行失败」（返回执行 rc）
 * 是**两种不同语义**，post_and_wait 必须分别如实返回，不可混为一谈。
 *
 * ==================== 三个并发安全要点 ====================
 * 1. drain 时**不持有队列锁执行任务**：任务内部再 post 是常见写法，
 *    持锁执行必然自锁。
 * 2. waiter 采用**堆分配 + 引用计数**（见下）。
 * 3. 关闭时**必须唤醒所有等待者**，否则 post_and_wait 永久挂死。
 *
 * --------------------- 关于 waiter 的引用计数（重要） ---------------------
 * 天真做法是把 waiter 放在 post_and_wait 的调用者栈上，但那样有竞态：
 *   调用者超时 → 准备返回并销毁栈上的 waiter
 *   与此同时 drainer 刚取出该任务 → 拿到 waiter 指针 → 写 done / signal
 *   → 写的是已失效的栈内存（use-after-free），随后 destroy 互斥量更是灾难。
 *
 * 因此 waiter 堆分配，并使引用计数：
 *   初始 refs = 2（调用者 1 + 「待执行」1），任一方用完即 release，
 *   归零时统一销毁互斥量/条件变量并 free。
 * 超时后调用者只 release 自己那一份，队列那一份仍由 drainer 在 signal 后 release。
 * signal 一个已无人等待的 waiter 是无害的（条件变量语义）。
 */
#include "lvglcj_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------- 可调参数 */

/* §3.9：默认容量 1024 */
#define LVGLCJ_QUEUE_DEFAULT_CAP 1024
#define LVGLCJ_QUEUE_MAX_CAP     (1 << 20)

/*
 * 单次 drain 最多执行多少个任务。
 * 取默认容量值：一帧内最多把当前排队的任务做完，又不会被无限生产者拖住
 * （剩余留到下一帧，LVGL 线程仍需为渲染保留时间片）。
 */
#define LVGLCJ_QUEUE_DRAIN_MAX   LVGLCJ_QUEUE_DEFAULT_CAP

/*
 * post_and_wait 的等待上限。
 * 必须有超时：无超时的等待会把「上游忘了 drain」变成永久挂死，
 * 而挂死的现象比一条明确的错误难定位得多。
 */
#define LVGLCJ_POST_WAIT_TIMEOUT_MS 5000

/* --------------------------------------------------------------- 数据结构 */

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             done;    /* 受 mtx 保护 */
    int32_t         rc;      /* 受 mtx 保护：任务执行结果 */
    _Atomic int32_t refs;    /* 引用计数：调用者 1 + 待执行 1 */
} lvglcj_waiter_t;

typedef struct {
    int64_t          task_id;
    lvglcj_waiter_t *waiter; /* NULL = 不等待（fire-and-forget） */
} lvglcj_queue_item_t;

typedef struct {
    pthread_mutex_t      mtx;
    pthread_cond_t       cond;
    lvglcj_queue_item_t *items;
    int32_t              cap;
    int32_t              head;
    int32_t              count;
    int32_t              full_policy;   /* lvglcj_queue_full_policy_t */
    int32_t              shutting_down; /* 关闭中：拒绝新任务并唤醒等待者 */
    /* 统计（诊断用；只观察，不参与控制流） */
    _Atomic int64_t      accepted;
    _Atomic int64_t      rejected;
    _Atomic int64_t      executed;
    _Atomic int64_t      dropped;
} lvglcj_queue_t;

static lvglcj_queue_t g_q = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .items = NULL,
    .cap = 0,
    .head = 0,
    .count = 0,
    .full_policy = LVGLCJ_QUEUE_FULL_FAIL_FAST,
    .shutting_down = 0,
};

/* ------------------------------------------------------- waiter 生命周期 */
/*
 * 锁序约定（全局唯一，违反即可能死锁）：
 *     g_q.mtx  →  waiter->mtx        （允许）
 *     waiter->mtx  →  g_q.mtx        （禁止）
 * 因此唤醒等待者的动作**一律在释放 g_q.mtx 之后**进行。
 */

static lvglcj_waiter_t *waiter_new(void)
{
    lvglcj_waiter_t *w = (lvglcj_waiter_t *)calloc(1, sizeof(lvglcj_waiter_t));
    if (w == NULL) {
        return NULL;
    }
    pthread_mutex_init(&w->mtx, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->done = 0;
    w->rc = LVGLCJ_OK;
    atomic_store(&w->refs, 2); /* 调用者 1 + 待执行 1 */
    return w;
}

/* 归还一份引用；归零则销毁并释放 */
static void waiter_release(lvglcj_waiter_t *w)
{
    if (w == NULL) {
        return;
    }
    if (atomic_fetch_sub(&w->refs, 1) == 1) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->mtx);
        free(w);
    }
}

/* 写入结果并唤醒（调用者**不得**持有 g_q.mtx） */
static void waiter_complete(lvglcj_waiter_t *w, int32_t rc)
{
    if (w == NULL) {
        return;
    }
    pthread_mutex_lock(&w->mtx);
    w->rc = rc;
    w->done = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mtx);
}

/* ============================================================ 等待图（§3.8.4） */
/*
 * 等待图检测要回答的问题是：
 *   「现在从这个线程同步等待 LVGL 线程执行任务，会不会形成等待环？」
 *
 * 三条成环路径，逐条对应下面一个判据：
 *   1. 在 LVGL 线程上等待自己 → 必然自锁
 *   2. 在回调里等待（回调本身就跑在 LVGL 线程上）→ 同上，且更隐蔽
 *   3. 在**渲染线程**上等待 → LVGL 线程执行任务时可能触发 flush，
 *      而 flush_wait_cb 需要渲染线程推进 → 渲染线程却在等任务 → 成环
 *
 * 判据 3 的渲染线程身份由后端注册（t5 SDL2 接入）；
 * 在 t5 之前 g_render_thread_tid == 0，该判据自动失效，不影响其它两条。
 */
static _Atomic int64_t g_render_thread_tid = 0;

void lvglcj_waitgraph_set_render_thread(int64_t tid)
{
    atomic_store(&g_render_thread_tid, tid);
}

int64_t lvglcj_waitgraph_render_thread(void)
{
    return atomic_load(&g_render_thread_tid);
}

/*
 * 返回非 0 表示「当前线程同步等待 LVGL 会成环」，返回值是给用户看的原因
 * （静态字符串，调用方不得 free）。返回 NULL 表示安全。
 */
const char *lvglcj_waitgraph_risk_reason(void)
{
    if (lvglcj_is_lvgl_thread()) {
        return "在 LVGL 线程内同步等待自身会死锁，请改用 postAsync";
    }
    if (lvglcj_callback_depth() > 0) {
        return "回调中同步等待会死锁，请改用 postAsync";
    }

    int64_t rtid = atomic_load(&g_render_thread_tid);
    if (rtid != 0 && rtid == lvglcj_current_os_tid()) {
        return "渲染线程同步等待会与 flush 等待成环，请改用 postAsync";
    }
    return NULL;
}

/* ============================================================ 生命周期 */

int32_t lvglcj_queue_init(void)
{
    pthread_mutex_lock(&g_q.mtx);

    if (g_q.items != NULL) {
        pthread_mutex_unlock(&g_q.mtx);
        return LVGLCJ_OK; /* 幂等 */
    }

    g_q.items = (lvglcj_queue_item_t *)calloc((size_t)LVGLCJ_QUEUE_DEFAULT_CAP,
                                              sizeof(lvglcj_queue_item_t));
    if (g_q.items == NULL) {
        pthread_mutex_unlock(&g_q.mtx);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "任务队列分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    g_q.cap = LVGLCJ_QUEUE_DEFAULT_CAP;
    g_q.head = 0;
    g_q.count = 0;
    g_q.full_policy = LVGLCJ_QUEUE_FULL_FAIL_FAST;
    g_q.shutting_down = 0;

    atomic_store(&g_q.accepted, 0);
    atomic_store(&g_q.rejected, 0);
    atomic_store(&g_q.executed, 0);
    atomic_store(&g_q.dropped, 0);

    pthread_mutex_unlock(&g_q.mtx);
    return LVGLCJ_OK;
}

void lvglcj_queue_destroy(void)
{
    /*
     * 先按「丢弃」语义清空并唤醒等待者，再释放内存。
     * 顺序不能反：若先 free(items) 再唤醒，被唤醒的线程可能仍在访问队列。
     */
    lvglcj_queue_shutdown(LVGLCJ_SHUTDOWN_DISCARD);

    pthread_mutex_lock(&g_q.mtx);
    free(g_q.items);
    g_q.items = NULL;
    g_q.cap = 0;
    g_q.head = 0;
    g_q.count = 0;
    pthread_mutex_unlock(&g_q.mtx);
}

/* ============================================================ 配置 */

int32_t lvglcj_queue_set_capacity(int32_t capacity)
{
    if (capacity < 1 || capacity > LVGLCJ_QUEUE_MAX_CAP) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "容量必须在 1..1048576 之间");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&g_q.mtx);

    if (g_q.items == NULL) {
        pthread_mutex_unlock(&g_q.mtx);
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, __func__,
                            "队列未初始化");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }

    if (capacity == g_q.cap) {
        pthread_mutex_unlock(&g_q.mtx);
        return LVGLCJ_OK;
    }

    lvglcj_queue_item_t *nitems =
        (lvglcj_queue_item_t *)calloc((size_t)capacity, sizeof(lvglcj_queue_item_t));
    if (nitems == NULL) {
        pthread_mutex_unlock(&g_q.mtx);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "扩容任务队列失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    /*
     * 保留已在排队的任务（按原顺序），而不是清空。
     * 缩容装不下时保留**最早**的若干个，并把被丢弃者的等待者唤醒报错 ——
     * 静默丢任务会让 post_and_wait 永久等待，是必须避免的失败模式。
     */
    int32_t keep = (g_q.count < capacity) ? g_q.count : capacity;
    for (int32_t i = 0; i < keep; ++i) {
        nitems[i] = g_q.items[(g_q.head + i) % g_q.cap];
    }

    /*
     * 收集被缩容挤出去的「孤儿等待者」。
     * 只需等到出锁后再唤醒，因此这里先取到指针数组；
     * 数组大小按上界 (count - keep) 分配，实际只填其中 waiter != NULL 的项。
     */
    lvglcj_waiter_t **orphans = NULL;
    int32_t n_orphan = 0;
    if (g_q.count > keep) {
        int32_t n_lost = g_q.count - keep;
        orphans = (lvglcj_waiter_t **)calloc((size_t)n_lost, sizeof(void *));
        if (orphans != NULL) {
            for (int32_t i = keep; i < g_q.count && n_orphan < n_lost; ++i) {
                lvglcj_queue_item_t it = g_q.items[(g_q.head + i) % g_q.cap];
                if (it.waiter != NULL) {
                    orphans[n_orphan++] = it.waiter;
                }
            }
        }
        atomic_fetch_add(&g_q.dropped, (int64_t)n_lost);
    }

    free(g_q.items);
    g_q.items = nitems;
    g_q.cap = capacity;
    g_q.head = 0;
    g_q.count = keep;

    pthread_mutex_unlock(&g_q.mtx);

    /* 出锁后唤醒孤儿等待者（锁序约定：禁止 waiter->mtx → g_q.mtx） */
    for (int32_t i = 0; i < n_orphan; ++i) {
        waiter_complete(orphans[i], LVGLCJ_ERR_QUEUE_FULL);
        waiter_release(orphans[i]);
    }
    free(orphans);

    if (n_orphan > 0) {
        lvglcj_record_error(LVGLCJ_ERR_QUEUE_FULL, 0, 0, __func__,
                            "缩容丢弃了已排队的任务，相关 postAndWait 已返回 QUEUE_FULL");
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_queue_set_full_policy(int32_t policy)
{
    if (policy != LVGLCJ_QUEUE_FULL_FAIL_FAST &&
        policy != LVGLCJ_QUEUE_FULL_BLOCK &&
        policy != LVGLCJ_QUEUE_FULL_DROP_OLDEST) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "未知的队列满策略");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&g_q.mtx);
    g_q.full_policy = policy;
    pthread_mutex_unlock(&g_q.mtx);
    return LVGLCJ_OK;
}

int32_t lvglcj_queue_size(void)
{
    pthread_mutex_lock(&g_q.mtx);
    int32_t n = g_q.count;
    pthread_mutex_unlock(&g_q.mtx);
    return n;
}

int32_t lvglcj_queue_capacity(void)
{
    pthread_mutex_lock(&g_q.mtx);
    int32_t c = g_q.cap;
    pthread_mutex_unlock(&g_q.mtx);
    return c;
}

int64_t lvglcj_queue_stat(int32_t which)
{
    switch (which) {
        case 0: return atomic_load(&g_q.accepted);
        case 1: return atomic_load(&g_q.rejected);
        case 2: return atomic_load(&g_q.executed);
        case 3: return atomic_load(&g_q.dropped);
        default: return -1;
    }
}

/* ============================================================ 入队 */
/*
 * 返回值：LVGLCJ_OK / LVGLCJ_ERR_QUEUE_FULL / LVGLCJ_ERR_NOT_INITIALIZED
 * 调用者**不得**持有 g_q.mtx。
 *
 * BLOCK 策略下会在本函数内阻塞等待空位；因此**禁止**在 LVGL 线程或回调中使用
 * （那会自锁）。调用方已做线程检查。
 */
static int32_t queue_push(int64_t task_id, lvglcj_waiter_t *waiter)
{
    lvglcj_waiter_t *drop_victim = NULL;

    pthread_mutex_lock(&g_q.mtx);

    if (g_q.items == NULL || g_q.shutting_down) {
        pthread_mutex_unlock(&g_q.mtx);
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, __func__,
                            "任务队列未运行（未初始化或已关闭）");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }

    if (g_q.count >= g_q.cap) {
        if (g_q.full_policy == LVGLCJ_QUEUE_FULL_FAIL_FAST) {
            atomic_fetch_add(&g_q.rejected, 1);
            pthread_mutex_unlock(&g_q.mtx);
            /* 不在此记录错误：调用方会带更完整的上下文记录一次 */
            return LVGLCJ_ERR_QUEUE_FULL;
        }
        if (g_q.full_policy == LVGLCJ_QUEUE_FULL_DROP_OLDEST) {
            lvglcj_queue_item_t old = g_q.items[g_q.head];
            g_q.head = (g_q.head + 1) % g_q.cap;
            g_q.count--;
            atomic_fetch_add(&g_q.dropped, 1);
            atomic_fetch_add(&g_q.rejected, 1);
            drop_victim = old.waiter;
        } else {
            /* BLOCK：等空位。shutting_down 时立即退出，避免永久挂住 */
            while (g_q.count >= g_q.cap && !g_q.shutting_down) {
                pthread_cond_wait(&g_q.cond, &g_q.mtx);
            }
            if (g_q.shutting_down) {
                pthread_mutex_unlock(&g_q.mtx);
                return LVGLCJ_ERR_NOT_INITIALIZED;
            }
        }
    }

    int32_t tail = (g_q.head + g_q.count) % g_q.cap;
    g_q.items[tail].task_id = task_id;
    g_q.items[tail].waiter = waiter;
    g_q.count++;
    atomic_fetch_add(&g_q.accepted, 1);

    pthread_cond_signal(&g_q.cond);
    pthread_mutex_unlock(&g_q.mtx);

    /* 出锁后处理被丢弃项的等待者（锁序约定） */
    if (drop_victim != NULL) {
        waiter_complete(drop_victim, LVGLCJ_ERR_QUEUE_FULL);
        waiter_release(drop_victim);
    }
    return LVGLCJ_OK;
}

/* ============================================================ drain */

int32_t lvglcj_queue_drain(void)
{
    int32_t n = 0;

    for (; n < LVGLCJ_QUEUE_DRAIN_MAX; ++n) {
        lvglcj_queue_item_t it;
        lvglcj_queue_item_t *slot;

        pthread_mutex_lock(&g_q.mtx);
        if (g_q.items == NULL) {
            pthread_mutex_unlock(&g_q.mtx);
            return LVGLCJ_ERR_NOT_INITIALIZED;
        }
        if (g_q.count == 0) {
            pthread_mutex_unlock(&g_q.mtx);
            break;
        }
        slot = &g_q.items[g_q.head];
        it = *slot;
        /* 清掉 waiter 引用，避免 slot 长期持有（也是防御性写法） */
        slot->waiter = NULL;
        slot->task_id = 0;
        g_q.head = (g_q.head + 1) % g_q.cap;
        g_q.count--;
        /* 空出了位置：唤醒可能在 BLOCK 上等待的生产者 */
        pthread_cond_signal(&g_q.cond);
        pthread_mutex_unlock(&g_q.mtx);

        /*
         * ★ 出锁执行。任务内部再 post 是常见写法，持锁执行必然自锁。
         * ★ 任务即闭包：task_id 就是 cid（§5.1）。
         */
        int32_t rc = lvglcj_call_closure((int32_t)it.task_id, 0);
        atomic_fetch_add(&g_q.executed, 1);

        if (it.waiter != NULL) {
            waiter_complete(it.waiter, rc);
            waiter_release(it.waiter); /* 归还「待执行」那一份引用 */
        }
    }

    if (n >= LVGLCJ_QUEUE_DRAIN_MAX && lvglcj_queue_size() > 0) {
        /* 有积压：本帧已尽力，剩余留到下一帧。持续出现说明生产者过快 */
        lvglcj_log(LVGLCJ_LOG_WARN, __func__,
                   "单帧任务数达到上限，剩余任务留待下一帧");
    }
    return LVGLCJ_OK;
}

/* ============================================================ 投递 API */

int32_t lvglcj_post_task(int64_t task_id)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    if (task_id <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, (int32_t)task_id,
                            __func__, "task_id 必须为正的闭包 id");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /*
     * BLOCK 策略下，若在 LVGL 线程/回调用阻塞入队会自锁；
     * 这种配置与用法组合必须在这里就被拦住，而不是等死锁发生。
     */
    if (lvglcj_is_lvgl_thread() || lvglcj_callback_depth() > 0) {
        pthread_mutex_lock(&g_q.mtx);
        int32_t is_block = (g_q.full_policy == LVGLCJ_QUEUE_FULL_BLOCK);
        pthread_mutex_unlock(&g_q.mtx);
        if (is_block) {
            lvglcj_record_error(LVGLCJ_ERR_DEADLOCK_RISK, 0, (int32_t)task_id,
                                __func__,
                                "BLOCK 策略禁止在 LVGL 线程/回调中投递（会自锁）");
            return LVGLCJ_ERR_DEADLOCK_RISK;
        }
    }

    rc = queue_push(task_id, NULL);
    if (rc == LVGLCJ_ERR_QUEUE_FULL) {
        /*
         * §3.9：入队失败必须让调用方明确知道，且**不得阻塞**。
         * 仓颉侧据此抛 QueueFull，而不是静默丢弃。
         */
        lvglcj_record_error(LVGLCJ_ERR_QUEUE_FULL, 0, (int32_t)task_id, __func__,
                            "任务队列已满（fail-fast 策略）");
    }
    return rc;
}

int32_t lvglcj_post_and_wait(int64_t task_id)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    if (task_id <= 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, (int32_t)task_id,
                            __func__, "task_id 必须为正的闭包 id");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /* §3.8.4 前置检查：成环则立即返回，不进入等待 */
    const char *risk = lvglcj_waitgraph_risk_reason();
    if (risk != NULL) {
        lvglcj_record_error(LVGLCJ_ERR_DEADLOCK_RISK, 0, (int32_t)task_id,
                            __func__, risk);
        return LVGLCJ_ERR_DEADLOCK_RISK;
    }

    lvglcj_waiter_t *w = waiter_new();
    if (w == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, (int32_t)task_id, __func__,
                            "等待者分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    rc = queue_push(task_id, w);
    if (rc != LVGLCJ_OK) {
        /* ★「入队失败」语义：任务根本没被执行 */
        if (rc == LVGLCJ_ERR_QUEUE_FULL) {
            lvglcj_record_error(LVGLCJ_ERR_QUEUE_FULL, 0, (int32_t)task_id,
                                __func__, "任务队列已满，任务未入队（未执行）");
        }
        /* 未入队 → 归还「待执行」那一份引用，再归还调用者那一份 */
        waiter_release(w);
        waiter_release(w);
        return rc;
    }

    /* 带超时等待；while 包裹以防虚假唤醒 */
    int timed_out = 0;
    pthread_mutex_lock(&w->mtx);
    while (!w->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += LVGLCJ_POST_WAIT_TIMEOUT_MS / 1000;
        ts.tv_nsec += (long)(LVGLCJ_POST_WAIT_TIMEOUT_MS % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&w->cond, &w->mtx, &ts) != 0) {
            timed_out = !w->done;
            break;
        }
    }
    int32_t exec_rc = w->rc;
    pthread_mutex_unlock(&w->mtx);

    /*
     * 无论成功、执行失败还是超时，调用者都只归还自己那一份引用。
     * 若任务仍在队列里，队列持有的那一份会在 drainer 执行后归还 ——
     * 因此不存在「释放了但 drainer 还要写」的窗口。
     */
    waiter_release(w);

    if (timed_out) {
        lvglcj_record_error(LVGLCJ_ERR_DEADLOCK_RISK, 0, (int32_t)task_id, __func__,
                            "等待任务执行超时（LVGL 主循环未在运行？）");
        return LVGLCJ_ERR_DEADLOCK_RISK;
    }

    /* ★ 执行失败原样向上传递：这是「任务执行失败」语义，与入队失败区分开 */
    return exec_rc;
}

/* ============================================================ 关闭 */

int32_t lvglcj_queue_shutdown(int32_t mode)
{
    if (mode == LVGLCJ_SHUTDOWN_DISCARD) {
        /*
         * 逐项取出并唤醒，而不是先收集到临时数组：
         * 数组大小需要跟随容量（可能远大于默认值），且持有队列锁唤醒会
         * 违反锁序约定。逐项处理天然安全。
         */
        int more = 1;
        while (more) {
            lvglcj_waiter_t *victim = NULL;

            pthread_mutex_lock(&g_q.mtx);
            g_q.shutting_down = 1;
            more = (g_q.count > 0);
            if (more) {
                lvglcj_queue_item_t it = g_q.items[g_q.head];
                g_q.head = (g_q.head + 1) % g_q.cap;
                g_q.count--;
                atomic_fetch_add(&g_q.dropped, 1);
                victim = it.waiter;
            }
            pthread_cond_broadcast(&g_q.cond); /* 唤醒 BLOCK 中的生产者 */
            pthread_mutex_unlock(&g_q.mtx);

            if (victim != NULL) {
                waiter_complete(victim, LVGLCJ_ERR_NOT_INITIALIZED);
                waiter_release(victim);
            }
        }
        lvglcj_log(LVGLCJ_LOG_INFO, __func__, "关闭策略 DISCARD：已丢弃未执行任务");
        return LVGLCJ_OK;
    }

    /*
     * Drain / DrainWithTimeout：把剩下的做完，等待者自然完成。
     * （DrainWithTimeout 的逐任务超时语义由 post_and_wait 内部具备；
     *   此处差别只是「要不要等多轮」，P0 统一按 Drain 处理并如实记录日志，
     *   真正的差异化留到 P1 随后端一起补。）
     */
    int32_t rc = lvglcj_queue_drain();
    lvglcj_log(LVGLCJ_LOG_INFO, __func__,
               (mode == LVGLCJ_SHUTDOWN_DRAIN_WITH_TIMEOUT)
                   ? "关闭策略 DRAIN_WITH_TIMEOUT：已清空队列"
                   : "关闭策略 DRAIN：已清空队列");

    pthread_mutex_lock(&g_q.mtx);
    g_q.shutting_down = 1;
    pthread_cond_broadcast(&g_q.cond);
    pthread_mutex_unlock(&g_q.mtx);
    return rc;
}

int32_t lvglcj_queue_is_shutting_down(void)
{
    pthread_mutex_lock(&g_q.mtx);
    int32_t s = g_q.shutting_down;
    pthread_mutex_unlock(&g_q.mtx);
    return s;
}
