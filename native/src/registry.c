/*
 * registry.c —— 对象级注册表（事件闭包 + 动画绑定）
 *
 * 解决的三个问题：
 *   1. DELETE 钩子要知道「这个对象挂了哪些闭包」→ 通知仓颉侧注销，防闭包表泄漏（§3.1.4 步骤 a）
 *   2. DELETE 钩子要知道「这个对象有哪些动画」→ 手动停掉，补偿 ADR-015 的代价（§3.10.3）
 *   3. 动画删除需要 (var, exec_cb) 键（LVGL 实测：lv_anim_delete 按此键删除，见 docs/P0_RESULTS.md G3）
 *      → 必须能由 anim_handle 反查 ctx，因此需要 anim ↔ obj 的绑定关系
 *
 * 设计取舍：紧凑数组 + 线性扫描，不做哈希。
 *   理由见 lvglcj_internal.h 的复杂度说明：这里的规模是「单对象的回调数」，
 *   不在热路径上；P0 优先保证**可读性与可验证性**，而非提前优化。
 *   全部操作都在 LVGL 线程内发生（由线程断言保证），但仍加锁以防误用。
 */
#include "lvglcj_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ 事件闭包表 */
typedef struct {
    int64_t obj_handle;
    void   *dsc; /* lv_event_dsc_t*（仅作身份标识，不解引用） */
    int32_t cid;
} reg_event_t;

static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static reg_event_t    *g_ev = NULL;
static int32_t         g_ev_cap = 0;
static int32_t         g_ev_len = 0;

#define REG_INIT_CAP 64

static int reg_ev_grow(void)
{
    int32_t new_cap = (g_ev_cap == 0) ? REG_INIT_CAP : g_ev_cap * 2;
    reg_event_t *fresh = (reg_event_t *)realloc(g_ev, (size_t)new_cap * sizeof(reg_event_t));
    if (fresh == NULL) {
        return -1;
    }
    g_ev = fresh;
    g_ev_cap = new_cap;
    return 0;
}

int32_t lvglcj_reg_event_add(int64_t obj_handle, void *dsc, int32_t cid)
{
    if (cid == LVGLCJ_CID_NONE) {
        /* 没有用户闭包（例如内部 DELETE 钩子），无需登记 */
        return LVGLCJ_OK;
    }

    pthread_mutex_lock(&g_ev_lock);

    if (g_ev_len >= g_ev_cap && reg_ev_grow() != 0) {
        pthread_mutex_unlock(&g_ev_lock);
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, obj_handle, cid, __func__,
                            "事件注册表扩容失败");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    g_ev[g_ev_len].obj_handle = obj_handle;
    g_ev[g_ev_len].dsc = dsc;
    g_ev[g_ev_len].cid = cid;
    g_ev_len++;

    pthread_mutex_unlock(&g_ev_lock);
    return LVGLCJ_OK;
}

/*
 * 按 dsc 精确删除（不关心 obj_handle：dsc 唯一标识一条注册）。
 * 用「尾部元素填补空洞」而不是 memmove：
 *   顺序对本表无意义（删除时是整体遍历），swap-remove 是 O(1)。
 */
