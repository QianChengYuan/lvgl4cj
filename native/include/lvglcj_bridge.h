/*
 * lvglcj_bridge.h —— lvgl4cj 的 C ABI 全量声明（设计文档 §五）
 *
 * 约定：
 *   · 命名 lvglcj_<子系统>_<动作>，全 extern "C"，无 C++ 符号
 *   · 返回 int32_t 错误码：0 成功 / 正数特殊标记 / 负数错误（见 lvglcj_error.h）
 *   · 对象一律用 int64_t 句柄（自增 ID，非指针地址）
 *   · 本头文件是 L1（仓颉）与 L2（C 桥接）之间的唯一契约；
 *     仓颉侧不 include 本文件，而是用 foreign func 逐条声明（src/ffi/bridge.cj），
 *     两侧一致性由 test/ffi_contract_test.cj 断言。
 *
 * MVP 支持矩阵（§1.2）：仅 64 位；单 LVGL OS 线程；LVGL 锁 v9.2+。
 * 非目标（§1.4）：lv_draw_*、LV_EVENT_DRAW_*、自定义 lv_fs 驱动、MCU/RTOS、32 位。
 */
#ifndef LVGLCJ_BRIDGE_H
#define LVGLCJ_BRIDGE_H

#include <stdint.h>

#include "lvgl.h"
#include "lvglcj_error.h"
#include "handle_table.h"
#include "callback.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ §5.1 运行时与线程 */
int32_t     lvglcj_init(void);
int32_t     lvglcj_deinit(void);
/* tick 由 C 侧接管：内部用 CLOCK_MONOTONIC，不依赖仓颉时钟（§3.6） */
int32_t     lvglcj_set_tick_cb(void);
uint32_t    lvglcj_tick_get(void);
uint32_t    lvglcj_tick_elaps(uint32_t prev);

const char *lvglcj_version(void);
uint32_t    lvglcj_conf_hash(void);

/*
 * 日志转发（§3.12.3）
 * ★ 与设计文档 §5.1 的一处有据偏离：原文签名是 lvglcj_set_log_cb(int32_t cid)，
 *   即让日志也走闭包表。但闭包表签名是 (Int64) -> Unit，装不下日志字符串；
 *   强行把 LVGL 的 buf 指针塞进 int64 会在回调返回后变成悬空指针。
 *   故改用与错误回调一致的**函数指针注册**（lvglcj_log_cb_t）。
 *
 * 注册接口是 lvglcj_log_set_cb(lvglcj_log_cb_t)，声明在 lvglcj_error.h
 * （与错误回调放在一起，避免同名双份声明造成漂移）。
 * 这里**不再重复声明** —— 实测教训：曾同时存在 lvglcj_set_log_cb 与
 * lvglcj_log_set_cb 两个名字，Cangjie 侧声明了前者而实现是后者，
 * 直到链接期才以 undefined reference 暴露。
 * 传 NULL 表示注销（此时 Warn 及以上退回 stderr）。
 */
/* 安装 LVGL 原生日志钩子；需在 lv_init() 之后调用 */
int32_t lvglcj_log_init(void);

/* 宏 → 查询函数（§3.5.1）：让仓颉侧能读到编译期配置，并在启动时校验哈希 */
int32_t lvglcj_conf_color_depth(void);
int32_t lvglcj_conf_use_log(void);
int32_t lvglcj_conf_mem_size(void);
int32_t lvglcj_conf_stride_align(void);
int32_t lvglcj_conf_use_fs_posix(void);
int32_t lvglcj_conf_def_refr_period(void);
int32_t lvglcj_conf_version_major(void);
int32_t lvglcj_conf_version_minor(void);
int32_t lvglcj_conf_version_patch(void);

/*
 * 启动 LVGL 主循环线程（方案 A：C 侧 pthread_create）。
 * 返回 LVGLCJ_ERR_NOT_SUPPORTED 表示当前构建不支持（仓颉无法在外部 OS 线程执行闭包
 * → 由 V1 探针判定，此时退回方案 C 的 lvglcj_pump）。
 */
int32_t lvglcj_start_thread(void);
int32_t lvglcj_stop_thread(int32_t shutdown_mode);

/* 方案 C（泵模式）：由仓颉主循环周期性调用；方案 A 下调用返回 NOT_SUPPORTED */
int32_t lvglcj_pump(void);

int64_t lvglcj_current_os_tid(void);
int32_t lvglcj_is_lvgl_thread(void);
/*
 * 显式声明「当前线程就是 LVGL 线程」（§3.6.5）。
 *   方案 C（泵模式）下由用户主循环线程调用 —— 不调用则 pump() 会被线程断言误拦。
 *   方案 A 下**不应调用**（主循环线程自行绑定）。
 *   仓颉测试/工具在多线程框架下也需要它来固定亲和性。
 */
int32_t lvglcj_rebind_current_thread(void);

/* 任务投递（§3.9）：task_id 由仓颉侧分配，C 侧只透传给 dispatch */
int32_t lvglcj_post_task(int64_t task_id);
int32_t lvglcj_post_and_wait(int64_t task_id); /* 含等待图死锁检测（§3.8.4） */

/* 延迟删除队列 drain（§3.8.2），每帧调用一次 */
void    lvglcj_drain_deferred(void);

/* 关闭策略 / 队列满策略，与仓颉侧 ShutdownMode / QueueFullPolicy 数值对齐 */
typedef enum {
    LVGLCJ_SHUTDOWN_DRAIN = 0,
    LVGLCJ_SHUTDOWN_DISCARD = 1,
    LVGLCJ_SHUTDOWN_DRAIN_WITH_TIMEOUT = 2
} lvglcj_shutdown_mode_t;

typedef enum {
    LVGLCJ_QUEUE_FULL_FAIL_FAST = 0,
    LVGLCJ_QUEUE_FULL_BLOCK = 1,
    LVGLCJ_QUEUE_FULL_DROP_OLDEST = 2
} lvglcj_queue_full_policy_t;

int32_t lvglcj_queue_set_capacity(int32_t capacity);
int32_t lvglcj_queue_set_full_policy(int32_t policy);
int32_t lvglcj_queue_size(void);

/* ============================================================ §5.2 句柄 */
/*
 * ★ 幂等性职责划分（全层统一，勿逐 API 例外）
 *
 *   设计文档 §3.1.3 的幂等性契约（close()/release() 重复调用无操作）
 *   由**仓颉 L1 层**实现，C 侧业务 API 一律**严格**：
 *
 *     C 侧（本头文件）：非 ALIVE 句柄 → 一律返回 LVGLCJ_ERR_INVALID_HANDLE
 *                       本层**不提供**「重复删除返回成功」的宽容语义
 *     仓颉侧（close()）：把 INVALID_HANDLE 视为成功，从而实现幂等
 *
 *   这样做的理由：仓颉侧只需记住**一条**规则（`rc == INVALID_HANDLE` 即当成功），
 *   而不必为每个子系统记住「谁宽容、谁严格」。
 *   反例（不要这样）：若 display 的删除宽容、obj 的删除严格，
 *   那么 close() 的实现就要按类型分支，迟早漏掉一个。
 *
 *   注意区分：删除**父对象**时，其子对象句柄进入 INVALIDATED（表项保留，
 *   便于给出明确报错与保留删除栈），而**被直接删除的那个对象**句柄会
 *   一路推进到 RELEASED（表项移除，state 查询返回 UNINIT）。
 */
/* 四态句柄表的对外部分；register / ptr_of 供桥接层内部与探针使用 */
int64_t lvglcj_handle_register(void *ptr, const char *name);
void   *lvglcj_ptr_of(int64_t h);
int64_t lvglcj_handle_of(void *ptr);
int32_t lvglcj_handle_alive(int64_t h);
int32_t lvglcj_handle_state(int64_t h);
void    lvglcj_handle_invalidate(int64_t h);
void    lvglcj_handle_release(int64_t h);
int32_t lvglcj_handle_pending_delete(int64_t h);
int32_t lvglcj_handle_count(int32_t state);

