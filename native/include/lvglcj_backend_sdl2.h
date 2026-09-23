/*
 * lvglcj_backend_sdl2.h —— SDL2 桌面后端（设计文档 §8）
 *
 * ==================== 与设计文档 §8.1.6 的关系 ====================
 * 本后端**手写 flush/read**，不使用 LVGL 内置的 SDL 驱动。
 * 原因（§8.1.6 原话）：内置驱动会绕过我们的 trampoline，
 * 于是「最需要验证的回调链路」恰好被跳过 —— 测试通过也说明不了问题。
 *
 * ==================== 线程模型（§8.1.2） ====================
 *   · SDL 事件循环必须跑在**创建窗口的线程**（通常主线程）
 *   · LVGL 主循环在另一条线程（方案 A），两者用「自定义事件 + 信号量」握手
 *
 * ★ 但本实现额外支持**单线程（方案 C / 测试）**，见下面的说明 —— 这是
 *   V6 探针没有覆盖、而实际必然遇到的一种情况。
 *
 * ==================== 为什么必须有「单线程分流」 ====================
 * §8.1.2 的流程是「flush 里投递渲染请求 → 主线程渲染 → sem_post → flush_wait 等待」。
 * 这在双线程下没问题；但如果**渲染线程就是 LVGL 线程**（方案 C，以及绝大多数
 * 自动化测试），flush 投递完请求后等待，而唯一能处理该请求的线程正卡在等待里 ——
 * 于是必然等到超时，每帧白等 100ms，帧率塌成 10 FPS 且日志刷满 BACKEND_FAILURE。
 *
 * 因此 flush 回调会先判断「当前线程是不是渲染线程」：
 *   是 → 直接内联渲染（既不等事件也不等信号量）
 *   否 → 走投递 + 有界等待的跨线程路径
 * 两条路径共用同一个渲染函数，只有「谁来调用」不同。
 *
 * ==================== 输入 ====================
 * 主线程采集 SDL 输入到原子变量；LVGL 线程的 indev 读回调通过
 * lvglcj_sdl2_feed_indev() 把它写进当前读取上下文（T5 机制）。
 * 仓颉侧的读回调因此只需一行：{ _ => lvglcj_sdl2_feed_indev() }。
 */
#ifndef LVGLCJ_BACKEND_SDL2_H
#define LVGLCJ_BACKEND_SDL2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化：建窗口/渲染器/纹理，并创建 display 且挂上 flush sink。
 * **必须由将要跑 SDL 事件循环的那个线程调用**（它会被登记为渲染线程，
 * 既用于上面的单线程分流，也用于 §3.8.4 的等待图判据 3）。
 *
 * 无显示环境（纯 headless CI）可设 SDL_VIDEODRIVER=dummy 使用软件渲染器。
 */
int32_t lvglcj_sdl2_init(int32_t w, int32_t h, int32_t color_format, int32_t buf_lines);

int32_t lvglcj_sdl2_deinit(void);

/* headless display 句柄；未初始化返回 0 */
int64_t lvglcj_sdl2_display(void);

/*
 * 主线程调用：处理一轮 SDL 事件（含渲染请求、输入、窗口事件）。
 * 返回：1 = 收到退出请求（窗口关闭 / SDL_QUIT）；0 = 继续；负数 = 错误。
 *
 * 调用方应在循环里持续调用它 —— 它就是「SDL 事件循环」本身。
 */
int32_t lvglcj_sdl2_poll_events(void);

/*
 * 把主线程采集的输入写进当前 indev 读取上下文。
 * 只能在 indev_read 回调内调用（否则返回 NOT_SUPPORTED，与 T5 的约束一致）。
 */
int32_t lvglcj_sdl2_feed_indev(void);

/* ---------------------------------------------------------- 观测（测试/断言用） */
int32_t lvglcj_sdl2_flush_count(void);        /* flush sink 被调用次数 */
int32_t lvglcj_sdl2_present_count(void);      /* 真正 Present 的次数 */
int32_t lvglcj_sdl2_wait_timeout_count(void); /* 有界等待超时次数（正常应恒为 0） */

/* 渲染事件与暂存数据**序号不匹配**的次数。
 * 判据：长期为 0 = 每次 flush 都等到的是自己那次渲染；
 * 增长 = 有过提前返回，那次覆盖的条带不会重画（画面残留旧像素）。 */
int32_t lvglcj_sdl2_stale_apply_count(void);
int32_t lvglcj_sdl2_is_paused(void);          /* 窗口关闭/最小化后为 1 */
int32_t lvglcj_sdl2_is_render_thread(void);   /* 当前线程是否为渲染线程 */

/*
 * 颜色格式 → SDL 像素格式映射查询。
 * 返回 0 表示不支持（调用方应据此拒绝，而不是猜一个格式 —— 猜错就是花屏）。
 * 暴露给测试是为了把「映射表」本身变成可断言的对象。
 */
uint32_t lvglcj_sdl2_pixel_format_for(int32_t lv_color_format);

/* ---------------------------------------------------------- 测试注入 */
/*
 * 让渲染请求被忽略，用于验证 flush 的**超时路径**（§8.1.2 强制要求 2）。
 * 生产代码不会调用它；存在的意义是「超时分支不能只靠读代码相信」。
 */
int32_t lvglcj_sdl2_set_render_suppressed(int32_t on);
/* 模拟窗口关闭（不经真实 SDL 事件），用于验证即时解锁 */
int32_t lvglcj_sdl2_inject_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_BACKEND_SDL2_H */
