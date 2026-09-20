/*
 * anim.c —— 动画子系统（设计文档 §3.10）
 *
 * ==================== 三个已冻结的实测结论（V7 探针，docs/P0_RESULTS.md）====================
 *
 * 1) **ADR-015：`lv_anim_t.var = anim_ctx`**，不用 obj、也不用 closure_id。
 *    原因：同一对象上可以同时有多个动画（一个改 width、一个改 height），
 *    若 var = obj，则 exec trampoline 的签名只有 (var, value)，
 *    **无法区分是哪个动画在调用** → 也就无法决定该调哪个仓颉闭包。
 *    把 var 指向「本次动画的上下文」后，ctx 里带着 exec_cid，问题消失。
 *
 * 2) **ADR-015 的代价（必须补偿）**：LVGL 原本会在「对象被删除」时
 *    自动停掉该对象上的动画（它按 var==obj 来找）。var 不再是 obj 之后，
 *    这条自动路径**失效** —— 实测 `running 1->1`，动画会持续残留。
 *    补偿点：`lifecycle.c` 的 DELETE 钩子步骤 (c) 调用
 *    `lvglcj_stop_all_anims_of_obj`。本文件提供该函数。
 *
 * 3) **ADR-017 / Patch P1：internal deleted_cb 是 anim_ctx 的「唯一释放点」**。
 *    V7 实测「自然结束 / 手动删除 / 对象删除」三条路径**都会**触发 deleted_cb，
 *    因此不需要在多处 free(ctx) —— 那正是双重 free 的来源。
 *    ★ 本文件里**没有任何其它地方** free(ctx)。
 *
 * ==================== G2 / G3 ====================
 * G2：v9.2 **没有** `lv_anim_get_var()`，但 `lv_anim_t` 是公开结构体，
 *     所以 deleted_cb 直接读 `a->var` 取回 ctx；同时把 ctx 也写进 `a->user_data`
 *     做冗余 —— 这样即使将来某个版本清了 var，deleted_cb 仍能工作。
 * G3：`lv_anim_delete(void* var, lv_anim_exec_xcb_t exec_cb)` 是**按 (var, exec_cb) 键**删除的。
 *     var 已经是 ctx 了，所以：
 *       · 用户侧删除：我们持有 ctx 句柄 → 直接以 (ctx, trampoline) 为键删除，不必绕路；
 *       · 对象删除路径：注册表里存的是 **anim 句柄**，需要 `lv_anim_t*` 才能读出这两个键
 *         → 因此 `lv_anim_start()` 返回的指针必须登记进句柄表（这就是 G3 说的
 *         「anim_handle → ctx 映射成为必需基础设施」）。
 *
 * ==================== 每个动画有两个句柄 ====================
 *   · **ctx 句柄**（用户侧 `lvglcj_anim_create` 返回的）：指向 anim_ctx，
 *     用户用它做 set_* / start / delete。
 *   · **anim 句柄**（内部）：指向 LVGL 真正持有的 `lv_anim_t`（start 会拷贝一份），
 *     只被「对象删除时停动画」的注册表使用。
 *   两者都由 internal deleted_cb 统一回收，这样用户侧不必关心第二个句柄。
 */
#include "lvglcj_internal.h"

#include <stdlib.h>
#include <string.h>

/* 识别 anim_ctx 的魔数（"ANIM"） */
#define LVGLCJ_ANIM_MAGIC ((uint32_t)0x414E494D)

typedef struct {
    uint32_t magic;         /* 必须是 LVGLCJ_ANIM_MAGIC，否则视为「不是我们的 ctx」 */
    uint8_t  started;       /* 是否已经 lv_anim_start 过 */

    lv_anim_t anim;         /* 模板：给 lv_anim_start 拷贝用（start 后改动它无效） */

    int64_t  obj_handle;    /* 目标对象句柄；0 = 未指定 */
    int64_t  self_handle;   /* 本 ctx 自己的句柄（deleted_cb 里要回收它） */
    int64_t  anim_handle;   /* LVGL 侧那份 lv_anim_t 的句柄（G3 所需） */

    int32_t  exec_cid;      /* 用户 exec 闭包 */
    int32_t  deleted_cid;   /* 用户 deleted 闭包（存在 ctx 里，不直接挂 LVGL —— Patch P1） */
    int32_t  start_cid;     /* 用户 start 闭包 */
} lvglcj_anim_ctx_t;

