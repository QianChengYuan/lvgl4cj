/*
 * callback.h —— 回调桥接契约（设计文档 §3.2，分支 B：自建闭包表 + closure_id）
 *
 * ============================ 重要前提 ============================
 * 仓颉的 CFunc Lambda **不能捕获变量**（cangjie-ffi 技能明确），
 * 因此设计文档 §3.2.4 的「分支 A（直接持有闭包指针 + pin 表）」在语言层
 * 即不成立，本工程**只实现分支 B**，不预留任何分支 A 代码路径。
 *
 * 分支 B 的数据流：
 *   仓颉侧持有真正的捕获闭包  HashMap<Int32, (Int64) -> Unit>
 *        │  registerClosure 分配 cid
 *        ▼
 *   C 侧 lv_obj_add_event_cb(obj, lvglcj_event_trampoline, code,
 *                            lvglcj_cid_to_ptr(cid))   ← user_data 装 cid
 *        │  LVGL 触发回调
 *        ▼
 *   trampoline 取出 cid，调用 g_dispatch(cid, arg)
 *        │  函数指针由仓颉在启动时注册（lvglcj_set_dispatch）
 *        ▼
 *   仓颉 dispatchClosure 查表 → **释放锁后**调用闭包 → 异常在此被捕获
 *
 * 关键约束：
 *   1. C 侧只保存**函数指针**，桥接库不引用任何仓颉符号
 *      → 规避静态库未定义符号与链接顺序问题
 *   2. cid ↔ user_data 必须显式经 intptr_t（ADR-001 / ADR-009）
 *   3. 回调抛出的异常绝不允许跨越 C 边界（设计文档 §3.8.1）
 * ==================================================================
 */
#ifndef LVGLCJ_CALLBACK_H
#define LVGLCJ_CALLBACK_H

#include <stdint.h>
#include <stddef.h>

#include "lvgl.h"
#include "lvglcj_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ cid 域 */
/*
 * closure_id 由仓颉侧分配。约定「0 与负值不参与分配」：
 *   cid = 0 表示「无闭包」（例如仅需内部 deleted_cb 的动画）。
 * 因此 user_data 永远不会是 NULL 与合法 cid 的二义值。
 */
#define LVGLCJ_CID_NONE ((int32_t)0)

/* ------------------------------------------------------------------ 分发器 */
/*
 * 统一的跨语言调用入口。arg 的语义按 trampoline 类型约定：
 *   event      -> 事件对象句柄（lv_event_t 注册进句柄表后的 id）
 *   timer      -> 定时器句柄
 *   anim_exec  -> 动画插值（int32 值，经 int64 传递）
 *   flush      -> display 句柄
 *   indev_read -> indev 句柄
 * 返回值：0 成功；非 0 表示仓颉闭包抛异常（已捕获），C 侧记录 CALLBACK_THREW。
 */
typedef int32_t (*lvglcj_dispatch_fn)(int32_t cid, int64_t arg);

/* 由仓颉侧在启动时注册；传 NULL 表示注销 */
int32_t lvglcj_set_dispatch(lvglcj_dispatch_fn fn);
lvglcj_dispatch_fn lvglcj_get_dispatch(void);

/* 内部使用：无分发器时返回 LVGLCJ_ERR_CALLBACK_THREW 并记录错误 */
int32_t lvglcj_call_closure(int32_t cid, int64_t arg);

/*
 * 保留参数值：dispatch(cid, LVGLCJ_ARG_UNREGISTER) 表示
 * 「请注销这个 cid，不要调用它」。
 *
 * 为什么需要这个协议（§3.1.4 步骤 (a)）：
 *   闭包表在**仓颉侧**，C 侧只持有 cid。当 LVGL 自行释放了回调载体
 *   （对象删除时 lv_event_dsc_t 被释放、动画结束时 ctx 被释放），
 *   C 侧必须通知仓颉侧把对应闭包摘掉，否则闭包表泄漏。
 *   INT64_MIN 与任何真实 arg（句柄、插值）都不可能冲突。
 */
