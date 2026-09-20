/*
 * handle_table.c —— 四态句柄表（设计文档 §3.1，ADR-001）
 *
 * 为什么不用指针地址当句柄（ADR-001）：
 *   地址会被复用。对象 A 删除后其地址可能被对象 B 复用，
 *   此时「旧句柄 = A 的地址」查表会命中 B → 误判存活 → use-after-free。
 *   自增 ID 永不复用，从根本上消除这条路径。
 *
 * 数据结构：
 *   两张开放寻址哈希表 + 墓碑删除
 *     id  → entry   （主表：句柄 → 指针/状态/诊断信息）
 *     ptr → id      （反查表：原生指针 → 句柄，供 lv_event_get_target 等使用）
 *
 *   为什么两张表都需要：C 侧拿到的往往是裸指针（LVGL 回调给的就是指针），
 *   要转成句柄必须先反查；而仓颉侧拿到的永远是句柄，要拿指针走主表。
 *
 * ★ 语义要点：INVALIDATED 保留表项（含删除栈），RELEASED 才回收内存。
 *   这个区分是「句柄失效可检测」与「崩溃可定位」的来源。
 *   事件对象是**短命**的（每次事件分发创建一个），因此表项必须可回收，
 *   否则 24h soak 会无限增长。ID 不复用 + 槽位回收，两者同时满足。
 */
#include "handle_table.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HT_INIT_CAP  256u
#define HT_LOAD_NUM  7u  /* 装载因子分子：超过 7/10 即扩容或整理 */
#define HT_LOAD_DEN  10u /* 装载因子分母 */

typedef struct {
    int64_t id;   /* 0 = 空槽；-1 = 墓碑 */
    void   *ptr;
    uint8_t state;
    uint8_t pending;
    uint8_t delete_depth;
    char    name[LVGLCJ_HANDLE_NAME_MAX];
    char    delete_stack[LVGLCJ_DELETE_STACK_MAX][LVGLCJ_HANDLE_NAME_MAX];
} ht_entry_t;

typedef struct {
    void   *ptr; /* NULL = 空槽；HT_TOMB = 墓碑 */
    int64_t id;
} ht_ptr_slot_t;

/* 墓碑哨兵：ptr 不可能等于这个值（它是 0x1，且我们只登记真实对象指针） */
#define HT_TOMB_PTR ((void *)(uintptr_t)1)

static pthread_mutex_t g_ht_lock = PTHREAD_MUTEX_INITIALIZER;

static ht_entry_t   *g_id_slots = NULL;
static size_t        g_id_cap = 0;
static size_t        g_id_used = 0;   /* 含墓碑 */
static size_t        g_id_live = 0;   /* 不含墓碑 */

static ht_ptr_slot_t *g_ptr_slots = NULL;
static size_t         g_ptr_cap = 0;
static size_t         g_ptr_used = 0; /* 含墓碑 */
static size_t         g_ptr_live = 0; /* 不含墓碑 */

static int64_t g_next_id = 1; /* 单调递增，永不复用（ADR-001） */
/* 当前处于 INVALIDATED（保留表项）的数量，用于有界回收判断 */
static size_t        g_invalid_live = 0;

/* ------------------------------------------------------------ 哈希与探测 */
static size_t ht_hash_i64(int64_t v)
{
    uint64_t x = (uint64_t)v;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

static size_t ht_hash_ptr(const void *p)
{
    return ht_hash_i64((int64_t)(uintptr_t)p);
}

/* ------------------------------------------------------------ 主表：id 槽 */
static int ht_id_find_slot(int64_t id, size_t *out_index)
{
    size_t mask = g_id_cap - 1u;
    size_t i = ht_hash_i64(id) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        ht_entry_t *e = &g_id_slots[i];
        if (e->id == 0) {
            *out_index = i;
            return 0; /* 未找到，但 i 是可插入位置 */
        }
        if (e->id == id) {
            *out_index = i;
            return 1;
        }
        i = (i + 1u) & mask;
    }
    *out_index = 0;
    return -1; /* 满（理论上不会发生，因为装载因子受控） */
}

