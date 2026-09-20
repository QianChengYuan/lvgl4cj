/*
 * thread.c —— 运行时与线程模型（设计文档 §3.6）
 *
 * 核心约束（ADR-003 / ADR-004）：
 *   LVGL 非线程安全，所有 lv_* 调用必须落在同一个 OS 线程。
 *   仓颉的 spawn 是 M:N 轻量级线程，可能被调度到不同 OS 线程，
 *   因此「LVGL 主循环跑在哪个线程」必须由 C 侧显式掌控，不能依赖仓颉线程语义。
 *
 * 两套方案（由 V1 探针结论二选一，共享同一份线程断言逻辑）：
 *   方案 A：C 侧 pthread_create 出独立 OS 线程跑主循环（lvglcj_start_thread）
 *   方案 C：不启线程，由仓颉主循环周期性调 lvglcj_pump()
 *
 * 本文件的 g_lvgl_thread_id 就是线程断言的唯一基准，Release 构建下**不关闭**：
 *   关闭它会把「跨线程数据竞争」变成上线后的间歇性崩溃（§D.5 禁止事项）。
 *
 * 【分阶段说明】当前主循环只做 tick/timer 处理。
 *   延迟删除 drain（§3.8.2）与任务队列 drain（§3.9）分别在 t3/t4 接入，
 *   接入点已在主循环中标注。探针（t1）不需要这两者。
 */
#include "lvglcj_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* ---------------------------------------------------------------- 线程身份 */
static pthread_t  g_lvgl_thread;
static int64_t    g_lvgl_thread_tid = 0;
static _Atomic int g_lvgl_thread_valid = 0;

static _Atomic int g_loop_running = 0;
static pthread_t   g_loop_thread;

/*
 * 显式把「当前线程」声明为 LVGL 线程。
 *
 * 两条用途，都是设计文档 §3.6.5 的直接推论：
 *   1. 方案 C（泵模式）：用户主循环所在线程必须自己声明身份，
 *      否则 g_lvgl_thread_id 停留在 lv_init 时的线程上，pump() 会被误拦。
 *   2. 测试与工具：仓颉的测试框架使用 M:N 轻量级线程，
 *      **不同的测试用例可能落在不同的 OS 线程上**（V3 探针已实测证明迁移存在）。
 *      若不在每个用例开头重新声明，LVGL 调用就会与创建者线程不同 → 数据竞争。
 *      （本项目实测症状：closure 测试间歇性 SIGSEGV，且同一用例两次运行结果不同。）
 *
 * 方案 A 下 **用户不应调用**：主循环线程会自行绑定，随意重绑会破坏线程断言的意义。
 */
int32_t lvglcj_rebind_current_thread(void)
{
    lvglcj_thread_bind_current();
    return LVGLCJ_OK;
}

int64_t lvglcj_current_os_tid(void)
{
#if defined(__linux__)
    /* 用内核线程号而非 pthread_t：pthread_t 可能被复用，且 V3 探针要比对的是 OS 线程 */
    return (int64_t)syscall(SYS_gettid);
#else
    return (int64_t)(uintptr_t)pthread_self();
#endif
}

void lvglcj_thread_bind_current(void)
{
    g_lvgl_thread = pthread_self();
    g_lvgl_thread_tid = lvglcj_current_os_tid();
    atomic_store(&g_lvgl_thread_valid, 1);
}

int32_t lvglcj_is_lvgl_thread(void)
{
    if (!atomic_load(&g_lvgl_thread_valid)) {
        return 0;
    }
    return (lvglcj_current_os_tid() == g_lvgl_thread_tid) ? 1 : 0;
}

