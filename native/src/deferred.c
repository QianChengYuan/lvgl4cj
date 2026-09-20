/*
 * deferred.c —— 延迟删除 / 延迟注销队列（设计文档 §3.8.2）
 *
 * 为什么必须有它：
 *   回调执行期间直接删除对象会造成重入崩溃（use-after-free）——
 *   事件分发还在遍历这条对象链，对象却已被释放（R7）。
 *   所以「回调中 close()」不立即生效，而是入队 + 标记 pending，
 *   由主循环在同一帧的 drain 里真正执行。
 *
 * 语义（写入用户文档，§3.8.2）：
 *   回调中调 obj.close() **不会立即生效**，但对象立刻进入「待删除」状态；
 *   实际删除最迟在同一帧内完成。
 *
 * 防饥饿（次生残留处置 #1）：
 *   单帧最多处理 64 轮；超限则剩余项留到下一帧；
 *   若**连续 3 帧**都超限，则强制同步清空并报 DEFERRED_LOOP(-15)，
 *   避免「永远删不完」导致队列持续增长（R19）。
 */
#include "lvglcj_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define LVGLCJ_DEFER_MAX_ITER 64
#define LVGLCJ_DEFER_OVERRUN_LIMIT 3
#define LVGLCJ_DEFER_INIT_CAP 32

typedef struct {
    int32_t op;
    int64_t handle;
    int32_t cid;
} defer_item_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static defer_item_t   *g_items = NULL;
static int32_t         g_cap = 0;
static int32_t         g_head = 0; /* FIFO 头 */
static int32_t         g_len = 0;

/* 连续超限帧计数（只在 LVGL 线程访问，但保持原子以便诊断接口安全读取） */
static _Atomic int32_t g_overrun_frames = 0;
/* 累计因超限而被迫强制的次数（可观测性：正常应为 0） */
static _Atomic int32_t g_forced_drains = 0;

int32_t lvglcj_deferred_init(void)
{
    pthread_mutex_lock(&g_lock);
    free(g_items);
    g_items = (defer_item_t *)calloc(LVGLCJ_DEFER_INIT_CAP, sizeof(defer_item_t));
    if (g_items == NULL) {
        g_cap = 0;
        pthread_mutex_unlock(&g_lock);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "延迟队列初始分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }
    g_cap = LVGLCJ_DEFER_INIT_CAP;
    g_head = 0;
    g_len = 0;
    atomic_store(&g_overrun_frames, 0);
    atomic_store(&g_forced_drains, 0);
    pthread_mutex_unlock(&g_lock);
    return LVGLCJ_OK;
}

void lvglcj_deferred_destroy(void)
{
    pthread_mutex_lock(&g_lock);
    free(g_items);
    g_items = NULL;
    g_cap = 0;
    g_head = 0;
    g_len = 0;
    pthread_mutex_unlock(&g_lock);
}

/* 调用方必须持有锁 */
static int defer_ensure_unlocked(void)
{
    if (g_head + g_len < g_cap) {
        return 0;
    }
    /* 前面已消费的空间先回收（把有效区间搬到数组开头） */
    if (g_head > 0) {
        memmove(g_items, g_items + g_head, (size_t)g_len * sizeof(defer_item_t));
        g_head = 0;
        if (g_len < g_cap) {
            return 0;
        }
    }
    int32_t new_cap = (g_cap == 0) ? LVGLCJ_DEFER_INIT_CAP : g_cap * 2;
    defer_item_t *fresh = (defer_item_t *)realloc(g_items, (size_t)new_cap * sizeof(defer_item_t));
    if (fresh == NULL) {
        return -1;
    }
    g_items = fresh;
    g_cap = new_cap;
    return 0;
}

int32_t lvglcj_deferred_push(int32_t op, int64_t handle, int32_t cid)
{
    pthread_mutex_lock(&g_lock);

    if (defer_ensure_unlocked() != 0) {
        pthread_mutex_unlock(&g_lock);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, handle, cid, __func__,
                            "延迟队列入队失败（扩容失败）");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    defer_item_t *it = &g_items[g_head + g_len];
    it->op = op;
    it->handle = handle;
    it->cid = cid;
    g_len++;

    pthread_mutex_unlock(&g_lock);
    return LVGLCJ_OK;
}

int32_t lvglcj_deferred_count(void)
{
    pthread_mutex_lock(&g_lock);
    int32_t n = g_len;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* 调用方必须持有锁；队列空返回 0 */
static int defer_pop_unlocked(defer_item_t *out)
{
    if (g_len <= 0) {
        return 0;
    }
    *out = g_items[g_head];
    g_head++;
    g_len--;
    if (g_len == 0) {
        g_head = 0; /* 空队列直接复位，避免 head 无限增长 */
    }
    return 1;
}

/* 执行一项：真正的删除/注销动作在这里发生 */
static void defer_apply(const defer_item_t *it)
{
    switch (it->op) {
        case LVGLCJ_DEFER_DELETE:
            (void)lvglcj_obj_delete_now(it->handle);
            break;
        case LVGLCJ_DEFER_UNREG:
            /* 通知仓颉侧注销闭包（C 侧不持有闭包实体） */
            lvglcj_closure_unregister(it->cid);
            break;
        default:
            lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, it->handle, it->cid,
                                __func__, "未知的延迟操作类型");
            break;
    }
}

void lvglcj_drain_deferred(void)
{
    defer_item_t item;
    int iter = 0;
    int drained = 0;

    pthread_mutex_lock(&g_lock);

    while (defer_pop_unlocked(&item)) {
        if (++iter > LVGLCJ_DEFER_MAX_ITER) {
            /*
             * 超过单帧上限：把刚取出的一项放回队首，剩余留到下一帧。
             * 放回是为了不丢项 —— 未执行的删除必须保持「待删除」语义。
             */
            if (g_head > 0) {
                g_head--;
                g_items[g_head] = item;
                g_len++;
            }
            pthread_mutex_unlock(&g_lock);

            int32_t frames = atomic_fetch_add(&g_overrun_frames, 1) + 1;
            lvglcj_record_error(LVGLCJ_ERR_DEFERRED_LOOP, 0, 0, __func__,
                                "延迟删除单帧超过 64 轮，剩余项留到下一帧");

            if (frames >= LVGLCJ_DEFER_OVERRUN_LIMIT) {
                /* 连续 3 帧超限 → 强制同步清空，避免队列无限增长（R19） */
                atomic_store(&g_overrun_frames, 0);
                atomic_fetch_add(&g_forced_drains, 1);

                pthread_mutex_lock(&g_lock);
                while (defer_pop_unlocked(&item)) {
                    pthread_mutex_unlock(&g_lock);
                    defer_apply(&item);
                    pthread_mutex_lock(&g_lock);
                }
                pthread_mutex_unlock(&g_lock);
            }
            return;
        }

        pthread_mutex_unlock(&g_lock);
        defer_apply(&item);
        pthread_mutex_lock(&g_lock);
        drained++;
    }

    pthread_mutex_unlock(&g_lock);

    /* 本轮正常清空 → 超限计数归零 */
    if (drained > 0) {
        atomic_store(&g_overrun_frames, 0);
    }
}

/* ---------------------------------------------------------- 可观测性 */
/* 正常长跑中应为 0；非 0 说明存在「连续多帧删不完」的异常场景 */
int32_t lvglcj_deferred_forced_drains(void)
{
    return atomic_load(&g_forced_drains);
}