static int ht_id_rehash(size_t new_cap)
{
    ht_entry_t *old = g_id_slots;
    size_t old_cap = g_id_cap;

    ht_entry_t *fresh = (ht_entry_t *)calloc(new_cap, sizeof(ht_entry_t));
    if (fresh == NULL) {
        return -1;
    }

    g_id_slots = fresh;
    g_id_cap = new_cap;
    g_id_used = 0;
    g_id_live = 0;
    g_invalid_live = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].id == 0 || old[i].id == -1) {
            continue;
        }
        size_t idx = 0;
        (void)ht_id_find_slot(old[i].id, &idx);
        g_id_slots[idx] = old[i];
        g_id_used++;
        g_id_live++;
        if (old[i].state == (uint8_t)LVGLCJ_HSTATE_INVALIDATED) {
            g_invalid_live++;
        }
    }
    free(old);
    return 0;
}

static int ht_id_ensure(size_t need_live)
{
    if (g_id_slots == NULL) {
        return ht_id_rehash(HT_INIT_CAP);
    }
    /*
     * ★ 装载因子判据：used/cap >= 7/10 即扩容。
     *   写成 (used)*NUM >= cap*DEN 是**错的** ——
     *   那展开成 used >= cap*10/7 = 143%，也就是哈希表被塞满才扩容，
     *   下一次插入直接找不到空槽而失败。
     *   （此 bug 由 test_handle_table.c 的 5000 条压测抓出。）
     *   装载因子按「占用槽位（含墓碑）」计算，避免墓碑堆积导致探测退化。
     */
    if ((g_id_used + need_live + 1u) * HT_LOAD_DEN >= g_id_cap * HT_LOAD_NUM) {
        return ht_id_rehash(g_id_cap * 2u);
    }
    /* 墓碑占多数时原地整理，保证探测链短 */
    if (g_id_used > g_id_live * 2u && g_id_used > 32u) {
        return ht_id_rehash(g_id_cap);
    }
    return 0;
}

/* ------------------------------------------------------------ 反查表：ptr 槽 */
static int ht_ptr_find_slot(const void *p, size_t *out_index)
{
    size_t mask = g_ptr_cap - 1u;
    size_t i = ht_hash_ptr(p) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        ht_ptr_slot_t *s = &g_ptr_slots[i];
        if (s->ptr == NULL) {
            *out_index = i;
            return 0;
        }
        if (s->ptr == p) {
            *out_index = i;
            return 1;
        }
        i = (i + 1u) & mask;
    }
    *out_index = 0;
    return -1;
}

static int ht_ptr_rehash(size_t new_cap)
{
    ht_ptr_slot_t *old = g_ptr_slots;
    size_t old_cap = g_ptr_cap;

    ht_ptr_slot_t *fresh = (ht_ptr_slot_t *)calloc(new_cap, sizeof(ht_ptr_slot_t));
    if (fresh == NULL) {
        return -1;
    }

    g_ptr_slots = fresh;
    g_ptr_cap = new_cap;
    g_ptr_used = 0;
    g_ptr_live = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].ptr == NULL || old[i].ptr == HT_TOMB_PTR) {
            continue;
        }
        size_t idx = 0;
        (void)ht_ptr_find_slot(old[i].ptr, &idx);
        g_ptr_slots[idx] = old[i];
        g_ptr_used++;
        g_ptr_live++;
    }
    free(old);
    return 0;
}

/*
 * 调用方必须持有锁：把某个指针在反查表中的槽位置为墓碑，并维护 live 计数。
 * 抽出成函数是为了让 invalidate 与 release 两条路径的计数维护**必然一致**——
 * 这类「两处手写同一件事」的地方正是计数漂移的高发区。
 */
static void ht_ptr_tombstone(const void *p)
{
    if (g_ptr_slots == NULL || p == NULL) {
        return;
    }
    size_t pidx = 0;
    if (ht_ptr_find_slot(p, &pidx) == 1) {
        g_ptr_slots[pidx].ptr = HT_TOMB_PTR;
        g_ptr_slots[pidx].id = 0;
        if (g_ptr_live > 0) {
            g_ptr_live--;
        }
    }
}