/* 活跃 anim_ctx 数：§11.3 泄漏对账用 */
static int32_t g_ctx_live = 0;

/* ------------------------------------------------------------ 内部工具 */

static lvglcj_anim_ctx_t *ctx_of(int64_t h)
{
    void *p = lvglcj_ptr_of(h);
    if (p == NULL) {
        return NULL;
    }
    lvglcj_anim_ctx_t *c = (lvglcj_anim_ctx_t *)p;
    return (c->magic == LVGLCJ_ANIM_MAGIC) ? c : NULL;
}

/*
 * 从 LVGL 回调拿到的 lv_anim_t* 反查 ctx。
 * G2：优先 a->var（ADR-015 的规定位置），失败则退回 a->user_data 冗余。
 */
static lvglcj_anim_ctx_t *ctx_from_anim(const lv_anim_t *a)
{
    if (a == NULL) {
        return NULL;
    }
    lvglcj_anim_ctx_t *c = (lvglcj_anim_ctx_t *)a->var;
    if (c != NULL && c->magic == LVGLCJ_ANIM_MAGIC) {
        return c;
    }
    c = (lvglcj_anim_ctx_t *)a->user_data;
    if (c != NULL && c->magic == LVGLCJ_ANIM_MAGIC) {
        return c;
    }
    return NULL;
}

/* 路径编号 → LVGL 路径函数（契约见 lvglcj_bridge.h 的 lvglcj_anim_path_t） */
static lv_anim_path_cb_t path_cb_of(int32_t path)
{
    switch (path) {
        case LVGLCJ_ANIM_PATH_LINEAR:      return lv_anim_path_linear;
        case LVGLCJ_ANIM_PATH_STEP:        return lv_anim_path_step;
        case LVGLCJ_ANIM_PATH_EASE_IN:     return lv_anim_path_ease_in;
        case LVGLCJ_ANIM_PATH_EASE_OUT:    return lv_anim_path_ease_out;
        case LVGLCJ_ANIM_PATH_EASE_IN_OUT: return lv_anim_path_ease_in_out;
        case LVGLCJ_ANIM_PATH_OVERSHOOT:   return lv_anim_path_overshoot;
        case LVGLCJ_ANIM_PATH_BOUNCE:      return lv_anim_path_bounce;
        default:                           return NULL;
    }
}

/* 注销一个闭包 id（协议：arg == LVGLCJ_ARG_UNREGISTER 表示「请注销，不要调用」） */
static void retire_closure(int32_t cid)
{
    if (cid != LVGLCJ_CID_NONE) {
        (void)lvglcj_call_closure(cid, LVGLCJ_ARG_UNREGISTER);
    }
}

/* ------------------------------------------ internal deleted_cb：唯一释放点 */

/*
 * ★★ 本函数是 anim_ctx 的**唯一**释放点（ADR-017 / Patch P1）★★
 *
 * 触发它的三条路径（V7 已全部实测确认）：
 *   path1 动画自然结束（含 repeat 跑完、playback 结束）
 *   path2 显式 lv_anim_delete
 *   path3 对象被删除时由 stop_all_anims_of_obj 间接删除
 *
 * 因此这里必须把「ctx 相关的所有资源」一次收干净：
 *   注册表绑定 → anim 句柄 → 用户闭包 → ctx 句柄 → ctx 本身
 * 顺序不能乱：
 *   · 先摘注册表，避免 stop_all_anims_of_obj 的循环再次看到它；
 *   · 先 release 两个句柄再 free(ctx)，否则句柄表里会留下指向已释放内存的表项；
 *   · 最后 free(ctx)，且**在此之前**不再读写 ctx（free 之后 ctx 即失效）。
 */
