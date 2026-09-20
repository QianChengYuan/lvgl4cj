/*
 * lvgl.cj —— lvgl4cj 模块根包
 *
 * 根包必须至少包含一个 .cj 文件，否则子包（core/ widgets/ ffi/ 等）全部无效
 * （cjpm 规则：所有父包到根包都须是有效源包）。
 *
 * 本文件承载：
 *   · 模块级常量与版本信息
 *   · 面向用户的顶层便捷入口
 */
package lvgl4cj

public let LVGLCJ_VERSION: String = "0.1.0"

/*
 * 构建期配置快照（§3.5.2）
 *
 * CONF_HASH 由 scripts/gen_conf_const.py 读取 probe_conf 的 JSON 输出写入
 * src/generated/conf_const.cj。启动时与运行库的 lvglcj_conf_hash() 比对，
 * 不一致即抛 InvalidConfig —— 把「换机器构建导致配置不一致」这种隐蔽的
 * 花屏/崩溃，前置为一条明确的启动报错（ADR-008）。
 */

/// 底层 LVGL 版本串（形如 "9.2.2 (lvgl4cj)"）
public func lvglVersion(): String {
    unsafe { lvglcj_version().toString() }
}

/// 当前运行库的配置哈希
public func confHash(): UInt32 {
    unsafe { lvglcj_conf_hash() }
}

/// 当前运行库的颜色深度
public func colorDepth(): Int32 {
    unsafe { lvglcj_conf_color_depth() }
}

/// 构建期是否启用 LVGL 日志转发
public func useLog(): Bool {
    unsafe { lvglcj_conf_use_log() != 0 }
}

/// LVGL 内存池大小（字节）；使用系统分配器时返回 0
public func memPoolSize(): Int32 {
    unsafe { lvglcj_conf_mem_size() }
}

/* ---------------------------------------------------------------- 线程归属 */
/*
 * 声明「当前调用线程就是 LVGL 线程」（设计文档 §3.6.5）。
 *
 * 什么时候必须调用：
 *   · **方案 C（泵模式）**：用户主循环所在线程必须自己声明身份，
 *     否则 g_lvgl_thread_id 会停留在 lv_init 时的那条线程上，pump() 会被线程断言误拦。
 *   · 测试 / 工具：仓颉的轻量级线程是 M:N 调度，**不同调用可能落在不同 OS 线程**
 *     （V3 探针已实测确认迁移存在）。多线程测试框架下若不在每次入口重新声明，
 *     就会出现「LVGL 调用与创建者不是同一条线程」的数据竞争。
 *
 * 什么时候**不要**调用：
 *   · **方案 A（独立 OS 线程主循环）**：主循环线程会自行绑定，
 *     随意重绑会破坏线程断言的意义 —— 那正是我们用来抓跨线程误用的机制。
 */
public func bindCurrentThreadAsLvgl(): Int32 {
    return unsafe { lvglcj_rebind_current_thread() }
}