static int ht_ptr_ensure(void)
{
    if (g_ptr_slots == NULL) {
        return ht_ptr_rehash(HT_INIT_CAP);
    }
    /* 与 ht_id_ensure 同理：used*DEN >= cap*NUM 才是「超过 70% 即扩容」 */
    if ((g_ptr_used + 1u) * HT_LOAD_DEN >= g_ptr_cap * HT_LOAD_NUM) {
        return ht_ptr_rehash(g_ptr_cap * 2u);
    }
    /* 墓碑占多数时原地整理，保证探测链短 */
    if (g_ptr_used > g_ptr_live * 2u && g_ptr_used > 32u) {
        return ht_ptr_rehash(g_ptr_cap);
    }
    return 0;
}

/* ------------------------------------------------------------ 生命周期 */
int32_t lvglcj_handle_table_init(void)
{
    pthread_mutex_lock(&g_ht_lock);

    free(g_id_slots);
    free(g_ptr_slots);
    g_id_slots = NULL;
    g_ptr_slots = NULL;
    g_id_cap = g_ptr_cap = 0;

    /*
     * ★ ID 计数器**跨 init/destroy 也要保持单调**：
     *   若销毁后从头开始，上一轮的悬空句柄就会命中新对象 —— 正是 ADR-001 要避免的。
     *   进程生命周期内只增不减即可（int64 不可能耗尽）。
     */
    if (g_next_id < 1) {
        g_next_id = 1;
    }

    int rc = ht_id_rehash(HT_INIT_CAP);
    if (rc == 0) {
        rc = ht_ptr_rehash(HT_INIT_CAP);
    }

    pthread_mutex_unlock(&g_ht_lock);

    if (rc != 0) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "句柄表初始分配失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }
    return LVGLCJ_OK;
}

void lvglcj_handle_table_destroy(void)
{
    pthread_mutex_lock(&g_ht_lock);
    free(g_id_slots);
    free(g_ptr_slots);
    g_id_slots = NULL;
    g_ptr_slots = NULL;
    g_id_cap = g_ptr_cap = 0;
    g_id_used = g_id_live = 0;
    g_invalid_live = 0;
    g_ptr_used = 0;
    pthread_mutex_unlock(&g_ht_lock);
}

/* 调用方必须持有锁 */
static ht_entry_t *ht_entry_of_unlocked(int64_t h)
{
    if (g_id_slots == NULL || h <= 0) {
        return NULL;
    }
    size_t idx = 0;
    if (ht_id_find_slot(h, &idx) != 1) {
        return NULL;
    }
    return &g_id_slots[idx];
}

/* ------------------------------------------------------------ 注册 / 查询 */
int64_t lvglcj_handle_register(void *ptr, const char *name)
{
    if (ptr == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "不能为 NULL 指针注册句柄");
        return LVGLCJ_HANDLE_NULL;
    }

    pthread_mutex_lock(&g_ht_lock);

    /* 已有 ALIVE 句柄则直接复用，保持 ptr ↔ id 一对一 */
    if (g_ptr_slots != NULL) {
        size_t pidx = 0;
        if (ht_ptr_find_slot(ptr, &pidx) == 1) {
            int64_t existing = g_ptr_slots[pidx].id;
            pthread_mutex_unlock(&g_ht_lock);
            return existing;
        }
    }

    if (ht_id_ensure(0) != 0 || ht_ptr_ensure() != 0) {
        pthread_mutex_unlock(&g_ht_lock);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "句柄表扩容失败");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t id = g_next_id++;
    if (g_next_id <= 0) {
        g_next_id = 1; /* 理论不可达；防御溢出 */
    }

    size_t idx = 0;
    if (ht_id_find_slot(id, &idx) != 0) {
        pthread_mutex_unlock(&g_ht_lock);
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, id, 0, __func__,
                            "句柄 ID 冲突（不应发生）");
        return LVGLCJ_HANDLE_NULL;
    }

    ht_entry_t *e = &g_id_slots[idx];
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->ptr = ptr;
    e->state = (uint8_t)LVGLCJ_HSTATE_ALIVE;
    if (name != NULL) {
        strncpy(e->name, name, sizeof(e->name) - 1);
    }
    g_id_used++;
    g_id_live++;

    size_t pidx = 0;
    (void)ht_ptr_find_slot(ptr, &pidx);
    g_ptr_slots[pidx].ptr = ptr;
    g_ptr_slots[pidx].id = id;
    g_ptr_used++;

    pthread_mutex_unlock(&g_ht_lock);
    return id;
}