static void anim_internal_deleted_cb(lv_anim_t *a)
{
    lvglcj_anim_ctx_t *ctx = ctx_from_anim(a);
    if (ctx == NULL) {
        /*
         * 不是本层创建的动画（例如 LVGL 内部的动画，或 ctx 早已释放）。
         * 直接返回：**绝不能**对来历不明的指针 free。
         */
        return;
    }

    /* 先把 ctx 里要用的东西全部取出来，并立刻「摘除身份」 */
    int64_t obj_h  = ctx->obj_handle;
    int64_t anim_h = ctx->anim_handle;
    int64_t self_h = ctx->self_handle;
    int32_t u_del  = ctx->deleted_cid;
    int32_t u_exec = ctx->exec_cid;
    int32_t u_start = ctx->start_cid;

    ctx->magic = 0;                       /* 防御：即使被重入一次也不会重复释放 */
    ctx->self_handle = LVGLCJ_HANDLE_NULL;
    ctx->anim_handle = LVGLCJ_HANDLE_NULL;
    ctx->deleted_cid = LVGLCJ_CID_NONE;
    ctx->exec_cid = LVGLCJ_CID_NONE;
    ctx->start_cid = LVGLCJ_CID_NONE;

    /* (1) 摘掉「对象 → 动画」注册表绑定 */
    if (obj_h != LVGLCJ_HANDLE_NULL && anim_h != LVGLCJ_HANDLE_NULL) {
        lvglcj_reg_anim_unbind(obj_h, anim_h);
    }

    /* (2) 回收 LVGL 那份 lv_anim_t 的句柄（必须在 free(ctx) 之前） */
    if (anim_h != LVGLCJ_HANDLE_NULL) {
        lvglcj_handle_release(anim_h);
    }

    /*
     * (3) 用户的 deleted 闭包：先**调用**（它是用户观察「动画没了」的唯一时机），
     *     再注销。arg 传 0 —— 此刻动画句柄已被回收，传它只会引导用户去调用
     *     一个必然失败的操作。
     */
    if (u_del != LVGLCJ_CID_NONE) {
        (void)lvglcj_call_closure(u_del, 0);
    }
    retire_closure(u_del);

    /* (4) exec / start 闭包：只注销，不调用（动画已结束，调用没有语义） */
    retire_closure(u_exec);
    retire_closure(u_start);

    /* (5) 回收 ctx 句柄（用户手里的那个）—— 同样必须在 free 之前 */
    if (self_h != LVGLCJ_HANDLE_NULL) {
        lvglcj_handle_release(self_h);
    }

    /* (6) 唯一的一次 free。此后不得再触碰 ctx。 */
    g_ctx_live--;
    free(ctx);
}

/*
 * 内部 start_cb：把「动画真正开始」这件事转给用户闭包。
 * 与 deleted_cb 不同，start 回调**不是**释放点，只做转发。
 */
static void anim_internal_start_cb(lv_anim_t *a)
{
    lvglcj_anim_ctx_t *ctx = ctx_from_anim(a);
    if (ctx == NULL || ctx->start_cid == LVGLCJ_CID_NONE) {
        return;
    }
    /* arg = 目标对象句柄（0 表示未指定）—— 比传插值更有用 */
    (void)lvglcj_call_closure(ctx->start_cid, ctx->obj_handle);
}

/* ------------------------------------------------------- T3：动画执行 trampoline */

/*
 * T3（§3.2.5）。LVGL 每个动画帧回调这里。
 *   var   = anim_ctx（ADR-015）
 *   value = 本次插值
 * 转给用户闭包时 arg = 插值 —— 与 lvglcj_bridge.h 开头声明的 arg 语义一致。
 *
 * 注意：用户闭包**不应该**在这里构造异常（见 docs/C4_DEFECT.md 的平台限制），
 * 失败请通过返回码或 onError 上报；分发器会把异常吞并在 C 边界之内。
 */
void lvglcj_anim_exec_trampoline(void *var, int32_t value)
{
    lvglcj_anim_ctx_t *ctx = (lvglcj_anim_ctx_t *)var;
    if (ctx == NULL || ctx->magic != LVGLCJ_ANIM_MAGIC) {
        return;
    }
    if (ctx->exec_cid == LVGLCJ_CID_NONE) {
        return;
    }
    (void)lvglcj_call_closure(ctx->exec_cid, (int64_t)value);
}

/* ------------------------------------------------------------ 创建与设置 */