#define LVGLCJ_ARG_UNREGISTER ((int64_t)(-9223372036854775807LL - 1))

/* 通知仓颉侧注销 cid（幂等；cid=0 时无操作） */
void lvglcj_closure_unregister(int32_t cid);

/* ------------------------------------------------------ closure_id ↔ 指针 */
/* 显式经 intptr_t，禁止直接把指针当地址用（ADR-001 / ADR-009：仅 64 位） */
static inline void *lvglcj_cid_to_ptr(int32_t cid)
{
    return (void *)(intptr_t)cid;
}

static inline int32_t lvglcj_ptr_to_cid(const void *p)
{
    return (int32_t)(intptr_t)p;
}

/* --------------------------------------------------------- 回调重入计数 */
/*
 * §3.8.2：回调执行期间 close() 必须转延迟删除。
 * 深度 > 0 即处于回调中；§3.8.1 规定回调嵌套深度上限 8。
 */
#define LVGLCJ_CALLBACK_DEPTH_MAX 8

void    lvglcj_callback_enter(const char *func);
void    lvglcj_callback_leave(void);
int32_t lvglcj_callback_depth(void);

/* ------------------------------------------------- trampoline 集合（§3.2.5） */
/*
 * T1 事件      lv_event_cb_t
 * T2 定时器    lv_timer_cb_t
 * T3 动画执行  lv_anim_exec_xcb_t          （arg = 插值，见 §3.10）
 * T4 显示刷新  lv_display_flush_cb_t        （v9 第三参是 uint8_t*，不是 v8 的 lv_color_t*）
 * T5 输入读取  lv_indev_read_cb_t
 * T6 文件系统  —— P0 不实现：MVP 用 LVGL 内置 POSIX 驱动（§3.13.1），
 *                自定义 lv_fs 驱动是 P2 前必须补的能力，不是可选项
 * 另有：动画内部 deleted_cb（Patch P1，anim_ctx 的唯一释放点）
 */
void lvglcj_event_trampoline(lv_event_t *e);
void lvglcj_timer_trampoline(lv_timer_t *t);
void lvglcj_anim_exec_trampoline(void *var, int32_t value);
void lvglcj_flush_trampoline(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
/*
 * flush_wait 是 T4 的伴生 trampoline（v9 签名无超时参数，超时在回调内部实现）。
 * 单独声明而非放在 display.c 里 static：它是 §3.2.5「trampoline 集合」的一员，
 * 放在这里才能保证「有哪些 trampoline」在一个地方看得全，不会随实现漂移。
 */
void lvglcj_flush_wait_trampoline(lv_display_t *disp);
void lvglcj_indev_read_trampoline(lv_indev_t *indev, lv_indev_data_t *data);

/* ------------------------------------------------- 线程断言（§3.6.5，Release 保留） */
/*
 * 注意：断言必须在 Release 构建下**依然生效**，否则跨线程数据竞争会变成
 * 间歇性崩溃（设计文档 §D.5 禁止事项）。
 * 方案 C 下 g_lvgl_thread_id 初始化为「创建 runtime 的线程 ID」。
 */
int32_t lvglcj_check_lvgl_thread(const char *func); /* LVGLCJ_OK 或 LVGLCJ_ERR_WRONG_THREAD */

#define LVGLCJ_CHECK_LVGL_THREAD_RET()                                    \
    do {                                                                  \
        int32_t _rc = lvglcj_check_lvgl_thread(__func__);                 \
        if (_rc != LVGLCJ_OK) return _rc;                                 \
    } while (0)

#define LVGLCJ_CHECK_LVGL_THREAD_VOID()                                   \
    do {                                                                  \
        if (lvglcj_check_lvgl_thread(__func__) != LVGLCJ_OK) return;      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_CALLBACK_H */
