/*
 * lvglcj_internal.h —— 桥接层**内部**共享声明
 *
 * 只被 native/src/*.c 使用，不对外（L1 仓颉侧看不见，也不该看见）。
 * 与 lvglcj_bridge.h 的分工：
 *   · lvglcj_bridge.h  = 对外 C ABI 契约（L1 ↔ L2）
 *   · 本文件           = L2 内部各 .c 之间的接缝
 */
#ifndef LVGLCJ_INTERNAL_H
#define LVGLCJ_INTERNAL_H

#include <stdint.h>

#include "lvglcj_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 初始化 */
/* 入口守卫：未初始化时记录 NOT_INITIALIZED 并返回错误码（§3.12） */
int32_t lvglcj_require_initialized(const char *func);
int32_t lvglcj_is_initialized(void);

/* ------------------------------------------------------------------ 线程 */
/*
 * 把「当前线程」登记为 LVGL 线程。
 * §3.6.5：方案 C 下应登记「创建 runtime 的线程」，方案 A 下由 C 侧主循环线程自登记。
 * 检查逻辑两边一致 —— 方案 C 下跨仓颉线程调 pump() 会被正确拦截。
 */
void lvglcj_thread_bind_current(void);

/* 定时器处理一次的间隔（方案 A 主循环的睡眠时长），默认 5ms */
#define LVGLCJ_LOOP_SLEEP_MS 5

/* ============================================ 延迟删除队列（§3.8.2） */
typedef enum {
    LVGLCJ_DEFER_DELETE = 0, /* 延迟删除对象 */
    LVGLCJ_DEFER_UNREG = 1   /* 延迟注销闭包 */
} lvglcj_defer_op_t;

int32_t lvglcj_deferred_init(void);
void    lvglcj_deferred_destroy(void);
int32_t lvglcj_deferred_push(int32_t op, int64_t handle, int32_t cid);
int32_t lvglcj_deferred_count(void);
/*
 * 可观测性：因「连续 3 帧超过 64 轮」而被强制同步清空的次数。
 * 正常长跑中应恒为 0；非 0 说明出现了队列持续删不完的异常场景（R19）。
 */
int32_t lvglcj_deferred_forced_drains(void);

/* ============================ 对象级注册表（t3/t7 共用） */
/*
 * 为什么需要它，而不是去枚举 LVGL 的事件描述符数组：
 *   DELETE 钩子在 LVGL 删除对象的**过程中**执行，
 *   此时对象的事件数组是否仍然完整，属于 LVGL 内部实现细节。
 *   把「这个对象有哪些闭包 / 哪些动画」记在桥接层自己的注册表里，
 *   生命周期清理就**不依赖 LVGL 内部状态**，行为可预期、可单测。
 *
 * 复杂度说明：P0 采用紧凑数组 + 线性扫描。
 *   正常 UI 中单个对象的回调/动画数量是个位数，对象总数是几十到几百，
 *   因此这里的 O(n) 扫描不在热路径上（热路径是句柄查找与闭包查表，均为 O(1)）。
 *   若 P1 的大规模场景（万级对象带事件）出现瓶颈，再换成按 obj_handle 哈希。
 */
int32_t lvglcj_reg_event_add(int64_t obj_handle, void *dsc, int32_t cid);
void    lvglcj_reg_event_remove_by_dsc(const void *dsc);
/* 取该对象的全部 cid；返回写入个数 */
int32_t lvglcj_reg_event_cids_of(int64_t obj_handle, int32_t *out, int32_t max);
int32_t lvglcj_reg_event_count_of(int64_t obj_handle);
/* 丢弃该对象的全部记录（DELETE 钩子中调用） */
void    lvglcj_reg_event_drop(int64_t obj_handle);

int32_t lvglcj_reg_anim_bind(int64_t obj_handle, int64_t anim_handle);
void    lvglcj_reg_anim_unbind(int64_t obj_handle, int64_t anim_handle);
int32_t lvglcj_reg_anim_list_of(int64_t obj_handle, int64_t *out, int32_t max);
int32_t lvglcj_reg_anim_count_of(int64_t obj_handle);