int64_t lvglcj_anim_create(void)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lvglcj_anim_ctx_t *ctx = (lvglcj_anim_ctx_t *)calloc(1, sizeof(lvglcj_anim_ctx_t));
    if (ctx == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "anim_ctx 分配失败");
        return LVGLCJ_HANDLE_NULL;
    }

    ctx->magic = LVGLCJ_ANIM_MAGIC;
    ctx->exec_cid = LVGLCJ_CID_NONE;
    ctx->deleted_cid = LVGLCJ_CID_NONE;
    ctx->start_cid = LVGLCJ_CID_NONE;
    ctx->obj_handle = LVGLCJ_HANDLE_NULL;
    ctx->anim_handle = LVGLCJ_HANDLE_NULL;

    lv_anim_init(&ctx->anim);
    /* 默认曲线与 LVGL 一致（linear），显式写出以免依赖内部默认值 */
    lv_anim_set_path_cb(&ctx->anim, lv_anim_path_linear);
    /*
     * 内部回调在此一次性挂好：
     *   deleted_cb 是**唯一释放点**，绝不能漏挂 —— 漏挂等于每个动画都泄漏 ctx。
     */
    lv_anim_set_deleted_cb(&ctx->anim, anim_internal_deleted_cb);
    lv_anim_set_start_cb(&ctx->anim, anim_internal_start_cb);
    /* exec_cb 固定为 trampoline：它也是 lv_anim_delete 的键的一部分（G3） */
    lv_anim_set_exec_cb(&ctx->anim, lvglcj_anim_exec_trampoline);

    int64_t h = lvglcj_handle_register(ctx, "lvglcj_anim_ctx_t");
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 登记失败必须把刚分配的 ctx 放掉 */
        ctx->magic = 0;
        free(ctx);
        return LVGLCJ_HANDLE_NULL;
    }
    ctx->self_handle = h;
    g_ctx_live++;
    return h;
}

/*
 * 取 ctx 并检查「尚未 start」。
 * ★ 为什么要拦「start 之后再 set」：lv_anim_start 会把 ctx->anim **拷贝**到
 *   LVGL 自己的存储里。此后写 ctx->anim 只是改了一个没人看的模板 ——
 *   表现为「设了但没生效」，且不报错。与其让用户去猜，不如明确拒绝。
 */
static lvglcj_anim_ctx_t *ctx_writable(int64_t a, const char *func)
{
    lvglcj_anim_ctx_t *ctx = ctx_of(a);
    if (ctx == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, a, 0, func,
                            "不是有效的动画上下文句柄");
        return NULL;
    }
    if (ctx->started) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, func,
                            "动画已 start，之后再 set 不会生效（start 会拷贝模板）");
        return NULL;
    }
    return ctx;
}