void *lvglcj_ptr_of(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    void *ptr = NULL;
    if (e != NULL && e->state == (uint8_t)LVGLCJ_HSTATE_ALIVE) {
        ptr = e->ptr;
    }
    pthread_mutex_unlock(&g_ht_lock);
    return ptr;
}

int64_t lvglcj_handle_of(void *ptr)
{
    if (ptr == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    pthread_mutex_lock(&g_ht_lock);

    int64_t id = LVGLCJ_HANDLE_NULL;
    if (g_ptr_slots != NULL) {
        size_t pidx = 0;
        if (ht_ptr_find_slot(ptr, &pidx) == 1) {
            ht_entry_t *e = ht_entry_of_unlocked(g_ptr_slots[pidx].id);
            if (e != NULL && e->state == (uint8_t)LVGLCJ_HSTATE_ALIVE) {
                id = e->id;
            }
        }
    }

    pthread_mutex_unlock(&g_ht_lock);
    return id;
}

int32_t lvglcj_handle_alive(int64_t h)
{
    return (lvglcj_handle_state(h) == (int32_t)LVGLCJ_HSTATE_ALIVE) ? 1 : 0;
}

int32_t lvglcj_handle_state(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    int32_t st = (e != NULL) ? (int32_t)e->state : (int32_t)LVGLCJ_HSTATE_UNINIT;
    pthread_mutex_unlock(&g_ht_lock);
    return st;
}

int32_t lvglcj_handle_require(int64_t h, const char *func)
{
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    int32_t st = (e != NULL) ? (int32_t)e->state : (int32_t)LVGLCJ_HSTATE_UNINIT;
    int32_t pending = (e != NULL) ? (int32_t)e->pending : 0;
    pthread_mutex_unlock(&g_ht_lock);

    if (st == (int32_t)LVGLCJ_HSTATE_ALIVE) {
        if (pending) {
            lvglcj_record_error(LVGLCJ_ERR_PENDING_DELETE, h, 0, func,
                                "对象处于待删除状态（延迟删除队列中）");
            return LVGLCJ_ERR_PENDING_DELETE;
        }
        return LVGLCJ_OK;
    }

    lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, h, 0, func,
                        (st == (int32_t)LVGLCJ_HSTATE_INVALIDATED)
                            ? "句柄已失效（原生对象已删除）"
                            : "句柄无效（未注册或已回收）");
    return LVGLCJ_ERR_INVALID_HANDLE;
}

/* ------------------------------------------------------------ 状态转移 */

/* 调用方必须持有锁。抽出来是为了让「有界回收」与公开 API 共用同一条释放路径 ——
 * 若各写一份，迟早出现「回收路径忘了减计数」的不一致。 */
