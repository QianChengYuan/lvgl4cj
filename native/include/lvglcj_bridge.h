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