/* ------------------------------------ 样式使用者注册表（t8 新增，style.c 依赖） */
/*
 * 【为什么必须有这张表】
 *   lv_style_t 是**引用型**资源：lv_obj_add_style 只保存指针，不拷贝。
 *   因此「样式必须先于使用它的对象被释放」是一个真实约束 ——
 *   而 ASan 实测（t8）证明违反它的后果是 **heap-use-after-free**：
 *   释放样式后删除对象时，LVGL 遍历对象的样式链表会读到已释放内存。
 *   更糟的是这个 UAF 不一定立刻崩：它可能静默读到垃圾值（样式算错、尺寸错乱）。
 *
 *   查表让 lvglcj_style_delete 能**明确拒绝**「还在被引用」的样式并报出引用者数量，
 *   把静默 UAF 变成一条可操作的报错。这正是 style.c 里那句
 *   「只能在 C 侧自己维护注册表」预判的方案。
 *
 * 记录的是 (对象, 样式) 对；对象的 DELETE 钩子会清掉它自己的全部记录。
 */
int32_t lvglcj_reg_style_bind(int64_t obj_handle, int64_t style_handle, int32_t selector);
void    lvglcj_reg_style_unbind(int64_t obj_handle, int64_t style_handle, int32_t selector);
void    lvglcj_reg_style_unbind_obj(int64_t obj_handle);
/*
 * 仍在引用该样式的**存活**对象数。
 * 顺带清理「对象句柄已不再存活」的陈旧记录 —— 这样即使某条 unbind 路径漏了，
 * 也不会永久阻塞样式释放（它只影响计数，不影响正确性）。
 */
int32_t lvglcj_reg_style_user_count(int64_t style_handle);

/* 释放注册表内存（runtime.c 在 deinit 时调用） */
void    lvglcj_registry_destroy(void);

/* ============================================= 生命周期 / 对象（内部接口） */
/*
 * 挂 DELETE 钩子（§3.3.3）：统一在对象创建入口调用，
 * 保证所有经绑定层创建的对象都能被感知。
 */
void lvglcj_lifecycle_install_hook(lv_obj_t *obj);

/* DELETE 钩子本体（lifecycle.c）——仅供 obj.c 挂载时引用 */
void lvglcj_delete_hook(lv_event_t *e);

/* 递归失效整棵子树（先深后浅，叶子→根，§3.3.1） */
/*
 * 原先这里声明的是 `lvglcj_invalidate_subtree(lv_obj_t *)`（只失效子句柄）。
 * 该函数已改为 lifecycle.c 内的 static `cleanup_subtree(obj, frame)` ——
 * 因为它现在必须**同时**清理每个后代的资源（闭包/样式/动画），而不只是失效句柄。
 * 原因见 lifecycle.c 中 cleanup_subtree 上方的说明（LVGL 先给父发 DELETE、
 * 后删子对象，导致子对象自己的钩子认不出自己）。
 * 它不再对外暴露：外部没有"只清理子树、不删对象"的合理用途。
 */

/*
 * clean 标志的保存-恢复（§3.3.2）。返回上一次的值，供 end 还原，
 * 因此**支持嵌套 clean**（obj_clean 内部再调 obj_clean 也安全）。
 */
int64_t lvglcj_lifecycle_begin_clean(int64_t parent_handle);
void    lvglcj_lifecycle_end_clean(int64_t saved);

/* 立即删除（不走延迟队列）——deferred.c 与 obj.c 共用 */
int32_t lvglcj_obj_delete_now(int64_t h);

/*
 * 内部函数（§3.10.3 命名统一）：停掉某对象的全部动画。
 * 不对外暴露；对外 API 是 lvglcj_obj_delete_anim。
 * ★ 本函数**不 free(ctx)** —— ctx 由动画的内部 deleted_cb 唯一释放（Patch P1），
 *   在这里 free 会造成双重 free。
 */
void lvglcj_stop_all_anims_of_obj(int64_t obj_handle);

/* ==================================== 调试 / 可观测性（§7.3，debug.c） */
/* 单调时钟（微秒）。放在这里是因为性能埋点与计时都要用，不应各自实现一份。 */
uint64_t lvglcj_now_us(void);

/*
 * 性能埋点。这两个函数是 debug.c 与主循环之间的**唯一**接口 ——
 * 把埋点集中成两个调用点，好处是「哪些路径会影响 FPS 统计」一目了然：
 *   · lvglcj_perf_on_flush()         T4 flush trampoline 每次调用后（display.c）
 *   · lvglcj_perf_on_timer_handler() lvglcj_timer_handler 每次返回后（timer.c）
 * 口径说明见 debug.c 文件头。
 */