/* ============================================================ §5.3 Display */
/*
 * buf_mode: 0=PARTIAL（MVP 默认，≥1/10 屏）1=DIRECT（整屏，只刷脏区）2=FULL（整屏全刷）
 * buf_lines: PARTIAL 下有效；DIRECT/FULL 下**被忽略**（非零时记 WARN 日志，不报错）
 *
 * ★ 缓冲一律在 C 侧分配并由桥接层持有（ADR-002）：
 *   绝不能把仓颉 Array<UInt8> 的底层指针传给 LVGL —— GC 可能移动或回收。
 * ★ 尺寸计算一律走 lv_draw_buf_width_to_stride，绝不自己算（§3.4.2）。
 */
typedef enum {
    LVGLCJ_BUF_PARTIAL = 0,
    LVGLCJ_BUF_DIRECT = 1,
    LVGLCJ_BUF_FULL = 2
} lvglcj_buf_mode_t;

int64_t lvglcj_display_create(int32_t w, int32_t h, int32_t color_format,
                              int32_t buf_mode, int32_t buf_lines);
int32_t lvglcj_display_set_color_format(int64_t disp, int32_t fmt);
int32_t lvglcj_display_get_color_format(int64_t disp);
int32_t lvglcj_display_set_flush_cb(int64_t disp, int32_t cid);
int32_t lvglcj_display_set_flush_wait_cb(int64_t disp, int32_t cid);
int32_t lvglcj_display_flush_ready(int64_t disp);
int32_t lvglcj_display_flush_is_last(int64_t disp);
int32_t lvglcj_display_set_rotation(int64_t disp, int32_t rot);
int32_t lvglcj_display_set_resolution(int64_t disp, int32_t w, int32_t h);
int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid);
int32_t lvglcj_display_delete(int64_t disp);

/* 绘制缓冲区只读信息（调试 / 测试用） */
int64_t lvglcj_display_get_buf_bytes(int64_t disp);
int32_t lvglcj_display_get_buf_stride(int64_t disp);

/*
 * 测试支持：对绘制缓冲做一次像素级体检（不暴露指针）。
 *
 * 返回**打包值**：高 32 位 = 不同像素值个数（上限 64，排除纯色/花屏），
 *                低 32 位 = 非零像素个数（排除全黑）；**负数表示出错**。
 *
 * 之所以打包而不是用两个出参：出参要求仓颉侧构造 CPointer 并写指针，
 * 容易引入指针算法错误；而这两个值都是非负计数，打包后「负数=出错」无歧义。
 *
 * 存在理由：仓颉侧看不到绘制缓冲（ADR-002），这正是「画面是否正确」
 * 无法在仓颉侧断言的根因。详见 display.c 中的说明。
 */
int64_t lvglcj_display_sample_buf(int64_t disp);

/* ============================================================ §5.4 InDev + Group */
/*
 * ★ 修正（相对早期版本的一处取值错误）：
 *   LVGL v9.2 的 lv_indev_type_t 是 NONE=0 / POINTER=1 / KEYPAD=2 /
 *   **BUTTON=3 / ENCODER=4**。早期版本把 ENCODER/BUTTON 写反了（3/4 互换），
 *   那会让「编码器」被当成「按钮」处理 —— 且不会报错，只是行为诡异。
 *   现在**逐值对齐 LVGL**，这样 C 侧无需任何映射表，也就不会再有机会写反。
 *   新增设备类型时请先去 lv_indev.h 核对，不要凭记忆续写。
 */
typedef enum {
    LVGLCJ_INDEV_NONE = 0,
    LVGLCJ_INDEV_POINTER = 1,
    LVGLCJ_INDEV_KEYPAD = 2,
    LVGLCJ_INDEV_BUTTON = 3,   /* == LV_INDEV_TYPE_BUTTON */
    LVGLCJ_INDEV_ENCODER = 4   /* == LV_INDEV_TYPE_ENCODER */
} lvglcj_indev_type_t;

int64_t lvglcj_indev_create(int32_t type);
int32_t lvglcj_indev_set_read_cb(int64_t indev, int32_t cid);
int32_t lvglcj_indev_set_display(int64_t indev, int64_t disp);
int32_t lvglcj_indev_set_group(int64_t indev, int64_t group);
int32_t lvglcj_indev_add_event(int64_t indev, int32_t code, int32_t cid);
int32_t lvglcj_indev_delete(int64_t indev);

/*
 * ---- T5 输入数据回填（★ 与设计文档 §5.4 的一处有据补充）----
 *
 * 问题：indev_read 回调必须把按压位置/按键写成 lv_indev_data_t，
 *   但设计文档没有给出仓颉侧回填该结构体的手段。
 *   两条路可选：
 *     (a) 把 lv_indev_data_t* 原样交给仓颉，由仓颉按 C 布局写裸内存；
 *     (b) C 侧暂存该指针，另给一组 setter。
 *
 *   选 (b)，理由是 (a) 不可靠：lvglcj_offsets_t 只导出 sizeof_indev_data、
 *   **不导出字段偏移**，而 lv_indev_data_t 内部还嵌了 lv_point_t。
 *   让仓颉侧靠「字段顺序猜布局」去写，一旦 LVGL 小版本调整字段顺序或对齐，
 *   就会变成静默的输入错乱（点了 A 触发 B），比崩溃更难发现。
 *   setter 方案把布局知识完全留在 C 侧，仓颉侧只传语义值。
 *
 * 使用约束：**只能在 indev_read 回调执行期间调用**。
 *   回调之外调用返回 LVGLCJ_ERR_NOT_SUPPORTED（没有「当前正在读取的设备」），
 *   这是刻意的 —— 输入数据是「本次读取」的瞬时状态，跨回调缓存它没有意义。
 */
int32_t lvglcj_indev_set_point(int32_t x, int32_t y, int32_t pressed);
int32_t lvglcj_indev_set_key(uint32_t key, int32_t pressed);
int32_t lvglcj_indev_set_enc_diff(int32_t diff);
/* 置 1 表示本次读取后立即再次读取（连续采样），场景少见但 v9 支持 */
int32_t lvglcj_indev_set_continue(int32_t cont);

int64_t lvglcj_group_create(void);
int32_t lvglcj_group_delete(int64_t g);
int32_t lvglcj_group_add_obj(int64_t g, int64_t obj);
int32_t lvglcj_group_remove_obj(int64_t obj);
int32_t lvglcj_group_focus_obj(int64_t obj);
int64_t lvglcj_group_get_focused(int64_t g);
int32_t lvglcj_group_focus_next(int64_t g);
int32_t lvglcj_group_focus_prev(int64_t g);
int32_t lvglcj_group_set_default(int64_t g);

/* ============================================================ §5.5 对象 */
int64_t lvglcj_obj_create(int64_t parent);
int32_t lvglcj_obj_delete(int64_t obj);
int32_t lvglcj_obj_clean(int64_t obj);
int32_t lvglcj_obj_set_pos(int64_t obj, int32_t x, int32_t y);
int32_t lvglcj_obj_set_size(int64_t obj, int32_t w, int32_t h);
int32_t lvglcj_obj_set_parent(int64_t obj, int64_t parent);
int32_t lvglcj_obj_align(int64_t obj, int32_t align, int32_t x_ofs, int32_t y_ofs);
int32_t lvglcj_obj_add_flag(int64_t obj, int32_t flag);
int32_t lvglcj_obj_remove_flag(int64_t obj, int32_t flag);
int32_t lvglcj_obj_add_state(int64_t obj, int32_t state);
int32_t lvglcj_obj_remove_state(int64_t obj, int32_t state);
int32_t lvglcj_obj_get_x(int64_t obj);
int32_t lvglcj_obj_get_y(int64_t obj);
int32_t lvglcj_obj_get_width(int64_t obj);
int32_t lvglcj_obj_get_height(int64_t obj);
int64_t lvglcj_screen_active(void);
int32_t lvglcj_screen_load(int64_t scr);
int32_t lvglcj_screen_load_anim(int64_t scr, int32_t anim_type, int32_t time_ms,
                                int32_t delay_ms, int32_t auto_del);