static void ht_release_unlocked(ht_entry_t *e)
{
    if (e == NULL || e->id == 0 || e->id == -1) {
        return;
    }

    /*
     * ★ 只有 **ALIVE** 的表项才需要摘反查，这是本函数最容易写错的一处。
     *
     * ht_ptr_tombstone() 是**按指针**摘除的，它不校验该反查项属于哪个 id。
     * 而 INVALIDATED 表项在**失效那一刻**就已经摘过反查了；从那以后，
     * 它保存的 e->ptr 只是一个「曾经用过」的地址，**随时可能被新对象复用**
     * （glibc malloc 会立刻复用刚 free 掉的块）。
     *
     * 若在此时再摘一次，就会把**新对象**刚登记好的反查项删掉，
     * 于是该新对象变成「有 id 表项、没有反查项」。后果不是崩溃，而是更隐蔽的
     * 计数漂移：后续再创建落在同一地址的对象时会被当成未登记而重新登记，
     * 同一个原生对象出现两个句柄，删除只释放一个 → ALIVE 单调增长。
     *
     * 这个缺陷原本就潜伏在 lvglcj_handle_release()（它对任何状态都无条件摘），
     * 只是「INVALIDATED 表项被释放」以前几乎不发生（直接删除的路径里
     * invalidate 与 release 紧邻，地址还没被复用）。t8 的有界回收让
     * INVALIDATED 表项开始被成批释放，它才暴露出来 —— 由 test_leak 的门禁②
     * 抓到位。
     */
    if (e->state == (uint8_t)LVGLCJ_HSTATE_ALIVE) {
        ht_ptr_tombstone(e->ptr);
    }

    if (e->state == (uint8_t)LVGLCJ_HSTATE_INVALIDATED && g_invalid_live > 0) {
        g_invalid_live--;
    }

    /* 真正回收槽位：置墓碑，live 计数减一（ID 不会复用） */
    e->id = -1;
    e->ptr = NULL;
    e->state = (uint8_t)LVGLCJ_HSTATE_RELEASED;
    if (g_id_live > 0) {
        g_id_live--;
    }
}

static int ht_cmp_i64_asc(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y) {
        return -1;
    }
    return (x > y) ? 1 : 0;
}

/*
 * INVALIDATED 表项的有界回收。
 *
 * 【为什么需要】
 *   父对象被删时子对象句柄进入 INVALIDATED 并**保留表项**：这是设计决定，
 *   目的是让「拿着子句柄再操作」能收到「原生对象已删除」这种明确报错，
 *   而不是含糊的「句柄无效」。
 *   但保留若无上限，任何「建容器 → 删容器」的循环都会让表项单调增长：
 *   实测一次「1 个父 + 10 个子」的删除留下 10 条，即 **10 条/轮**。
 *   于是 §11.1 的 soak 判据「句柄收敛」不成立，内存也无界。
 *
 * 【策略】保留最近 N 条（N = LVGLCJ_INVALIDATED_KEEP_MAX），
 *   超出后按**最旧优先**回收，且一次收到低水位（3/4 N），
 *   使回收动作被摊薄 —— 不会每来一条失效就触发一次全表扫描。
 *
 * 【为什么「最旧」= id 最小】句柄 id 单调递增且永不复用（ADR-001），
 *   因此 id 大小即时间先后，不需要额外维护插入序。
 *
 * 【代价】一次回收是 O(n log n)（收集 + 排序），发生在增长越过上限时，
 *   摊销到每条失效记录上可以忽略。分配失败时**不回收**（宁可暂时超限，
 *   也不要在内存紧张时做半途而废的回收）。
 */
static void ht_reap_invalidated_unlocked(void)
{
    if (g_invalid_live <= (size_t)LVGLCJ_INVALIDATED_KEEP_MAX) {
        return;
    }

    size_t low_water = (size_t)LVGLCJ_INVALIDATED_KEEP_MAX * 3u / 4u;
    size_t to_remove = g_invalid_live - low_water;
    if (to_remove == 0) {
        return;
    }

    int64_t *ids = (int64_t *)malloc(g_invalid_live * sizeof(int64_t));
    if (ids == NULL) {
        return;
    }

    size_t n = 0;
    for (size_t i = 0; i < g_id_cap && n < g_invalid_live; i++) {
        ht_entry_t *e = &g_id_slots[i];
        if (e->id > 0 && e->state == (uint8_t)LVGLCJ_HSTATE_INVALIDATED) {
            ids[n++] = e->id;
        }
    }

    if (n == 0) {
        free(ids);
        return;
    }

    qsort(ids, n, sizeof(int64_t), ht_cmp_i64_asc);

    if (to_remove > n) {
        to_remove = n;
    }
    for (size_t k = 0; k < to_remove; k++) {
        ht_release_unlocked(ht_entry_of_unlocked(ids[k]));
    }

    free(ids);
}

