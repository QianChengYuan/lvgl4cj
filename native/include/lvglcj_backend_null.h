/*
 * lvglcj_backend_null.h —— headless 后端（CI / 单测 / 无窗口环境）
 *
 * ==================== 为什么它是**必需项**而不是可选优化 ====================
 * 由 P0 探针结论 G1 实测得出：
 *   无 display 时 `lv_screen_active()` 返回 NULL，而 `lv_obj_create(NULL)`
 *   会触发 LVGL 内部断言 `Asserted ... obj != NULL` 并**直接终止进程**。
 *
 * 也就是说：任何想创建对象的测试/示例，都必须**先有一个 display**。
 * 在没有窗口的 CI 里，这个 display 只能来自 headless 后端 —— 因此本文件不是
 * 「省事的替代品」，而是 t5（SDL2）与 t6（hello_cj 断言）能跑起来的前置。
 *
 * ==================== 实现上的一个关键选择 ====================
 * 本后端**不注册 flush 回调**（不占用 closure_id）。
 *   这不是偷懒：display.c 的 T4 trampoline 在「未注册 cid」时会**自行调用
 *   lv_display_flush_ready()**，因此 headless 下刷新天然不会卡住。
 *   而需要观测刷新时，仓颉侧照常用 lvglcj_display_set_flush_cb(disp, cid)
 *   注册自己的回调 —— 于是 headless 测试**同样覆盖 T4 完整链路**，
 *   不是「因为没窗口所以跳过了回调」。
 *
 * ==================== 线程模型 ====================
 * 本后端**不启动** LVGL 主循环线程，刻意如此：
 *   headless 场景（单测）需要**确定性推进**，由调用方用 lvglcj_null_pump(n)
 *   一帧一帧地驱动，避免「后台线程在断言之间偷跑几帧」造成的不稳定。
 *   这对应方案 C（泵模式），而方案 A 的线程由 LvglRuntime.start() 显式启动。
 */
#ifndef LVGLCJ_BACKEND_NULL_H
#define LVGLCJ_BACKEND_NULL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 headless 后端：创建一个 display（绘制缓冲由桥接层在 C 侧分配）。
 *
 * color_format 取 lv_color_format_t 数值；buf_lines 传 0 表示按 PARTIAL 的
 * 1/10 屏下限自动选取（与 lvglcj_display_create 的语义一致）。
 * 返回 LVGLCJ_OK 或错误码。
 */
int32_t lvglcj_null_init(int32_t w, int32_t h, int32_t color_format, int32_t buf_lines);

/* 销毁 display；幂等 */
int32_t lvglcj_null_deinit(void);

/* 取 headless display 的句柄；未初始化返回 0 */
int64_t lvglcj_null_display(void);

/*
 * 确定性推进 n 帧：每帧 = 延迟删除 drain → 任务队列 drain → lv_timer_handler，
 * 与方案 A 主循环的帧内顺序完全一致（见 thread.c 的说明），
 * 因此「headless 下通过」与「方案 A 线程下通过」在时序上具有可比性。
 *
 * 每帧后 sleep 由桥接层统一控制的帧间隔，让 LVGL 的刷新周期能真正到期。
 */
int32_t lvglcj_null_pump(int32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_BACKEND_NULL_H */