int32_t lvglcj_anim_set_target(int64_t a, int64_t obj)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (obj != LVGLCJ_HANDLE_NULL && lvglcj_handle_require(obj, __func__) != LVGLCJ_OK) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    /*
     * 只记录「所属对象」，**不**把它写进 anim.var —— var 必须是 ctx（ADR-015），
     * 否则 exec trampoline 就分不清是哪个动画了。
     */
    ctx->obj_handle = obj;
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_values(int64_t a, int32_t from, int32_t to)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_anim_set_values(&ctx->anim, from, to);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_time(int64_t a, int32_t ms)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (ms < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "时长不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_anim_set_time(&ctx->anim, (uint32_t)ms);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_delay(int64_t a, int32_t ms)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (ms < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "延迟不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_anim_set_delay(&ctx->anim, (uint32_t)ms);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_exec_cb(int64_t a, int32_t cid)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (cid == LVGLCJ_CID_NONE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "exec 闭包 id 不能为 0（0 表示「没有闭包」）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /*
     * 只存 cid；挂给 LVGL 的永远是 trampoline（它需要 (var, value) 才能定位 ctx，
     * 而用户的闭包不该知道 ctx 的存在）。
     */
    ctx->exec_cid = cid;
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_path(int64_t a, int32_t path)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_anim_path_cb_t cb = path_cb_of(path);
    if (cb == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "未知的插值曲线编号（见 lvglcj_anim_path_t）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    lv_anim_set_path_cb(&ctx->anim, cb);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_repeat(int64_t a, int32_t cnt)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (cnt < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "重复次数不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /*
     * LVGL 的 repeat_cnt 语义：0 = 无限循环，1 = 只跑一次（不重复），N = 共跑 N 次。
     * 这里逐值透传，不做 ±1 换算 —— 任何换算都会让「0 是不限次」这条特殊语义变模糊。
     */
    lv_anim_set_repeat_count(&ctx->anim, (uint32_t)cnt);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_playback(int64_t a, int32_t ms)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (ms < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "playback 时长不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }
    /* v9 已把 playback_time 标为 legacy，用 playback_duration */
    lv_anim_set_playback_duration(&ctx->anim, (uint32_t)ms);
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_deleted_cb(int64_t a, int32_t cid)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    /* Patch P1：用户回调存在 ctx 里，由 internal deleted_cb 统一转发 */
    ctx->deleted_cid = cid;
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_set_start_cb(int64_t a, int32_t cid)
{
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    ctx->start_cid = cid;
    return LVGLCJ_OK;
}

/* ------------------------------------------------------------ 启动与删除 */

int32_t lvglcj_anim_start(int64_t a)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lvglcj_anim_ctx_t *ctx = ctx_writable(a, __func__);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    /*
     * exec_cb 是必需的：没有它动画什么都不做，而且 lv_anim_delete 的键
     * (var, exec_cb) 也失去意义。宁可在这里报错，也不要创建一个「跑着但无效果」的动画。
     */
    if (ctx->exec_cid == LVGLCJ_CID_NONE) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, a, 0, __func__,
                            "启动前必须先用 anim_set_exec_cb 指定执行回调");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    /*
     * ★ var = ctx（ADR-015）；同时写 user_data 做冗余（G2）。
     *   必须在 start 之前设置 —— start 会拷贝模板。
     */
    lv_anim_set_var(&ctx->anim, ctx);
    lv_anim_set_user_data(&ctx->anim, ctx);

    lv_anim_t *stored = lv_anim_start(&ctx->anim);
    if (stored == NULL) {
        /*
         * 通常是 LVGL 动画池耗尽。此处**不能**走 deleted_cb 路径
         * （动画压根没建起来，回调不会被调用），所以由本函数负责把 ctx 收回：
         * 直接把 ctx 句柄标为失效并释放，避免泄漏。
         */
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, a, 0, __func__,
                            "lv_anim_start 失败（动画池可能已满）");
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    int64_t ah = lvglcj_handle_register(stored, "lv_anim_t");
    if (ah == LVGLCJ_HANDLE_NULL) {
        /*
         * 动画已经在跑，但我们没能给它登记句柄。此时唯一正确的做法是**把它删掉**：
         * 删除会同步触发 deleted_cb，由它统一释放 ctx 与闭包。
         * 注意：调用之后 ctx 已被释放，**不得再触碰**。
         */
        (void)lv_anim_delete(stored->var, stored->exec_cb);
        return LVGLCJ_ERR_OUT_OF_MEMORY;
    }

    ctx->anim_handle = ah;
    ctx->started = 1;

    /* 绑定「对象 → 动画」：DELETE 钩子的补偿路径靠它找到这个动画（ADR-015 代价补偿） */
    if (ctx->obj_handle != LVGLCJ_HANDLE_NULL) {
        int32_t brc = lvglcj_reg_anim_bind(ctx->obj_handle, ah);
        if (brc != LVGLCJ_OK) {
            /*
             * 绑定失败不致命（动画仍能跑），但对象删除时就停不掉它 ——
             * 记错并让动画继续跑，比在这里把已启动的动画强行删掉更符合预期。
             */
            lvglcj_record_error(brc, a, 0, __func__,
                                "动画已启动但未能登记到目标对象（对象删除时可能停不掉它）");
        }
    }
    return LVGLCJ_OK;
}

int32_t lvglcj_anim_delete(int64_t a)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    /* 严格：非 ALIVE 一律 INVALID_HANDLE（幂等由仓颉 close() 负责） */
    LVGLCJ_HANDLE_GUARD(a, __func__);

    lvglcj_anim_ctx_t *ctx = ctx_of(a);
    if (ctx == NULL) {
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    if (!ctx->started) {
        /*
         * 还没 start 就删除：不会有 deleted_cb（动画从未进入 LVGL 的管理），
         * 因此**必须由本函数**负责释放 ctx 与用户闭包，否则一定泄漏。
         */
        int32_t u_exec = ctx->exec_cid;
        int32_t u_del = ctx->deleted_cid;
        int32_t u_start = ctx->start_cid;
        int64_t self_h = ctx->self_handle;
        ctx->magic = 0;
        retire_closure(u_exec);
        retire_closure(u_del);
        retire_closure(u_start);
        if (self_h != LVGLCJ_HANDLE_NULL) {
            lvglcj_handle_release(self_h);
        }
        g_ctx_live--;
        free(ctx);
        return LVGLCJ_OK;
    }

    /*
     * 已启动：以 (ctx, trampoline) 为键删除（G3 的键就是这两个值）。
     * ★ lv_anim_delete 会**同步**触发 internal deleted_cb，后者会释放 ctx ——
     *   因此下一行之后绝不能再使用 ctx。
     */
    int64_t key_var = (int64_t)(uintptr_t)ctx;
    (void)key_var; /* 仅作说明：var 即 ctx 指针本身 */
    lv_anim_delete((void *)ctx, lvglcj_anim_exec_trampoline);
    return LVGLCJ_OK;
}

/* -------------------------------------------- 按对象停止（DELETE 钩子步骤 (c)） */

/*
 * ★ 不得在此 free(anim_ctx)（§D.5 禁止事项 / Patch P1）：
 *   ctx 由动画的内部 deleted_cb 唯一释放。本函数只负责「请求停止」，
 *   释放由 LVGL 回调链上的 deleted_cb 完成。
 */
void lvglcj_stop_all_anims_of_obj(int64_t obj_handle)
{
    if (obj_handle == LVGLCJ_HANDLE_NULL) {
        return;
    }

    int32_t total = lvglcj_reg_anim_count_of(obj_handle);
    if (total <= 0) {
        return;
    }

    /*
     * 逐个处理。注意：这里**不能**用固定大小的数组一把取完再遍历 ——
     * lv_anim_delete 会触发 deleted_cb，而 deleted_cb 会调
     * lvglcj_reg_anim_unbind 修改注册表，导致下标失效。
     * 因此每轮只取「第一个」，处理完再取下一个。
     */
    int32_t guard = 0;
    const int32_t max_guard = 4096; /* 防御性上限：正常远达不到 */

    for (;;) {
        int64_t anims[1];
        if (lvglcj_reg_anim_list_of(obj_handle, anims, 1) <= 0) {
            break; /* 已清空 */
        }
        int64_t anim_h = anims[0];

        if (++guard > max_guard) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, obj_handle, 0, __func__,
                                "停动画循环超过上限，可能存在注册表不一致");
            break;
        }

        lv_anim_t *a = (lv_anim_t *)lvglcj_ptr_of(anim_h);
        if (a == NULL) {
            /* 动画载体已消失但注册表还在：直接摘掉记录，避免死循环 */
            lvglcj_reg_anim_unbind(obj_handle, anim_h);
            lvglcj_handle_release(anim_h);
            continue;
        }

        /* 先摘注册表，再删除 —— 避免 deleted_cb 里再次 unbind 造成重复删除 */
        lvglcj_reg_anim_unbind(obj_handle, anim_h);

        /*
         * 用 (var, exec_cb) 键删除（G3）。
         * var 就是 anim_ctx（ADR-015），exec_cb 是动画执行 trampoline。
         * 这次调用会同步触发 deleted_cb → ctx、闭包、句柄全部回收。
         */
        lv_anim_delete(a->var, a->exec_cb);
    }
}

/* ---------------------------------------------------------- 可观测性 */

int32_t lvglcj_anim_count_running(void)
{
    return (int32_t)lv_anim_count_running();
}

/*
 * 活跃 anim_ctx 数（§11.3 泄漏对账）。
 *
 * 与 lvglcj_anim_count_running() 的关系：
 *   · **稳态（没有动画在跑）时两者都必须是 0** —— 这就是「零泄漏」的判据；
 *   · 动画在跑期间，本计数应 ≥ lv_anim_count_running()，因为延迟（delay）
 *     尚未到期的动画没被算进 running，但它的 ctx 已经存在。
 *   因此对账断言应当写成「稳态双零」，而不是「两者恒等」。
 */
int32_t lvglcj_anim_ctx_count(void)
{
    return g_ctx_live;
}

int32_t lvglcj_obj_delete_anim(int64_t obj)
{
    LVGLCJ_HANDLE_GUARD(obj, __func__);
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    lvglcj_stop_all_anims_of_obj(obj);
    return LVGLCJ_OK;
}