void lvglcj_handle_invalidate(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);

    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e == NULL || e->state != (uint8_t)LVGLCJ_HSTATE_ALIVE) {
        /* 幂等：非 ALIVE 一律无操作（§3.1.3） */
        pthread_mutex_unlock(&g_ht_lock);
        return;
    }

    /* 摘掉反查表项，让该指针后续可被重新注册（地址复用是正常的） */
    ht_ptr_tombstone(e->ptr);

    /* ★ 保留表项与删除栈：让仓颉侧能给出明确报错并定位「谁删了我」 */
    e->state = (uint8_t)LVGLCJ_HSTATE_INVALIDATED;
    g_invalid_live++;

    /*
     * 有界回收（见 ht_reap_invalidated_unlocked 的说明）。
     * 放在这里而不是放进 release：释放路径上不该做全表动作，
     * 而失效正是「保留量增长」的唯一来源。
     */
    ht_reap_invalidated_unlocked();

    pthread_mutex_unlock(&g_ht_lock);
}

void lvglcj_handle_release(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);

    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e == NULL) {
        pthread_mutex_unlock(&g_ht_lock);
        return; /* 幂等 */
    }

    ht_release_unlocked(e);

    pthread_mutex_unlock(&g_ht_lock);
}

void lvglcj_handle_mark_pending_delete(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e != NULL) {
        e->pending = 1;
    }
    pthread_mutex_unlock(&g_ht_lock);
}

int32_t lvglcj_handle_pending_delete(int64_t h)
{
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    int32_t pending = (e != NULL) ? (int32_t)e->pending : 0;
    pthread_mutex_unlock(&g_ht_lock);
    return pending;
}

void lvglcj_handle_push_delete_frame(int64_t h, const char *func)
{
    if (func == NULL) {
        return;
    }
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e != NULL && e->delete_depth < LVGLCJ_DELETE_STACK_MAX) {
        strncpy(e->delete_stack[e->delete_depth], func, LVGLCJ_HANDLE_NAME_MAX - 1);
        e->delete_depth++;
    }
    pthread_mutex_unlock(&g_ht_lock);
}

void lvglcj_handle_set_name(int64_t h, const char *name)
{
    if (name == NULL) {
        return;
    }
    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e != NULL) {
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    }
    pthread_mutex_unlock(&g_ht_lock);
}

int32_t lvglcj_handle_get_name(int64_t h, char *buf, int32_t buflen)
{
    if (buf == NULL || buflen <= 0) {
        return 0;
    }
    buf[0] = '\0';

    pthread_mutex_lock(&g_ht_lock);
    ht_entry_t *e = ht_entry_of_unlocked(h);
    if (e != NULL) {
        /* 表项里已保证以 '\0' 结尾；再按调用方缓冲大小裁剪一次 */
        strncpy(buf, e->name, (size_t)buflen - 1);
        buf[buflen - 1] = '\0';
    }
    pthread_mutex_unlock(&g_ht_lock);

    return (int32_t)strlen(buf);
}

/* ------------------------------------------------------------ 可观测性 */
int32_t lvglcj_handle_count(int32_t state)
{
    pthread_mutex_lock(&g_ht_lock);

    int32_t count = 0;
    if (g_id_slots != NULL) {
        for (size_t i = 0; i < g_id_cap; i++) {
            ht_entry_t *e = &g_id_slots[i];
            if (e->id <= 0) {
                continue;
            }
            if (state < 0 || (int32_t)e->state == state) {
                count++;
            }
        }
    }

    pthread_mutex_unlock(&g_ht_lock);
    return count;
}

int32_t lvglcj_handle_dump(int64_t *handles, int32_t *states, int32_t max)
{
    if (handles == NULL || max <= 0) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&g_ht_lock);

    int32_t n = 0;
    if (g_id_slots != NULL) {
        for (size_t i = 0; i < g_id_cap && n < max; i++) {
            ht_entry_t *e = &g_id_slots[i];
            if (e->id <= 0) {
                continue;
            }
            handles[n] = e->id;
            if (states != NULL) {
                states[n] = (int32_t)e->state;
            }
            n++;
        }
    }

    pthread_mutex_unlock(&g_ht_lock);
    return n;
}