void lvglcj_reg_event_remove_by_dsc(const void *dsc)
{
    if (dsc == NULL) {
        return;
    }

    pthread_mutex_lock(&g_ev_lock);
    for (int32_t i = 0; i < g_ev_len; i++) {
        if (g_ev[i].dsc == dsc) {
            g_ev[i] = g_ev[g_ev_len - 1];
            g_ev_len--;
            break;
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
}

int32_t lvglcj_reg_event_cids_of(int64_t obj_handle, int32_t *out, int32_t max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_ev_lock);
    int32_t n = 0;
    for (int32_t i = 0; i < g_ev_len && n < max; i++) {
        if (g_ev[i].obj_handle == obj_handle) {
            out[n] = g_ev[i].cid;
            n++;
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
    return n;
}

int32_t lvglcj_reg_event_count_of(int64_t obj_handle)
{
    pthread_mutex_lock(&g_ev_lock);
    int32_t n = 0;
    for (int32_t i = 0; i < g_ev_len; i++) {
        if (g_ev[i].obj_handle == obj_handle) {
            n++;
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
    return n;
}

void lvglcj_reg_event_drop(int64_t obj_handle)
{
    pthread_mutex_lock(&g_ev_lock);
    for (int32_t i = 0; i < g_ev_len;) {
        if (g_ev[i].obj_handle == obj_handle) {
            g_ev[i] = g_ev[g_ev_len - 1];
            g_ev_len--;
            /* 注意：不递增 i —— 换过来的元素还没检查 */
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
}

/* ------------------------------------------------------------ 动画绑定表 */
typedef struct {
    int64_t obj_handle;
    int64_t anim_handle;
} reg_anim_t;

static pthread_mutex_t g_an_lock = PTHREAD_MUTEX_INITIALIZER;
static reg_anim_t     *g_an = NULL;
static int32_t         g_an_cap = 0;
static int32_t         g_an_len = 0;

int32_t lvglcj_reg_anim_bind(int64_t obj_handle, int64_t anim_handle)
{
    if (obj_handle == LVGLCJ_HANDLE_NULL || anim_handle == LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&g_an_lock);

    if (g_an_len >= g_an_cap) {
        int32_t new_cap = (g_an_cap == 0) ? REG_INIT_CAP : g_an_cap * 2;
        reg_anim_t *fresh = (reg_anim_t *)realloc(g_an, (size_t)new_cap * sizeof(reg_anim_t));
        if (fresh == NULL) {
            pthread_mutex_unlock(&g_an_lock);
            lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, obj_handle, 0, __func__,
                                "动画注册表扩容失败");
            return LVGLCJ_ERR_OUT_OF_MEMORY;
        }
        g_an = fresh;
        g_an_cap = new_cap;
    }

    g_an[g_an_len].obj_handle = obj_handle;
    g_an[g_an_len].anim_handle = anim_handle;
    g_an_len++;

    pthread_mutex_unlock(&g_an_lock);
    return LVGLCJ_OK;
}

void lvglcj_reg_anim_unbind(int64_t obj_handle, int64_t anim_handle)
{
    pthread_mutex_lock(&g_an_lock);
    for (int32_t i = 0; i < g_an_len; i++) {
        if (g_an[i].obj_handle == obj_handle && g_an[i].anim_handle == anim_handle) {
            g_an[i] = g_an[g_an_len - 1];
            g_an_len--;
            break;
        }
    }
    pthread_mutex_unlock(&g_an_lock);
}

int32_t lvglcj_reg_anim_list_of(int64_t obj_handle, int64_t *out, int32_t max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_an_lock);
    int32_t n = 0;
    for (int32_t i = 0; i < g_an_len && n < max; i++) {
        if (g_an[i].obj_handle == obj_handle) {
            out[n] = g_an[i].anim_handle;
            n++;
        }
    }
    pthread_mutex_unlock(&g_an_lock);
    return n;
}

int32_t lvglcj_reg_anim_count_of(int64_t obj_handle)
{
    pthread_mutex_lock(&g_an_lock);
    int32_t n = 0;
    for (int32_t i = 0; i < g_an_len; i++) {
        if (g_an[i].obj_handle == obj_handle) {
            n++;
        }
    }
    pthread_mutex_unlock(&g_an_lock);
    return n;
}

/* ------------------------------------------------------ 样式使用者注册表 */
/*
 * 结构镜像上面的动画绑定表：一个定长增长的 (obj, style) 对数组 + 一把锁。
 * 不复用同一张表：两张表的失效时机完全不同（动画随动画结束，样式随对象删除），
 * 混在一起会让「谁该清哪条」变得难以推理。
 */
typedef struct {
    int64_t obj_handle;
    int64_t style_handle;
    int32_t selector;      /* ★ 必须入键：见 lvglcj_reg_style_bind 的说明 */
} reg_style_t;

static pthread_mutex_t g_st_lock = PTHREAD_MUTEX_INITIALIZER;
static reg_style_t    *g_st = NULL;
static int32_t         g_st_cap = 0;
static int32_t         g_st_len = 0;

/*
 * ★ selector 是键的一部分，不能省。
 *
 * LVGL 的样式绑定三元组是 (obj, style, selector)——同一个 style 可以以
 * 不同 selector 挂到同一个对象上（默认态一份、按下态一份）。
 * 若我们的注册表只记 (obj, style)，就会出现这个漏洞：
 *   add_style(obj, st, DEFAULT)  → 记一条
 *   remove_style(obj, st, PRESSED) → 误删那条记录
 *   style_delete(st)             → 守卫以为无人引用而放行
 *   ……但 DEFAULT 那份**还在**对象的样式链里 → 释放样式的悬空指针 UAF 又回来了。
 * 这类「守卫看上去在工作、实际漏过」的缺陷比没有守卫更危险，所以宁可多一个键。
 */
int32_t lvglcj_reg_style_bind(int64_t obj_handle, int64_t style_handle, int32_t selector)
{
    if (obj_handle == LVGLCJ_HANDLE_NULL || style_handle == LVGLCJ_HANDLE_NULL) {
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&g_st_lock);

    /* 完全相同的三元组重复绑定：只记一条 */
    for (int32_t i = 0; i < g_st_len; i++) {
        if (g_st[i].obj_handle == obj_handle && g_st[i].style_handle == style_handle &&
            g_st[i].selector == selector) {
            pthread_mutex_unlock(&g_st_lock);
            return LVGLCJ_OK;
        }
    }

    if (g_st_len >= g_st_cap) {
        int32_t new_cap = (g_st_cap == 0) ? REG_INIT_CAP : g_st_cap * 2;
        reg_style_t *fresh = (reg_style_t *)realloc(g_st, (size_t)new_cap * sizeof(reg_style_t));
        if (fresh == NULL) {
            pthread_mutex_unlock(&g_st_lock);
            lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, obj_handle, 0, __func__,
                                "样式使用者注册表扩容失败");
            return LVGLCJ_ERR_OUT_OF_MEMORY;
        }
        g_st = fresh;
        g_st_cap = new_cap;
    }

    g_st[g_st_len].obj_handle = obj_handle;
    g_st[g_st_len].style_handle = style_handle;
    g_st[g_st_len].selector = selector;
    g_st_len++;

    pthread_mutex_unlock(&g_st_lock);
    return LVGLCJ_OK;
}

void lvglcj_reg_style_unbind(int64_t obj_handle, int64_t style_handle, int32_t selector)
{
    pthread_mutex_lock(&g_st_lock);
    for (int32_t i = 0; i < g_st_len; i++) {
        if (g_st[i].obj_handle == obj_handle && g_st[i].style_handle == style_handle &&
            g_st[i].selector == selector) {
            g_st[i] = g_st[g_st_len - 1];
            g_st_len--;
            break;
        }
    }
    pthread_mutex_unlock(&g_st_lock);
}

void lvglcj_reg_style_unbind_obj(int64_t obj_handle)
{
    pthread_mutex_lock(&g_st_lock);
    /* 倒序 + 尾部交换删除：正序遍历时元素会被后面的交换打乱 */
    for (int32_t i = g_st_len - 1; i >= 0; i--) {
        if (g_st[i].obj_handle == obj_handle) {
            g_st[i] = g_st[g_st_len - 1];
            g_st_len--;
        }
    }
    pthread_mutex_unlock(&g_st_lock);
}

int32_t lvglcj_reg_style_user_count(int64_t style_handle)
{
    pthread_mutex_lock(&g_st_lock);

    int32_t n = 0;
    for (int32_t i = g_st_len - 1; i >= 0; i--) {
        if (g_st[i].style_handle != style_handle) {
            continue;
        }
        /*
         * 只统计**仍然存活**的对象；顺手把「对象已死」的陈旧记录删掉。
         * 这样即使某条 unbind 路径（例如将来新增的删除分支）漏了，
         * 最坏结果也只是计数短暂偏大 —— 而不会永久卡住样式释放。
         */
        if (lvglcj_handle_alive(g_st[i].obj_handle)) {
            n++;
        } else {
            g_st[i] = g_st[g_st_len - 1];
            g_st_len--;
        }
    }

    pthread_mutex_unlock(&g_st_lock);
    return n;
}

/* ------------------------------------------------------------ 生命周期 */
void lvglcj_registry_destroy(void)
{
    pthread_mutex_lock(&g_ev_lock);
    free(g_ev);
    g_ev = NULL;
    g_ev_cap = 0;
    g_ev_len = 0;
    pthread_mutex_unlock(&g_ev_lock);

    pthread_mutex_lock(&g_an_lock);
    free(g_an);
    g_an = NULL;
    g_an_cap = 0;
    g_an_len = 0;
    pthread_mutex_unlock(&g_an_lock);

    pthread_mutex_lock(&g_st_lock);
    free(g_st);
    g_st = NULL;
    g_st_cap = 0;
    g_st_len = 0;
    pthread_mutex_unlock(&g_st_lock);
}