int64_t lvglcj_obj_get_child(int64_t obj, int32_t idx);
int32_t lvglcj_obj_get_child_count(int64_t obj);
int32_t lvglcj_obj_move_foreground(int64_t obj);
int32_t lvglcj_obj_move_background(int64_t obj);
int32_t lvglcj_obj_move_to_index(int64_t obj, int32_t index);
int32_t lvglcj_obj_swap(int64_t obj1, int64_t obj2);
/* 布局（P0 只需 flex 最小集，§12 任务5） */
int32_t lvglcj_obj_set_flex_flow(int64_t obj, int32_t flow);
int32_t lvglcj_obj_set_flex_align(int64_t obj, int32_t main_place, int32_t cross_place,
                                  int32_t track_place);
int32_t lvglcj_obj_set_style_pad_all(int64_t obj, int32_t value, int32_t selector);
int32_t lvglcj_obj_set_style_pad_gap(int64_t obj, int32_t value, int32_t selector);

/* ============================================================ §5.6 事件 */
/*
 * 注册事件回调：返回 event descriptor 句柄（用于注销），失败返回 0。
 * ★ LV_EVENT_DRAW_* 系列直接返回 LVGLCJ_ERR_NOT_SUPPORTED（§3.7.3）
 */
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int32_t cid);
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t ev_dsc);
int32_t lvglcj_obj_remove_event_by_cid(int64_t obj, int32_t cid);
/*
 * param 双路径（§5.6）：
 *   路径 1（句柄）：param 是句柄表注册的对象句柄 → 接收侧用 lvglcj_event_get_user_data
 *   路径 2（标量）：param 是经 intptr_t 编码的标量 → 接收侧用 lvglcj_event_get_param_scalar
 * 两条路径由注册侧决定，接收侧必须用对应 API。
 */
int32_t lvglcj_obj_send_event(int64_t obj, int32_t code, int64_t param);
/*
 * ★ 与设计文档 §5.6 的一处有据偏离：原文用单一入口承载两条 param 语义
 *   （句柄 或 编码后的标量），但发送侧无法在 C 层可靠区分二者 ——
 *   「标量恰好等于某个句柄 ID」会造成静默误判。
 *   故拆成两个**显式**入口：上面的句柄路径，与下面的标量路径。
 * 接收侧仍分别用 lvglcj_event_get_user_data / lvglcj_event_get_param_scalar 取回。
 */
int32_t lvglcj_obj_send_event_scalar(int64_t obj, int32_t code, int64_t param_scalar);

/*
 * ★ 与设计文档 §5.6 的第二处有据偏离（重要，影响用户可见 API）：
 *   原文的「句柄路径」是「param 放句柄 → 接收侧用 lvglcj_event_get_user_data 取回」。
 *   但分支 B **已经用 user_data 装载 closure_id**（trampoline 找回闭包的唯一凭据），
 *   二者不可能共存；且 LVGL v9.2 也没有运行期设置 user_data 的接口。
 *
 *   正确做法：载荷一律走 **param**，并提供两个对称的读取入口：
 *     lvglcj_event_get_param_handle()  ← 发送侧用 lvglcj_obj_send_event（句柄路径）
 *     lvglcj_event_get_param_scalar()  ← 发送侧用 lvglcj_obj_send_event_scalar（标量路径）
 */
int32_t lvglcj_event_get_code(int64_t evh);
int64_t lvglcj_event_get_target(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);
int64_t lvglcj_event_get_param_handle(int64_t evh);
int64_t lvglcj_event_get_param_scalar(int64_t evh);
/* 诊断用：当前正在处理该事件的是哪个闭包（即 user_data 里装的 closure_id） */
int32_t lvglcj_event_get_closure_id(int64_t evh);
int32_t lvglcj_event_stop_bubbling(int64_t evh);
/* v9.2 无 lv_event_stop_trickling；对应能力是 stop_processing，故依真实语义命名 */
int32_t lvglcj_event_stop_processing(int64_t evh);

/* ============================================================ §5.7 样式 */
/*
 * v9 已去掉 state 参数，改用 selector（= Part | State 组合）。
 * 以下为 P0「样式最小集」，覆盖 §5.7 的全部类别；P1 补齐余下属性。
 */
int64_t lvglcj_style_create(void);
int32_t lvglcj_style_delete(int64_t style);

