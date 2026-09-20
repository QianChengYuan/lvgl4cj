/*
 * dispatch.c —— 跨语言调用分发器（设计文档 §3.2.3 分支 B 的核心）
 *
 * 这是整个回调桥接的关键接缝，也是「C 侧不引用任何仓颉符号」的实现手段：
 *   仓颉启动时调 lvglcj_set_dispatch(fn) 把函数指针交过来，
 *   C 侧只保存指针，链接期不需要解析任何仓颉符号
 *   → 静态库不会有未定义符号，也不需要依赖 @C 导出符号名的规则。
 *
 * 另一条约定：回调重入计数（§3.8.2）。
 *   deferred.c 用 lvglcj_callback_depth() > 0 判断「当前是否在回调里」，
 *   从而决定 close() 是立即执行还是转延迟删除。
 */
#include "lvglcj_bridge.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

static lvglcj_dispatch_fn g_dispatch = NULL;
static pthread_mutex_t    g_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

/* 回调深度：用原子量，因为方案 A 下压栈/出栈发生在 C 线程，而查询可能来自别处 */
static _Atomic int32_t g_callback_depth = 0;

int32_t lvglcj_set_dispatch(lvglcj_dispatch_fn fn)
{
    pthread_mutex_lock(&g_dispatch_lock);
    g_dispatch = fn;
    pthread_mutex_unlock(&g_dispatch_lock);
    return LVGLCJ_OK;
}

lvglcj_dispatch_fn lvglcj_get_dispatch(void)
{
    lvglcj_dispatch_fn fn;
    pthread_mutex_lock(&g_dispatch_lock);
    fn = g_dispatch;
    pthread_mutex_unlock(&g_dispatch_lock);
    return fn;
}

int32_t lvglcj_call_closure(int32_t cid, int64_t arg)
{
    if (cid == LVGLCJ_CID_NONE) {
        /* 合法情形：只挂内部回调、没有用户闭包（例如纯 lv_anim 的 deleted_cb） */
        return LVGLCJ_OK;
    }

    lvglcj_dispatch_fn fn = lvglcj_get_dispatch();
    if (fn == NULL) {
        /*
         * 没有分发器却要调闭包 = 编程错误。
         * 记录但不崩溃：探针与单测会断言这个计数为 0。
         */
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, 0, cid, __func__,
                            "dispatch 未注册（lvglcj_set_dispatch 未被调用）");
        return LVGLCJ_ERR_CALLBACK_THREW;
    }

    /*
     * ★ 闭包调用必须在回调深度保护下进行：
     *   仓颉闭包内若调 obj.close()，deferred.c 依据深度决定转延迟删除。
     */
    lvglcj_callback_enter(__func__);
    int32_t rc = fn(cid, arg);
    lvglcj_callback_leave();

    if (rc != 0) {
        /* 仓颉侧已捕获异常并返回非 0；这里只负责记录，绝不跨越 C 边界抛 */
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, 0, cid, __func__, NULL);
    }
    return rc;
}

void lvglcj_closure_unregister(int32_t cid)
{
    if (cid == LVGLCJ_CID_NONE) {
        return;
    }
    lvglcj_dispatch_fn fn = lvglcj_get_dispatch();
    if (fn == NULL) {
        return; /* 尚未注册分发器：仓颉侧的表也还不存在，无需通知 */
    }
    /* 不进入回调深度保护：这不是「用户回调」，不应影响延迟删除判定 */
    (void)fn(cid, LVGLCJ_ARG_UNREGISTER);
}

/* ------------------------------------------------------------------ 深度 */
void lvglcj_callback_enter(const char *func)
{
    int32_t d = atomic_fetch_add(&g_callback_depth, 1) + 1;
    if (d > LVGLCJ_CALLBACK_DEPTH_MAX) {
        /* §3.8.1：回调中触发新事件的嵌套深度上限 8，超出即报错 */
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, 0, 0, func,
                            "回调嵌套深度超过上限 8");
    }
}

void lvglcj_callback_leave(void)
{
    int32_t d = atomic_fetch_sub(&g_callback_depth, 1) - 1;
    if (d < 0) {
        /* 出栈多于入栈 = 内部计数错乱，立即复位并报警 */
        atomic_store(&g_callback_depth, 0);
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, 0, 0, __func__,
                            "回调深度计数下溢，已复位为 0");
    }
}

int32_t lvglcj_callback_depth(void)
{
    return atomic_load(&g_callback_depth);
}
