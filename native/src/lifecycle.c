/*
 * lifecycle.c —— 生命周期级联失效（设计文档 §3.1.4 / §3.3）
 *
 * ======================= 钩子内的固定顺序 =======================
 * DELETE 钩子对**整棵子树**（先深后浅）逐个节点执行 cleanup_one()：
 *   (a)  先移除该对象所有闭包      —— 防止闭包表泄漏
 *   (a2) 解绑该对象的样式记录      —— 否则样式永远显示"有人在用"（守卫变死锁）
 *   (c)  停止该对象所有动画        —— 补偿 ADR-015 的代价（var != obj 时 LVGL 不会自动停）
 *   (d)  最后 invalidate 自身      —— 状态 ALIVE → INVALIDATED
 *
 * (a) 必须在 (a2)(c)(d) 之前 —— 这个顺序在此写死，不要调整。
 * 原因：一旦自身进入 INVALIDATED，(a) 里通过句柄反查对象信息的能力就没了。
 *
 * ★ 为什么是「整棵子树由父代劳」而不是「每个对象各自在自己的钩子里清理」：
 *   LVGL 先给父发 DELETE、之后才递归删子对象，而失效子句柄会摘掉它的
 *   ptr→handle 反查项 —— 子对象的钩子随后认不出自己，会整段跳过清理。
 *   完整经过与实测后果见 cleanup_subtree() 上方的说明。
 * ===============================================================
 *
 * ★ 本文件**不得** free(anim_ctx)：
 *   ctx 由动画的内部 deleted_cb 唯一释放（Patch P1 / ADR-017）。
 *   在这里 free 会造成双重 free（设计文档 §D.5 明令禁止）。
 */
#include "lvglcj_internal.h"

/*
 * clean 标志（§3.3.2）：用**保存-恢复**实现，因此支持嵌套 clean。
 *
 * 它解决什么问题：从单个 DELETE 事件无法判断「父是不是正在被 clean」——
 *   父对象此刻仍活着且完全有效。用显式标志把这一信息传进来。
 *
 * 它**不做**什么：本钩子在任何路径下都不会去失效父对象，
 *   所以 clean 后父仍然存活是自然结果。
 *   标志的实际用途是给诊断信息打标注（删除栈里区分 obj_delete / obj_clean），
 *   让「谁删了我」在 dump 时能区分「显式删除」与「父清空子对象」。
 */
static int64_t g_cleaning_parent = 0;

/* 单次事件回调里最多处理多少个 cid（超出的会报错提示，而不是静默丢弃） */
#define LVGLCJ_HOOK_MAX_CIDS 64

/* ------------------------------------------------------ 单对象的资源清理 */

/*
 * 对**一个**对象做完整清理：(a) 注销闭包 → (a2) 解绑样式 → (c) 停动画 → (d) 失效自身。
 *
 * 调用方必须保证 h 尚未失效 —— 因为 (a) 里要用句柄反查对象信息，
 * 一旦进入 INVALIDATED 这个能力就没了（这也是四步顺序不能调的原因）。
 */
static void cleanup_one(int64_t h, const char *frame)
{
    lvglcj_handle_push_delete_frame(h, frame);

    /* ---------------- (a) 先移除该对象所有闭包（防止闭包表泄漏） ---------------- */
    int32_t cids[LVGLCJ_HOOK_MAX_CIDS];
    int32_t n = lvglcj_reg_event_cids_of(h, cids, LVGLCJ_HOOK_MAX_CIDS);

    if (n == LVGLCJ_HOOK_MAX_CIDS && lvglcj_reg_event_count_of(h) > LVGLCJ_HOOK_MAX_CIDS) {
        /* 超出缓冲：宁可报错也不静默泄漏（正常 UI 不会触发） */
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, h, 0, __func__,
                            "单对象事件闭包数超过清理上限 64，可能存在闭包泄漏");
    }

    for (int32_t i = 0; i < n; i++) {
        lvglcj_closure_unregister(cids[i]);
    }
    lvglcj_reg_event_drop(h);

    /*
     * (a2) 清掉「该对象正在用哪些样式」的记录。
     *      LVGL 删除对象时会一并摘掉它的样式引用，我们的注册表必须同步，
     *      否则样式会永远显示「有人在用」而无法释放（守卫变成死锁）。
     */
    lvglcj_reg_style_unbind_obj(h);

    /* ---------------- (c) 停止该对象所有动画（ADR-015 的代价补偿） ---------------- */
    lvglcj_stop_all_anims_of_obj(h);

    /* ---------------- (d) 最后 invalidate 自身 ---------------- */
    lvglcj_handle_invalidate(h);
}