int32_t lvglcj_style_set_bg_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_bg_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_bg_grad_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_bg_grad_dir(int64_t s, int32_t dir);
int32_t lvglcj_style_set_border_width(int64_t s, int32_t w);
int32_t lvglcj_style_set_border_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_border_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_radius(int64_t s, int32_t r);
int32_t lvglcj_style_set_pad_all(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_top(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_bottom(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_left(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_right(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_row(int64_t s, int32_t v);
int32_t lvglcj_style_set_pad_column(int64_t s, int32_t v);
int32_t lvglcj_style_set_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_height(int64_t s, int32_t v);
int32_t lvglcj_style_set_min_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_max_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_min_height(int64_t s, int32_t v);
int32_t lvglcj_style_set_max_height(int64_t s, int32_t v);
int32_t lvglcj_style_set_text_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_text_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_text_font(int64_t s, int64_t font);
int32_t lvglcj_style_set_text_align(int64_t s, int32_t align);
int32_t lvglcj_style_set_text_letter_space(int64_t s, int32_t v);
int32_t lvglcj_style_set_text_line_space(int64_t s, int32_t v);
int32_t lvglcj_style_set_shadow_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_shadow_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_shadow_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_shadow_offset_x(int64_t s, int32_t v);
int32_t lvglcj_style_set_shadow_offset_y(int64_t s, int32_t v);
int32_t lvglcj_style_set_outline_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_outline_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_outline_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_outline_pad(int64_t s, int32_t v);
int32_t lvglcj_style_set_translate_x(int64_t s, int32_t v);
int32_t lvglcj_style_set_translate_y(int64_t s, int32_t v);
int32_t lvglcj_style_set_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_line_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_line_dash_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_line_dash_gap(int64_t s, int32_t v);
int32_t lvglcj_style_set_arc_width(int64_t s, int32_t v);
int32_t lvglcj_style_set_arc_color(int64_t s, uint32_t color);
int32_t lvglcj_style_set_arc_rounded(int64_t s, int32_t v);
int32_t lvglcj_style_set_image_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_image_recolor(int64_t s, uint32_t color);
int32_t lvglcj_style_set_image_recolor_opa(int64_t s, int32_t opa);
int32_t lvglcj_style_set_blend_mode(int64_t s, int32_t mode);
int32_t lvglcj_style_set_layout(int64_t s, int32_t layout);
int32_t lvglcj_style_set_base_dir(int64_t s, int32_t dir);
int32_t lvglcj_style_set_clip_corner(int64_t s, int32_t v);
int32_t lvglcj_style_set_rotary_sensitivity(int64_t s, int32_t v);

int32_t lvglcj_obj_add_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style_all(int64_t obj);
/* 读回属性：返回打包后的值（颜色 0xRRGGBB，数值直接返回） */
int64_t lvglcj_obj_get_style_prop(int64_t obj, int32_t prop, int32_t selector);

/* ============================================================ §5.8 Timer */
int64_t lvglcj_timer_create(int32_t cid, int32_t period_ms);
int32_t lvglcj_timer_delete(int64_t timer);
int32_t lvglcj_timer_pause(int64_t timer);
int32_t lvglcj_timer_resume(int64_t timer);
int32_t lvglcj_timer_set_period(int64_t timer, int32_t ms);
int32_t lvglcj_timer_ready(int64_t timer);
/*
 * 处理一次定时器。返回**错误码**（0 成功）。
 *
 * ★ 与早期版本的一处有据修正：LVGL 的 lv_timer_handler() 返回的是
 *   「距下一个定时器到期还有多少 ms」，那是数据不是错误码。
 *   把它直接当返回值会违反本头文件开头声明的统一约定
 *   （0 成功 / 正数仅 +1 为特殊标记 / 负数错误）——
 *   返回 30 会被当成特殊标记，返回 0 又会被当成「成功」，两种解读都错且静默。
 *   因此改为：本函数返回错误码，下次间隔用 lvglcj_timer_next_ms() 查询。
 */
int32_t lvglcj_timer_handler(void);
/* 最近一次 lvglcj_timer_handler() 报告的下次触发间隔（ms）；仅作观测/自适应休眠用 */
int32_t lvglcj_timer_next_ms(void);

/* ============================================================ §5.9 动画 */
/*
 * ★ var = anim_ctx（ADR-015）：不用 obj、也不用 closure_id。
 *   原因：同一 obj 上多个动画（同时改 width 与 height）的 var 会是同一个 obj，
 *   trampoline 无法区分是哪个动画在调用。
 * ★ lvglcj_anim_create 内部自动挂 internal deleted_cb，它是 anim_ctx 的**唯一释放点**
 *   （Patch P1 / ADR-017）：自然结束 / 手动删除 / 对象删除三条路径统一走它。
 *   因此 DELETE 钩子里**不得** free(ctx)，否则双重 free。
 * ★ 代价：var != obj 使 LVGL「对象删除自动停动画」失效，必须由 DELETE 钩子手动补。
 */
int64_t lvglcj_anim_create(void);
int32_t lvglcj_anim_set_target(int64_t a, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t a, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t a, int32_t cid);
/*
 * 插值曲线。取值是**桥接层自定义枚举**，与 LVGL 的内部顺序无关 ——
 * 它在这里被映射到 lv_anim_path_* 函数指针。
 * ★ 早先版本只写了 `int32_t path` 而未定义取值，
 *   那等于让调用方凭猜；现在把取值定死（违反即返回 INVALID_ARGUMENT）。
 */
typedef enum {
    LVGLCJ_ANIM_PATH_LINEAR = 0,     /* 匀速（默认） */
    LVGLCJ_ANIM_PATH_STEP = 1,       /* 阶跃：到结束时刻才跳到终值 */
    LVGLCJ_ANIM_PATH_EASE_IN = 2,    /* 慢起 */
    LVGLCJ_ANIM_PATH_EASE_OUT = 3,   /* 慢止 */
    LVGLCJ_ANIM_PATH_EASE_IN_OUT = 4,/* 慢起慢止 */
    LVGLCJ_ANIM_PATH_OVERSHOOT = 5,  /* 冲过头再回 */
    LVGLCJ_ANIM_PATH_BOUNCE = 6      /* 回弹 */
} lvglcj_anim_path_t;

int32_t lvglcj_anim_set_path(int64_t a, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t a, int32_t cnt);
/* 与 LVGL 同名语义：playback 指到达终点后**反向播回**的时长。
   注意 v9 已把 lv_anim_set_playback_time 标为 legacy，本层用 playback_duration。 */
int32_t lvglcj_anim_set_playback(int64_t a, int32_t ms);
/* 用户回调存在 ctx->user_deleted_cb，不直接挂到 LVGL（Patch P1） */
int32_t lvglcj_anim_set_deleted_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_set_start_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_start(int64_t a);
int32_t lvglcj_anim_delete(int64_t a);
/* 对外 API（用户用）；内部函数 stop_all_anims_of_obj 不对外暴露（次生残留处置 #2） */
int32_t lvglcj_obj_delete_anim(int64_t obj);
int32_t lvglcj_anim_count_running(void);
/* 泄漏对账（§11.3）：稳态下应与 lvglcj_anim_count_running() 一致 */
int32_t lvglcj_anim_ctx_count(void);

/* ============================================================ 控件（P0：仅三个） */
int64_t lvglcj_label_create(int64_t parent);
int32_t lvglcj_label_set_text(int64_t label, const char *text);
int32_t lvglcj_label_set_long_mode(int64_t label, int32_t mode);

int64_t lvglcj_button_create(int64_t parent);

/* ============================================ 控件（P1 批次 1：switch / checkbox / bar）
 *
 * 设计文档 §7.1 把控件分成 P0（三个）与 P1 两批。本段是 P1 的第一批，
 * 选它们的依据是**属性面最小且不需要新的资源类型**：
 *   · switch / checkbox 共用「勾选态」这一种属性
 *   · bar 用「值 + 范围」
 * 于是不必引入图像描述符（image）、点数组（line）、字符串列表（dropdown）
 * 这些新的值类型 —— 那些留到后续批次，因为它们各自带来新的所有权问题。
 *
 * ★ 三条本批控件共有的约定（三处必须一致，不要各自为政）：
 *
 * 1. **「取状态」返回 1/0，负数才是错误**（如 *_is_checked）。
 *    0 是合法答案（"没勾上"），不该为了避开 0 而把它编成错误码 ——
 *    那会逼调用方把正常情况写进异常分支。
 *    （与 lvglcj_font_has_glyph 同一取舍，两处表述保持一致。）
 *
 *    ★ 但 **bar_get_value 不用这个形式**：条形的值域本身可为负，
 *      "负数即错误"在那里不成立，所以它用出参 + 错误码返回。
 *      同一个判断在不同函数上可以得出相反结论，关键是看值域是否与错误码重叠。
 *
 * 2. **set_value 一律关动画**（LV_ANIM_OFF）。带动画时"设完立刻读值"读到的是
 *    中间态，而调用方（含测试）期望"设完即可读到新值"。确定性优先于观感；
 *    需要动画的场景由调用方自己驱动。
 *
 * 3. 创建函数与 obj_create / label_create 完全同构（创建 → 登记 → 挂 DELETE 钩子）。
 *    少挂钩子 → 父对象删除时该控件的句柄不被递归失效，
 *    isAlive() 仍为 true 而底层对象已消失，即 §3.3 要防的悬空句柄。
 */

/* switch：二态开关（勾选态即"开"） */
int64_t lvglcj_switch_create(int64_t parent);
int32_t lvglcj_switch_set_checked(int64_t sw, int32_t checked);
int32_t lvglcj_switch_is_checked(int64_t sw);

/* checkbox：带文本的二态控件 */
int64_t lvglcj_checkbox_create(int64_t parent);
int32_t lvglcj_checkbox_set_text(int64_t cb, const char *text);
int32_t lvglcj_checkbox_set_checked(int64_t cb, int32_t checked);
int32_t lvglcj_checkbox_is_checked(int64_t cb);

/* bar：进度/数值显示 */
int64_t lvglcj_bar_create(int64_t parent);
/* 范围：LVGL 要求 min < max，这里显式拦住相反的顺序 */
int32_t lvglcj_bar_set_range(int64_t bar, int32_t min, int32_t max);
int32_t lvglcj_bar_set_value(int64_t bar, int32_t value);
/* ★ 出参形式：值域可为负，"负数即错误"在这里不成立 */
int32_t lvglcj_bar_get_value(int64_t bar, int32_t *out_value);

/* ============================================ 控件（P1 批次 2：slider / arc / led / spinner）
 *
 * 选它们的依据与批次 1 相同：属性面小、不引入新的资源类型。
 *   · **slider 与 arc 与批次 1 的 bar 是同一种形状**（值 + 范围），
 *     因此三者共用同一段实现与同一条「取状态」约定 ——
 *     这是"形状相同就共用"的直接收益，不是巧合。
 *   · led 只有颜色与亮度。
 *   · spinner **没有属性**：它的旋转由 LVGL 内部动画驱动，不经过本层。
 *
 * ★ 沿用的约定（不重复展开，见批次 1 那段说明）：
 *     · 创建走同一套四步（创建 → 登记 → 挂 DELETE 钩子）
 *     · get_value 用**出参**（值域可为负，负数与错误码会撞车）
 *     · set_value 一律**关动画**（设完立刻读值即为新值）
 *     · 范围要求 min < max，顺序写反会被明确拒绝而不是变成显示异常
 */

/* slider：可拖动的取值控件（值域同 bar） */
int64_t lvglcj_slider_create(int64_t parent);
int32_t lvglcj_slider_set_range(int64_t slider, int32_t min, int32_t max);
int32_t lvglcj_slider_set_value(int64_t slider, int32_t value);
int32_t lvglcj_slider_get_value(int64_t slider, int32_t *out_value);

/* arc：环形取值控件（值域同 bar）。
 * 注意它与 slider 在 LVGL 侧的差异：lv_arc_set_value **没有** anim 参数，
 * 而 lv_slider_set_value 有 —— 我们的 ABI 两边都不暴露动画，因此对外一致。 */
int64_t lvglcj_arc_create(int64_t parent);
int32_t lvglcj_arc_set_range(int64_t arc, int32_t min, int32_t max);
int32_t lvglcj_arc_set_value(int64_t arc, int32_t value);
int32_t lvglcj_arc_get_value(int64_t arc, int32_t *out_value);

/* led：指示灯。颜色 0xRRGGBB（与 Canvas 绘制类一致，不含 alpha） */
int64_t lvglcj_led_create(int64_t parent);
int32_t lvglcj_led_set_color(int64_t led, uint32_t color);
/* 亮度 0..255（越界明确拒绝；LVGL 收的是 uint8_t，放行会静默截断） */
int32_t lvglcj_led_set_brightness(int64_t led, int32_t brightness);
/* 开/关（非 0 = 开） */
int32_t lvglcj_led_set_on(int64_t led, int32_t on);

/* spinner：忙碌指示。**无属性、无配置** —— 旋转由 LVGL 内部动画驱动 */
int64_t lvglcj_spinner_create(int64_t parent);

/* ============================================ 控件（P1 批次 3：dropdown）
 *
 * 这一批开始引入**新的值类型**（不再是"值/勾选"这种标量），dropdown 引入的是
 * **字符串列表** —— 也是三类新值类型里所有权最轻的一类，因为 LVGL 会**拷贝**它。
 *
 * ★★ 一个必须写下来的陷阱：LVGL 有一对语义相反的接口
 *      lv_dropdown_set_options()        —— **会拷贝**字符串（文档明说调用后可销毁传参）
 *      lv_dropdown_set_options_static() —— **不拷贝**，要求该内存一直有效
 *    我们只暴露**前者**。用后者配仓颉侧临时构造的 CString 会立刻变成悬空指针：
 *    调用返回即 free，而 LVGL 仍持有那个地址。这类错误不会当场报错，
 *    而是在某次重绘时读到已回收的内存 —— 所以这里连入口都不提供。
 *    （与 lv_label_set_text / lv_checkbox_set_text 同一取舍：只收"会被拷贝"的形态。）
 *
 * ★ 「取状态」的返回值形式在这里**又变了一次**，判据仍是同一条：
 *     dropdown 的索引与选项数都是**非负**的，不与错误码的负值域重叠，
 *     所以可以像 font_has_glyph 那样直接返回（0 是合法答案、负数是错误）。
 *     而 bar/slider/arc 的 get_value 必须用出参 —— 因为它们的值域可为负。
 *     同一个"取状态"问题在不同控件上得出不同结论，规则是同一条：
 *     **值域是否与错误码重叠**。
 */
int64_t lvglcj_dropdown_create(int64_t parent);

/* options 是 **'\n' 分隔** 的字符串（LVGL 的原生形态），例如 "一\n二\n三"。
 * LVGL 会拷贝一份，因此传入的 CString 调用返回后即可释放。 */
int32_t lvglcj_dropdown_set_options(int64_t dd, const char *options);

/* 选中项索引（0 起）。负数明确拒绝 —— 底层收 uint32_t，放行会被隐式转成大整数 */
int32_t lvglcj_dropdown_set_selected(int64_t dd, int32_t idx);

/* 当前选中项索引；**负数是错误码**（索引本身非负，不会撞车，见上面的说明） */
int32_t lvglcj_dropdown_get_selected(int64_t dd);

/* 选项个数；同样负数是错误码 */
int32_t lvglcj_dropdown_get_option_count(int64_t dd);

/* ============================================ 控件（P1 批次 4：line）
 *
 * ★★ line 与 dropdown 的本质区别：**LVGL 不拷贝点数组**。
 *   它的文档明写 "Only the address is saved, so the array needs to be alive
 *   while the line exists"。也就是说这是一类**新的所有权问题**：
 *   LVGL 持有我们给的地址，生命周期由**我们**负责。
 *
 *   这与 Canvas 的像素缓冲属于同一类（那里也是"只给尺寸、缓冲由我们持有"），
 *   因此**复用同一套模式**而不是另创一个：
 *     指针存在对象自身的 user_data 里（无容量问题）→ DELETE 时释放 →
 *     重设时先释放旧的。
 *
 * ★ 为什么 ABI 收**扁平的 int32 坐标对**（x0,y0,x1,y1,...）而不是点结构体数组：
 *   LVGL 的 lv_point_precise_t 成员类型取决于其配置（可能是 float），
 *   把它的内存布局摆到 ABI 上，会让契约随上游配置漂移 —— 那是我们不想要的耦合。
 *   int32 坐标对是稳定的，转换在 C 侧做一次。
 */
int64_t lvglcj_line_create(int64_t parent);

/* 设置点集。
 *   xy          : x0,y0,x1,y1,... 共 point_count 对（**不是**字节数）
 *   point_count : 点的个数，必须 ≥ 1
 * 点坐标会被**复制**到 C 侧持有的缓冲里，因此调用方的数组可以随即释放。 */
int32_t lvglcj_line_set_points(int64_t line, const int32_t *xy, int32_t point_count);

/* y 轴反向（非 0 = 反向）。LVGL 的默认坐标系向下增长，画折线图时常需要翻转 */
int32_t lvglcj_line_set_y_invert(int64_t line, int32_t on);

/* ============================================ 控件（P1 批次 5：image）
 *
 * ★ 所有权：**LVGL 会拷贝路径字符串**。这是读实现确认的，不是推测：
 *     lv_image_set_src 对 FILE/SYMBOL 类型执行 lv_strdup(src)，
 *     并把旧的源用 lv_free 释放（即所有权归它）。
 *   因此本 ABI 收的 const char* 在调用返回后即可释放 —— 与 dropdown/label 同类，
 *   而不是 canvas/line 那种"LVGL 只存指针、由我们持有"的类型。
 *
 * ★★ 契约里**不提供**图像描述符（lv_image_dsc_t*）这一形态。原因：
 *   LVGL 对 LV_IMAGE_SRC_VARIABLE 是 `img->src = src` —— **只存指针、不拷贝**，
 *   那会变成又一类"由我们持有"的所有权问题（第三例），且描述符的内存布局还要
 *   一并冻结进契约。本批只做文件路径形态，把描述符留到有明确需求时单独设计。
 *   （与 dropdown 不提供 set_options_static 是同一种取舍：不给会制造悬空的入口。）
 *
 * ★ 已知的静默失败（如实写在契约里）：**路径不存在时 LVGL 不报错**，
 *   只是不显示图像（它自己打一行日志）。本层无法在不额外付出代价的情况下
 *   提前发现（POSIX 驱动没有 exists 接口），所以调用方若要判定"到底加载上没有"，
 *   需自行确认文件存在。不把这件事写清楚，它会表现为"设了 src 但界面空白"。
 */
int64_t lvglcj_image_create(int64_t parent);

/* 设置图像源（文件路径）。
 * path == NULL 表示**清空图像** —— 这是合法操作而非错误：
 * 图像没有"空字符串"这种表达方式，NULL 是表达"没有图像"的唯一途径。
 * （注意这与 label/checkbox/dropdown 的文本 setter 相反，那里 NULL 是错误。） */
int32_t lvglcj_image_set_src(int64_t img, const char *path);

/* 偏移（像素） */
int32_t lvglcj_image_set_offset(int64_t img, int32_t x, int32_t y);

/* 缩放。★ 单位不是百分比：**256 = 100%**，128 = 一半，512 = 两倍。
 * 传 100 会得到约 39% —— 这是 LVGL 的既有约定，此处如实透传并显式校验 0。 */
int32_t lvglcj_image_set_scale(int64_t img, int32_t zoom);

/* ============================================ 控件（P1 批次 6：roller）
 *
 * ★ 所有权：**两种模式都会拷贝选项串**（读实现确认，与直觉相反）：
 *     · NORMAL   模式走 lv_label_set_text，label 是拷贝语义；
 *     · INFINITE 模式自己按 inf_page_cnt 复制出重复串（lv_malloc）。
 *   所以"无限模式大概要调用方保活这份字符串"这个直觉是错的 —— 传临时串安全。
 *
 * ★ 与 image 相反：**options == NULL 不是"清空"而是错误**。
 *   实现里有 LV_ASSERT_NULL(options)，放过去会在 LVGL 内部直接断言。
 *   （清空 roller 的选项没有语义；要"空"就传一个不含 '\n' 的串。）
 */
#define LVGLCJ_ROLLER_MODE_NORMAL   0
#define LVGLCJ_ROLLER_MODE_INFINITE 1

int64_t lvglcj_roller_create(int64_t parent);

/* 设置选项。options 为 '\n' 分隔（与 dropdown 同一约定）。
 * mode 取 LVGLCJ_ROLLER_MODE_*；越界值被拒。 */
int32_t lvglcj_roller_set_options(int64_t roller, const char *options, int32_t mode);

/* 选中第 sel 项（0 基）。
 * ★ 本层**必须**校验范围：底层 lv_roller_set_selected 收 uint32_t 且不自行钳制，
 *   越界不会断言，而是把内部 id 设成越界值，随后渲染/取串会读到界外。
 *   故这里按选项数校验，负值与非 int32 转换同样拦下（负数转 uint32 会变成巨大索引）。 */
int32_t lvglcj_roller_set_selected(int64_t roller, int32_t sel);

/* 返回当前选中索引；失败返回负的错误码 */
int32_t lvglcj_roller_get_selected(int64_t roller);

/* 可见行数。0 会得到一个零高度的控件，没有语义，拦下 */
int32_t lvglcj_roller_set_visible_row_count(int64_t roller, int32_t rows);

/* ============================================ 控件（P1 批次 7：textarea）
 *
 * ★ 所有权：**文本会被拷贝**（读实现确认）：set_text 走 lv_label_set_text；
 *   密码模式下先 lv_strdup 到内部 pwd_tmp 再交给 label。两条路径都不保留调用方的指针。
 *
 * ★ get_text 是本 ABI 里第一个"把字符串取回来"的入口，形态是刻意选的：
 *   写入调用方缓冲 + 返回长度，**而不是返回 const char***。
 *   返回内部指针会把"这份指针何时失效"变成调用方的负担（text 随时会被 set_text 改掉），
 *   而 const char* 的返回类型又没法用负错误码表达失败。
 *   写入缓冲把两件事都变成显式的：要多大（先传 NULL 探长度）、失败没有（负错误码）。
 */
int64_t lvglcj_textarea_create(int64_t parent);

/* 设置文本。'\n' 表示多行（单行模式下会被折成空格）。文本会被拷贝。 */
int32_t lvglcj_textarea_set_text(int64_t ta, const char *txt);

/* 取文本：写进调用方缓冲（尾部补 NUL），返回**文本字节数**（不含 NUL，多字节字符按字节计）。
 *   buf == NULL → 只回报所需长度，不写入（先探测、再取用的两步用法）。
 *   buf != NULL 但 size <= 所需长度 → **不写入任何内容**并返回 LVGLCJ_ERR_INVALID_ARGUMENT。
 *     （宁可报错也不截断：截断会得到一个"看起来对"的短字符串，那是比报错更坏的失败。）
 *   句柄失效等返回负错误码 —— 不会返回 0 冒充"空文本"。 */
int32_t lvglcj_textarea_get_text(int64_t ta, char *buf, int32_t size);

/* 占位文本（文本为空时显示）。会被拷贝。 */
int32_t lvglcj_textarea_set_placeholder_text(int64_t ta, const char *txt);

/* 单行模式（回车不换行、且不显示换行） */
int32_t lvglcj_textarea_set_one_line(int64_t ta, int32_t on);

/* 密码模式：显示为圆点，内部把真实文本另存一份 */
int32_t lvglcj_textarea_set_password_mode(int64_t ta, int32_t on);

/* 最大字符数。★ 0 表示不限制，所以 0 合法、负数被拒。
 * 另注：LVGL 的 lv_textarea_set_text 会**绕过**这个上限（其文档明确写了）。 */
int32_t lvglcj_textarea_set_max_length(int64_t ta, int32_t len);

/* ============================================ 控件（P1 批次 8：table）
 *
 * ★ 所有权：**单元格文本会被拷贝**。这一点曾被我按 v8 的旧印象判为"只存指针"，
 *   实际 v9 的文档与实现都表明是拷贝：
 *     文档 —— "It will be copied and saved so this variable is not required after..."
 *     实现 —— lv_realloc(旧块, 所需长度) 后 copy_cell_txt() 写入。
 *   所以调用方的字符串在返回后即可释放，本层不需要替 LVGL 持有任何东西。
 *   （记录这次更正，是因为"table 的单元格不拷贝"是流传很广的旧说法，
 *     若照它设计，会凭空造出一套多余的持有机制。）
 *
 * ★★ 也有一个真的坑：set_cell_value 对**超出当前规模**的行列是**自动扩容**的
 *   （LVGL 文档明确写了）。配合"负值转 uint32_t 会变成约 42 亿"，
 *   后果不是越界读，而是直接申请一张 42 亿列的表。故本层必须拦负值。
 *
 * ★ 与 image 相反、与 roller 一致：txt == NULL 是**错误**（实现里有 LV_ASSERT_NULL）。
 */
int64_t lvglcj_table_create(int64_t parent);

/* 设置单元格文本（会被拷贝）。行/列超出当前规模时自动扩容 —— 这是 LVGL 的既定行为。 */
int32_t lvglcj_table_set_cell_value(int64_t tbl, int32_t row, int32_t col, const char *txt);

/* 取单元格文本：与 textarea 的 get_text 同一形态（写缓冲 + 返回字节数；传 NULL 只探长度）。
 * ★ 取用**不做**自动扩容：索引超出当前行列范围直接返回 LVGLCJ_ERR_INVALID_ARGUMENT，
 *   不会先替你造出一张空表（那会让"读错一格"变成静默改变对象结构）。 */
int32_t lvglcj_table_get_cell_value(int64_t tbl, int32_t row, int32_t col, char *buf, int32_t size);

/* 行数 / 列数。0 合法（相当于清空）；负数被拒（同上，会转成巨大值）。 */
int32_t lvglcj_table_set_row_count(int64_t tbl, int32_t rows);
int32_t lvglcj_table_set_column_count(int64_t tbl, int32_t cols);
int32_t lvglcj_table_get_row_count(int64_t tbl);
int32_t lvglcj_table_get_column_count(int64_t tbl);

/* 单元格控制位。★ v9 的枚举只有下面这些 —— v8 里那对 TEXT_CENTER/TEXT_RIGHT
 * 已经消失（v9 用样式做对齐），照 v8 的用法写会静默失效。 */
#define LVGLCJ_TABLE_CTRL_MERGE_RIGHT 1 /* 与右边一格合并 */
#define LVGLCJ_TABLE_CTRL_TEXT_CROP   2 /* 文本超出时裁剪而非换行 */
/* 注：LVGL 另有 CUSTOM_1..4，是留给应用自己的标记位、LVGL 不做任何处理，
 * 故不进契约（放进来只会让人以为它们有显示效果）。 */
int32_t lvglcj_table_add_cell_ctrl(int64_t tbl, int32_t row, int32_t col, int32_t ctrl);
int32_t lvglcj_table_clear_cell_ctrl(int64_t tbl, int32_t row, int32_t col, int32_t ctrl);

/* ============================================ 控件（P1 批次 9：keyboard）
 *
 * ★★ 绑定 textarea 的生命周期危险（读实现确认，不是推测）：
 *      · lv_keyboard.c 里**没有**任何 LV_EVENT_DELETE 处理；
 *      · 全文件唯一给 keyboard->ta 赋值的地方就是 lv_keyboard_set_textarea；
 *      · lv_textarea.c **完全不引用** keyboard（textarea 不知道谁绑了它）；
 *      · 而按键处理直接解引用 keyboard->ta。
 *    结论：**删除 textarea 之前必须先解绑**，否则之后任何一次按键都会踩悬空指针
 *    （崩溃点在 LVGL 的事件回调里，不在我们的包装函数里，因此本层无法"顺手拦一下"）。
 *    本层不替调用方决定生命周期，故把它写成契约，并提供解绑入口（ta 传 0）。
 *
 * ★ 本批**不提供** lv_keyboard_set_map：自定义布局需要调用方长期持有
 *   map/ctrl_map 两个数组（LVGL 只存指针），那要求一套"由我们持有"的机制，
 *   与 canvas/line 同属一类问题，值得单独一轮做，而不是在这里顺手加半个。
 *
 * ★ 模式取值就是 LVGL 枚举的顺序值 0..7。注意 TEXT_ARABIC 只在编译期打开
 *   LV_USE_ARABIC_PERSIAN_CHARS 时才存在 —— 本项目的构建没开，故不提供该取值。
 */
#define LVGLCJ_KEYBOARD_MODE_TEXT_LOWER 0
#define LVGLCJ_KEYBOARD_MODE_TEXT_UPPER 1
#define LVGLCJ_KEYBOARD_MODE_SPECIAL    2
#define LVGLCJ_KEYBOARD_MODE_NUMBER     3
#define LVGLCJ_KEYBOARD_MODE_USER_1     4
#define LVGLCJ_KEYBOARD_MODE_USER_2     5
#define LVGLCJ_KEYBOARD_MODE_USER_3     6
#define LVGLCJ_KEYBOARD_MODE_USER_4     7
#define LVGLCJ_KEYBOARD_MODE_MAX        7

int64_t lvglcj_keyboard_create(int64_t parent);

/* 绑定 textarea。ta == 0 表示**解绑**（合法操作，也是删除 textarea 前必须做的一步）。
 * 注意 LVGL 会断言 ta 确实是 textarea 类 —— 传入别的对象会在断言打开时直接中止，
 * 所以 L1 层用类型（setTextarea(LvTextarea)）来保证这一点，而不是靠调用方自觉。 */
int32_t lvglcj_keyboard_set_textarea(int64_t kb, int64_t ta);

/* 取绑定的 textarea 句柄；未绑定返回 LVGLCJ_HANDLE_NULL。
 * 失败（键盘句柄失效等）返回负错误码 —— 与"未绑定"用不同的值表达。
 * ★ 返回的是**句柄**而不是裸指针：即便被绑的 textarea 已被删除，
 *   反查也会给出它那个已失效的句柄，调用方拿到的是一个会正常报错的东西。 */
int64_t lvglcj_keyboard_get_textarea(int64_t kb);

/* 模式。越界值必须拦下：LVGL 会拿它去索引布局表，越界就是读到界外。 */
int32_t lvglcj_keyboard_set_mode(int64_t kb, int32_t mode);
int32_t lvglcj_keyboard_get_mode(int64_t kb);

/* 按键弹出预览 */
int32_t lvglcj_keyboard_set_popovers(int64_t kb, int32_t on);

/* ============================================ 控件（P1 批次 10：chart）
 *
 * ★★ 本批需要一层**我们自己的 series 索引**，原因两条都是读出来的：
 *   ① lv_chart_series_t 不是 lv_obj_t，进不了对象句柄表 —— 那张表的状态机建立在
 *      "LVGL 对象"这个前提上（对象被删时 LVGL 派发事件、我们据此失效句柄）；
 *      series 不是对象，没有这个机制。
 *   ② lv_chart.h **不公开结构体**，拿不到 chart->series[i]，
 *      所以也没法"按索引回头去问 LVGL"。
 *   于是对外只暴露**索引**（int32），指针留在我方表里。
 *
 * ★ 索引的失效规则（每条都对应一个查证过的事实）：
 *   · 图表被删除（含被父对象**级联**删掉）→ 索引不可用。
 *     判据是每次使用前查图表句柄是否仍 ALIVE，而不是"我们记得删过" ——
 *     级联删除不经过本层包装函数，只有句柄表知道对象已经没了。
 *   · remove_series → 该索引永久失效。
 *   · 索引**不回收**：移除后新加的 series 拿新索引，不复用旧号。
 *     否则"旧索引指向新序列"会安静地给出错误数据 —— 比报错难查得多。
 *     （代价是单个图表累计 add 次数有上限；索引表按需增长，无固定常量。）
 *   ★ 槽位可安全回收：句柄 id 单调递增、永不复用（handle_table.c 的 ADR-001），
 *     故"回收一个已死图表的槽位"不会被将来某个新图表命中。
 */
#define LVGLCJ_CHART_TYPE_NONE    0
#define LVGLCJ_CHART_TYPE_LINE    1
#define LVGLCJ_CHART_TYPE_BAR     2
#define LVGLCJ_CHART_TYPE_SCATTER 3

/* 坐标轴。★ 注意这是**位标志**取值：X 轴是 0x02/0x04，不是 2/3 那样连号。 */
#define LVGLCJ_CHART_AXIS_PRIMARY_Y   0
#define LVGLCJ_CHART_AXIS_SECONDARY_Y 1
#define LVGLCJ_CHART_AXIS_PRIMARY_X   2
#define LVGLCJ_CHART_AXIS_SECONDARY_X 4

int64_t lvglcj_chart_create(int64_t parent);

/* 图表类型（LVGLCJ_CHART_TYPE_*） */
int32_t lvglcj_chart_set_type(int64_t chart, int32_t type);

/* 数据点数。0 与负数被拒：点数决定内部数组尺寸，0 会得到一个没有数据点的图表。 */
int32_t lvglcj_chart_set_point_count(int64_t chart, int32_t cnt);
int32_t lvglcj_chart_get_point_count(int64_t chart);

/* 坐标轴范围（axis 取 LVGLCJ_CHART_AXIS_*） */
int32_t lvglcj_chart_set_range(int64_t chart, int32_t axis, int32_t min, int32_t max);

/* 添加一条曲线，返回**索引**（>= 0）或负错误码。
 * ★ axis 只接受 PRIMARY_Y / SECONDARY_Y：series 只能挂 Y 轴（LVGL 文档如此），
 *   X 轴那两个取值只用于范围设置。 */
int32_t lvglcj_chart_add_series(int64_t chart, uint32_t color, int32_t axis);

/* 移除一条曲线。该索引**永久**失效（不回收，见上）。 */
int32_t lvglcj_chart_remove_series(int64_t chart, int32_t idx);

/* 追加一个点（按序推进） */
int32_t lvglcj_chart_set_next_value(int64_t chart, int32_t idx, int32_t value);

/* 按下标写一个点。
 * ★ point_id 必须由本层校验：底层收 uint32_t 且**不检查范围**，
 *   越界就是一次界外写（写坏的是 series 的 y_points 数组）——
 *   它不报错，只会安静地破坏内存，属于最坏的一类输入。 */
int32_t lvglcj_chart_set_value_by_id(int64_t chart, int32_t idx, int32_t point_id, int32_t value);

/* ============================================================ §3.11.2 Canvas */
int64_t lvglcj_canvas_create(int64_t parent);
int32_t lvglcj_canvas_set_buffer(int64_t canvas, int32_t w, int32_t h);
int32_t lvglcj_canvas_set_palette(int64_t canvas, int32_t idx, uint32_t color);
int32_t lvglcj_canvas_fill_bg(int64_t canvas, uint32_t color, int32_t opa);
int32_t lvglcj_canvas_draw_point(int64_t canvas, int32_t x, int32_t y, uint32_t color);
int32_t lvglcj_canvas_draw_line(int64_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                uint32_t color);
int32_t lvglcj_canvas_draw_rect(int64_t canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                                uint32_t color);
int32_t lvglcj_canvas_draw_arc(int64_t canvas, int32_t cx, int32_t cy, int32_t r,
                               int32_t start_angle, int32_t end_angle, uint32_t color);

/* ============================================================ §5.10 字体 / 文件系统 */
/*
 * 内置字体选择。
 *
 * ★ 为什么需要它：LVGL 内置的 Montserrat 系列**只有拉丁字形**，
 *   界面文字含中文时 LVGL 会对每个汉字报 "glyph dsc. not found" 且画面上什么都不显示
 *   （表现为「按钮是空的」）。因此显示中文必须显式使用 CJK 字体。
 * ★ CJK_16 对应 LV_FONT_SIMSUN_16_CJK，需在 lv_conf 中开启
 *   （scripts/gen_lv_conf.sh 已开）；未开启时本函数明确报错而非回退。
 *
 *   ★★ 它不是「常用汉字表」★★
 *   官方描述为 "1000 of the most common CJK radicals"（lv_conf 模板原文），
 *   但实测（native/probe/probe_font_coverage.c）表明它更像一份
 *   手工拼合的混合清单：约 1118 个 CJK 统一表意文字 + 173 个日文假名，
 *   简体常用字、繁体字、日文异体混在一起，连部首都只覆盖 9/28。
 *   实测缺失：问 厅 灯 调 窗 帘 显 ……；实测在册：問 調 窓 ……
 *
 *   因此**不要**把「启用 CJK 字体」当成「中文可显示」的开关。
 *   要显示某段文本前，用 lvglcj_font_has_glyph() 逐字核对 ——
 *   缺字时 LVGL 只打一行 glyph not found 日志，画面上直接少一块**且不报错**。
 *
 * ★ 内置字体是静态对象，**不可释放** —— lvglcj_font_delete 会拒绝它。
 */
typedef enum {
    LVGLCJ_FONT_DEFAULT = 0, /* lv_conf 的 LV_FONT_DEFAULT */
    LVGLCJ_FONT_CJK_16 = 1   /* LV_FONT_SIMSUN_16_CJK */
} lvglcj_builtin_font_t;

/* 取内置字体句柄；惰性登记，重复调用返回同一句柄 */
int64_t lvglcj_font_builtin(int32_t which);

/*
 * ★ 查询某个码位在该字体里**有没有字形**。
 *
 * 【返回值是三态，不是错误码】
 *     1  = 有字形
 *     0  = 没有字形（该字符渲染不出来）—— **这是合法答案，不是失败**
 *     负数 = 错误码（未初始化 / 句柄无效 / 非 LVGL 线程 / 码位非法）
 *
 * ★ 因此**不能**用 `rc != LVGLCJ_OK` 判断成功 —— 0 既是 LVGLCJ_OK，
 *   也是「没有字形」。这是本函数唯一容易用错的地方，务必按 1/0/负数 三态处理。
 *   （之所以让「无字形」占 0：它是常见且正常的结果，不该为了避开 0
 *     而把它编成一个错误码 —— 那会让调用方不得不把正常情况放进异常分支。）
 *
 * 【为什么必须有它】
 *   内置 CJK 字库不是「中文可用」的开关（见上面说明），它**有缺字**。
 *   而缺字时 LVGL 只在日志打一行 glyph not found，画面上直接少一块 ——
 *   不报错、不抛异常、不影响其他字符。于是「一段文案里少了一个字」
 *   在「输入 → 显示」的整条链路上都是静默的，可能到验收甚至上线后才发现。
 *   有了本函数，调用方与 scripts/cjk_audit.py 这类工具就能把
 *   「哪些字显示不出来」变成一次可编程的查询，而不是靠人眼看截图。
 *
 * 【约束】与 §5.10 其余函数一致，必须在 **LVGL 线程**调用：
 *   字形查询与绘制共用 LVGL 的字体/字形缓存，不是纯只读操作。
 *
 * @param codepoint  Unicode 码位（如 0x95EE = 「问」）；负数报 INVALID_ARGUMENT。
 */
int32_t lvglcj_font_has_glyph(int64_t font, int32_t codepoint);

int64_t lvglcj_font_load(const char *path);

/* 从 TTF/OTF 直接加载（需要 LV_USE_TINY_TTF + LV_TINY_TTF_FILE_SUPPORT，见 gen_lv_conf.sh）。
 *
 * ★ 与 lvglcj_font_load 的区别：后者读的是 LVGL **转换后**的 .bin 字体，前者读原始 TTF。
 *   要显示任意中文，用 .bin 就得先把整字体转换（体积大，通常要子集化）；读 TTF 则直接可用。
 *   代价是运行时要解析字体（tiny_ttf），换来的是开发期不必为每个字号/字集生成一个文件。
 *
 * ★ path 走 LVGL 的文件系统（FS 盘符 'A'，根目录见 LV_FS_POSIX_PATH），形如 "A:/xxx.ttf"。
 *   用不了系统字体的绝对路径 —— 除非把它表示成相对于 FS 根的路径。 */
int64_t lvglcj_font_load_ttf(const char *path, int32_t font_size);
int32_t lvglcj_font_delete(int64_t font);
int32_t lvglcj_fs_init_posix(const char *root);

/* ============================================================ §5.11 调试 / 可观测性 */
typedef struct {
    int64_t *handles;
    int32_t *depths;
    char   **names;
    int32_t  count;
} lvglcj_tree_dump_t;

int32_t lvglcj_debug_dump_tree(int64_t root, int32_t max_depth, lvglcj_tree_dump_t *out);
void    lvglcj_debug_free_tree_dump(lvglcj_tree_dump_t *dump);
int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag, uint32_t *max_used,
                           uint32_t *free_size);

typedef struct {
    uint32_t fps;
    uint32_t cpu_percent;
    uint32_t refr_time_ms;
    uint32_t draw_time_ms;
    uint32_t obj_count;
} lvglcj_perf_t;

int32_t lvglcj_perf_sample(lvglcj_perf_t *out);
int32_t lvglcj_obj_count(void);

/* ------------------------------------------------- FFI 契约测试支持（§10.2）
 * 返回 C 侧 lv_area_t 的成员偏移，供仓颉 @C struct 布局断言比对 */
typedef struct {
    int32_t x1_offset;
    int32_t y1_offset;
    int32_t x2_offset;
    int32_t y2_offset;
    int32_t sizeof_area;
    /* 结构体尺寸与对齐（与仓颉 sizeOf/alignOf 比对） */
    int32_t sizeof_indev_data;
    int32_t sizeof_perf;
    int32_t sizeof_error_ctx;
    int32_t sizeof_tree_dump;
} lvglcj_offsets_t;

int32_t lvglcj_probe_offsets(lvglcj_offsets_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_BRIDGE_H */