int32_t lvglcj_check_lvgl_thread(const char *func)
{
    if (!atomic_load(&g_lvgl_thread_valid)) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_INITIALIZED, 0, 0, func,
                            "LVGL 线程尚未确定（runtime 未 start）");
        return LVGLCJ_ERR_NOT_INITIALIZED;
    }
    if (!lvglcj_is_lvgl_thread()) {
        lvglcj_record_error(LVGLCJ_ERR_WRONG_THREAD, 0, 0, func,
                            "必须在 LVGL 线程调用，请使用 runtime.post { } 投递");
        return LVGLCJ_ERR_WRONG_THREAD;
    }
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 主循环（方案 A） */
/*
 * 这是在「仓颉运行时不认识的 OS 线程」里执行的循环。
 * V1 探针正是要回答：这样一条线程能否安全地调用仓颉代码（见 probe_support.c）。
 * 若 V1 结论为否，本函数不会被启用，改走 lvglcj_pump（方案 C）。
 */
static void *lvglcj_loop_main(void *arg)
{
    (void)arg;

    /* 主循环线程自登记为 LVGL 线程 —— 后续所有 lv_* 调用都以它为准 */
    lvglcj_thread_bind_current();

    while (atomic_load(&g_loop_running)) {
        /*
         * 帧内顺序固定为：延迟删除 → 任务队列 → LVGL 定时器处理。
         * 理由：
         *   · 延迟删除最优先 —— 回调里 close() 的对象必须在同一帧内真的消失
         *     （§3.8.2「最迟同帧内生效」），否则该对象可能在本帧被再次绘制。
         *   · 任务队列次之 —— 任务通常是「改 UI」的请求，先落地再跑 timer_handler，
         *     这样本帧的渲染结果就包含这些改动，不会晚一帧。
         *   · lv_timer_handler 最后 —— 它内部会触发 refresh → flush。
         */
        lvglcj_drain_deferred();
        lvglcj_queue_drain();
        lv_timer_handler();

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)LVGLCJ_LOOP_SLEEP_MS * 1000L * 1000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int32_t lvglcj_start_thread(void)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }

    if (atomic_load(&g_loop_running)) {
        return LVGLCJ_OK; /* 幂等 */
    }

    atomic_store(&g_loop_running, 1);
    int err = pthread_create(&g_loop_thread, NULL, lvglcj_loop_main, NULL);
    if (err != 0) {
        atomic_store(&g_loop_running, 0);
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "pthread_create 失败");
        return LVGLCJ_ERR_BACKEND_FAILURE;
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_stop_thread(int32_t shutdown_mode)
{
    /*
     * 顺序很重要：
     *   1. 先置 g_loop_running=0 并 join，确保此后不再有人并发访问 LVGL。
     *      （join 之后，队列里残留的任务不会再有消费者。）
     *   2. 再按关闭策略处置队列 —— 必须**先停线程再清队列**，
     *      反过来的话主循环可能在清空过程中又取走任务，造成状态不确定。
     *   3. Discard 会唤醒所有 post_and_wait 等待者，避免它们挂到超时。
     */
    if (atomic_load(&g_loop_running)) {
        atomic_store(&g_loop_running, 0);
        pthread_join(g_loop_thread, NULL);
    }

    /*
     * 方案 C（泵模式）下没有主循环线程，队列里的任务由用户主循环驱动；
     * 此时 stop 也必须把队列处置掉，语义才与方案 A 一致。
     */
    return lvglcj_queue_shutdown(shutdown_mode);
}

/* ------------------------------------------------------------ 泵模式（方案 C） */
int32_t lvglcj_pump(void)
{
    /*
     * 方案 C 下没有独立线程，LVGL 线程 = 创建 runtime 的线程。
     * 这里的线程检查就是「防止跨仓颉线程调 pump()」的关键（§3.6.5 语义修正）。
     */
    int32_t rc = lvglcj_check_lvgl_thread(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    if (atomic_load(&g_loop_running)) {
        /* 方案 A 已启用独立线程，不应再手动泵 */
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    /*
     * 方案 C 下由仓颉主循环驱动，**帧内顺序必须与方案 A 完全一致**，
     * 否则两套线程方案的时序语义会出现差异，用户代码无法通用。
     */
    lvglcj_drain_deferred();
    lvglcj_queue_drain();
    return lvglcj_timer_handler();
}
/* 任务队列的投递/配置实现见 queue.c（post_task / post_and_wait /
 * queue_set_capacity / queue_set_full_policy / queue_size 均在那里）。 */