void lvglcj_perf_on_flush(uint32_t flush_us, int32_t is_last);
void lvglcj_perf_on_timer_handler(uint32_t handler_us);

/* ============================================ 任务队列（§3.9，queue.c） */
/*
 * 队列对外的投递/配置 API 在 lvglcj_bridge.h（L1 可见）；
 * 这里只声明 L2 内部需要的生命周期与 drain 接口。
 */
int32_t lvglcj_queue_init(void);
void    lvglcj_queue_destroy(void);
/*
 * 每帧调用一次：把排队的任务逐个交给 dispatch 执行。
 * 由 thread.c 的主循环（方案 A）与 pump()（方案 C）共同调用 ——
 * 这是两套线程方案**共用同一套队列抽象**的落点。
 */
int32_t lvglcj_queue_drain(void);
/* 关闭：Drain 做完剩下的 / Discard 丢弃并唤醒等待者 */
int32_t lvglcj_queue_shutdown(int32_t shutdown_mode);
int32_t lvglcj_queue_is_shutting_down(void);
int32_t lvglcj_queue_capacity(void);
/* which: 0=accepted 1=rejected 2=executed 3=dropped；其它返回 -1 */
int64_t lvglcj_queue_stat(int32_t which);

/* ==================== 控件创建的共用步骤（widgets.c / canvas.c 共用）====================
 *
 * 抽出来的理由不是少写几行，而是**去掉多份拷贝各自漂移的可能**：
 * 「创建 → 登记 → 挂 DELETE 钩子」这段流程一旦某处漏掉挂钩子，
 * 就会产生悬空句柄（isAlive 为 true 而底层对象已消失，即 §3.3 要防的那种）。
 * widgets.c 与 canvas.c 现在共用这两个函数，因此不会出现
 * "一处记得挂、另一处忘了"。
 *
 * 另外 lvglcj_widget_parent_of 用「错误码 + 出参」而不是"直接返回指针"，
 * 是因为 parent == 0 是**合法输入**（表示挂到当前屏幕），解析结果也是 NULL ——
 * 与"父句柄无效"的 NULL 无法区分，混成一个会让"传错句柄"被静默当成"挂到屏幕上"。
 */
int32_t lvglcj_widget_parent_of(int64_t parent, const char *fn_name, lv_obj_t **out);
int64_t lvglcj_widget_register_created(lv_obj_t *obj, const char *type_name);

/* ============================ 后端 sink 钩子（display.c） */
/*
 * 为什么需要它：后端（SDL2）必须在 flush 时拿到「本次刷新的区域 + 像素指针」
 * 才能上传纹理，而像素指针**绝不能交给仓颉侧**（ADR-002：GC 会移动缓冲）。
 * 用一个 C 函数指针把这件事留在 C 侧即可 —— 与 T5 的输入回填是同一思路。
 *
 * 与 flush cid 的关系（两者可同时存在）：
 *   · sink 先执行，负责真实的上传/呈现；它自己决定何时 flush_ready
 *   · 之后若注册了仓颉 cid，仍会调用它，供仓颉侧观测或叠加行为
 *   · 两者都没有时，trampoline 自动 flush_ready（headless 路径）
 */
typedef void (*lvglcj_flush_sink_t)(int64_t disp_handle, const lv_area_t *area,
                                    uint8_t *px_map, uint32_t stride, void *user);
/* flush_wait 的 sink：sink 内部必须自带超时（§8.1.2） */
typedef void (*lvglcj_flush_wait_sink_t)(int64_t disp_handle, void *user);

int32_t lvglcj_display_set_flush_sink(int64_t disp, lvglcj_flush_sink_t sink, void *user);
int32_t lvglcj_display_set_flush_wait_sink(int64_t disp, lvglcj_flush_wait_sink_t sink,
                                          void *user);

/* ---------------------------------------- 等待图（§3.8.4，实现在 queue.c） */
/*
 * 渲染线程身份由后端注册（t5 SDL2）；tid=0 表示注销。
 * 在 t5 之前该判据自动失效（g_render_thread_tid == 0），不影响其它两条判据。
 */
void    lvglcj_waitgraph_set_render_thread(int64_t tid);
int64_t lvglcj_waitgraph_render_thread(void);
/*
 * 返回 NULL 表示当前线程同步等待 LVGL 安全；
 * 否则返回**静态**原因字符串（给用户看，不得 free）。
 */
const char *lvglcj_waitgraph_risk_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_INTERNAL_H */