/*
 * 清理整棵子树：(先深后浅) 先所有子对象，最后自己。
 *
 * ★★ 为什么必须由**父**代劳，而不能指望每个子对象自己的 DELETE 钩子 ★★
 *
 *   LVGL v9 的 obj_delete_core() 顺序是：
 *       lv_obj_send_event(obj, LV_EVENT_DELETE, NULL);   ← ① 先给**父**发 DELETE
 *       lv_event_remove_all(...)
 *       lv_obj_t * child = lv_obj_get_child(obj, 0);     ← ② 之后才递归删子对象
 *   即：**父的 DELETE 先到，子的 DELETE 后到**。
 *
 *   而父的钩子要「先深后浅地失效子句柄」就必须调 lvglcj_handle_invalidate(child)，
 *   那会摘掉 child 及其后代的 ptr→handle 反查项。于是等 ② 删到 child、
 *   child 自己的 DELETE 钩子触发时，lvglcj_handle_of(child) 已返回 NULL，
 *   钩子**在第一步就提前返回** —— (a)(a2)(c) 三步全被跳过。
 *
 *   实测后果（t8 随机序列测试发现，数字精确到 100/100 个子对象）：
 *     · (a)  子对象的事件闭包**全部泄漏** —— 闭包表随「建容器→删容器」单调增长；
 *     · (a2) 只被"子对象"引用的样式永远显示"有人在用" —— 释放守卫变**死锁**；
 *     · (c)  深度 ≥ 2 的后代动画**不会被停** —— ADR-015 的补偿失效。
 *
 *   ★ 已有的确定性 soak 没抓到它，是因为负载**形状恰好绕过**了这条路径：
 *     它的树里唯一挂了回调的对象（btn）在删父之前就被显式删掉了。
 *     随机序列的价值正在于此 —— 它会撞上人没想过的形状。
 */
static void cleanup_subtree(lv_obj_t *obj, const char *frame)
{
    if (obj == NULL) {
        return;
    }

    /* 先深后浅：先处理所有子对象 */
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (child != NULL) {
            cleanup_subtree(child, frame);
        }
    }

    int64_t h = lvglcj_handle_of(obj);
    if (h != LVGLCJ_HANDLE_NULL) {
        cleanup_one(h, frame);
    }
    /*
     * handle_of 返回 NULL 有两种情况，都不需要动作：
     *   · 该对象不是经绑定层创建的（LVGL 内部自造，§3.1.5 例外）；
     *   · 它已经被本函数的某次父级调用清理过（句柄已失效）。
     */
}

/* ------------------------------------------------------------ DELETE 钩子 */
void lvglcj_delete_hook(lv_event_t *e)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    if (target == NULL) {
        return;
    }

    lv_obj_t *parent = lv_obj_get_parent(target);
    int is_clean_child = (parent != NULL) && (g_cleaning_parent != 0) &&
                         (lvglcj_handle_of(parent) == g_cleaning_parent);

    /*
     * ★ 这里**不再**用 `lvglcj_handle_of(target) == NULL` 提前返回。
     *
     *   旧实现在此处直接 return，理由写的是「不是经绑定层创建的对象没有清理义务」。
     *   但那个判据把两种情况混为一谈：
     *     · 真的没经绑定层创建（LVGL 内部自造）—— 确实无需动作；
     *     · 已经被**父的钩子**在子树清理时顺手失效了 —— 它的资源也已经清完了，
     *       同样无需动作。
     *   两者都不需要动作，所以"不提前返回"是安全的；而提前返回会漏掉第三种情况：
     *   父的钩子失效了子句柄、却**还没来得及**清子的资源（旧实现正是如此，
     *   因为旧的 invalidate_subtree 只失效句柄、不做清理）。
     *
     *   现在清理由父一次做完（见 cleanup_subtree 的说明），
     *   所以这里直接交给 cleanup_subtree —— 它按每个节点各自判 handle_of，
     *   已清理过的节点自然跳过。不再需要在外层做这个判断。
     */
    cleanup_subtree(target, is_clean_child ? "obj_clean" : "obj_delete");
}

/* ------------------------------------------------------------ 安装钩子 */
void lvglcj_lifecycle_install_hook(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    /*
     * user_data 传 NULL：本钩子不是用户闭包，因此不需要 closure_id，
     * 也不会被 lvglcj_event_trampoline 分发。
     */
    (void)lv_obj_add_event_cb(obj, lvglcj_delete_hook, LV_EVENT_DELETE, NULL);
}

/* -------------------------------------------------- clean 标志的保存-恢复 */
int64_t lvglcj_lifecycle_begin_clean(int64_t parent_handle)
{
    int64_t saved = g_cleaning_parent;
    g_cleaning_parent = parent_handle;
    return saved;
}

void lvglcj_lifecycle_end_clean(int64_t saved)
{
    g_cleaning_parent = saved;
}
