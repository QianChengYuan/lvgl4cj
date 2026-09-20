/*
 * lvglcj_probe.h —— V1–V7 探针专用接口（设计文档 §12 P0 第 1 周）
 *
 * 探针不是公开 ABI。本头文件只被 native/probe/*.c 与 probe/ 下的仓颉探针工程使用，
 * 不进入 liblvgl4cj_bridge。
 *
 * 本项目的探针分两类：
 *   · 纯 C 探针（probe_conf / probe_v4_asan / probe_v6_sdl_thread / probe_v7_anim_deleted）
 *     —— 独立可执行文件，见 native/probe/
 *   · 跨语言探针（V1 / V3）—— 必须有仓颉代码参与，因此是仓颉可执行工程，
 *     通过本头文件里的 lvglcj_probe_* 接口向 C 侧借用「创建 OS 线程」等能力，
 *     见 probe/v1_thread_attach/ 与 probe/v3_affinity/
 */
#ifndef LVGLCJ_PROBE_H
#define LVGLCJ_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 通用工具 */
/*
 * 真实 OS 线程 ID。
 * 用 gettid(2) 而不是 pthread_self()：pthread_t 与 OS 线程不是一一对应
 * （在多数实现里 pthread_t 是指针/句柄，可能被复用），而 V3 探针的目的正是
 * 统计「仓颉轻量级线程是否被调度到不同的 OS 线程」，必须用内核线程号。
 */
int64_t lvglcj_probe_os_tid(void);

/* 打印带前缀的一行，便于 run_probe.sh 解析结论 */
void lvglcj_probe_report(const char *probe, const char *key, const char *value);

/* ============================================ V1：外部 OS 线程能否执行仓颉代码 */
/*
 * V1 是方案 A / C 的分叉闸门（设计文档 §3.6.3）。
 *
 * 测试方法：
 *   1. 仓颉侧把「一个顶层不捕获的 CFunc」注册进来（lvglcj_probe_set_cangjie_entry）
 *   2. 调用 lvglcj_probe_run_c_thread() → C 侧 pthread_create 出**独立 OS 线程**
 *   3. 该线程反复调用仓颉注册的函数指针
 *   4. 仓颉侧检查：是否被调用、调用时的 OS TID 是否与主线程相同、是否正确返回
 *
 * 判读：
 *   · 调用成功且返回 0        → 方案 A 可行
 *   · 崩溃 / 卡死 / 返回非 0  → 方案 A 不可行，退回方案 C（泵模式）
 */
int32_t lvglcj_probe_set_cangjie_entry(int32_t (*fn)(int32_t, int64_t));
int32_t lvglcj_probe_run_c_thread(int32_t iterations);

/* C 线程的执行结果：0 成功；非 0 为仓颉回调返回的错误码；-999 表示未执行完 */
int32_t lvglcj_probe_c_thread_result(void);
/* C 线程实际调用仓颉函数的次数 */
int32_t lvglcj_probe_c_thread_calls(void);
/* C 线程自身的 OS TID（用于与仓颉侧观察到的 TID 比对） */
int64_t lvglcj_probe_c_thread_tid(void);
/* 让主线程把 C 线程的执行结果同步回来（带超时，避免探针挂死） */
int32_t lvglcj_probe_join_c_thread(int32_t timeout_ms);

/* ==================================================== V4：ASan 有效性自证 */
/*
 * §9.5 要求「先人为制造一个 C 侧问题，确认 ASan 能报出」，
 * 否则「零报告」可能只是说明 ASan 没生效。
 * kind: 0 = 堆内存泄漏；1 = 堆缓冲区越界写
 * 返回 0 表示「按预期执行完毕」（ASan 应在退出时报错）
 */
int32_t lvglcj_probe_asan_trigger(int32_t kind);

/* ================================================= V7：deleted_cb 三路径 */
/*
 * 验证 LVGL v9.2 的 lv_anim deleted_cb 在三条路径下是否都触发（Patch P1 前提）：
 *   路径 1：自然结束（duration 到 / repeat 次数到）
 *   路径 2：手动删除（lv_anim_delete）
 *   路径 3：对象删除（lv_obj_delete 触发内部停动画）
 *
 * 输出三行 `V7 path1=... path2=... path3=...`，run_probe.sh 汇总。
 * 若三条不全触发 → Patch P1 的「internal deleted_cb 作唯一释放点」不成立，
 *   需启用兜底：anim 句柄表 + 与 lv_anim_count_running() 对账（§3.10.3）。
 */
int32_t lvglcj_probe_v7_anim_deleted(void);

/* ================================================= V6：SDL2 双线程同步 */
/*
 * 验证 §8.1.2 的可行性：SDL 事件循环在主线程、LVGL 渲染在另一线程，
 * 用信号量 + 超时同步；并验证「窗口关闭 → 主动释放信号量 → 等待方立即返回」。
 * 返回 0 表示全部子项通过。
 */
int32_t lvglcj_probe_v6_sdl_thread(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_PROBE_H */
