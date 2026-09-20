/*
 * queue.cj —— 跨线程任务队列的仓颉侧（设计文档 §3.9）
 *
 * ===================== 两侧分工 =====================
 *   C 侧（native/src/queue.c）：有界环形缓冲、锁与条件变量、等待图死锁检测、超时
 *   仓颉侧（本文件）          ：把「一段仓颉代码」变成 task_id、把错误码翻译成异常
 *
 * ===================== task_id 就是 cid =====================
 * 投递任务 = 「注册一个**一次性闭包** + 把 cid 交给 C 侧队列」，
 * 依据 lvglcj_bridge.h §5.1「task_id 由仓颉侧分配，C 侧只透传给 dispatch」。
 *
 * 这样做的好处是任务与回调**共用同一套机制**：
 *   · 同一处异常吞并（dispatchClosure 的 catch），任务体抛异常不会污染队列
 *   · 同一处回收（一次性闭包在 dispatch 中摘除），不会漏
 *   · 不需要第二套分发入口，也就不需要第二套注册 API
 *
 * ===================== 本文件唯一的易错点 =====================
 * 「入队失败」时必须**立刻注销**闭包：该 cid 永远不会被执行，
 * 不注销就在闭包表里留下一条永不回收的记录。
 * 这类泄漏只在队列满时才发生，因此极难在常规测试里发现。
 */
package lvgl4cj

import std.deriving.Derive

/*
 * 关闭策略（数值与 C 侧 lvglcj_shutdown_mode_t 逐值对齐）
 * ★ 数值不是任选的：C 侧按整数分支，写错会静默走错策略。
 * @Derive[Equatable]：仓颉的枚举默认不支持 ==，需要显式派生
 * （否则 `mode == Drain` 会是编译错误，而不是静默返回 false）。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum ShutdownMode {
    | Drain // 0：把剩余任务做完再关
    | Discard // 1：丢弃剩余任务，并唤醒所有等待者
    | DrainWithTimeout(Int32) // 2：限时清空（超时毫秒数）
}

/*
 * 队列满策略（数值与 C 侧 lvglcj_queue_full_policy_t 逐值对齐）
 * 默认 FailFast 的理由见设计文档 §3.9：阻塞入队会形成跨线程死锁链条。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum QueueFullPolicy {
    | FailFast // 0：立即返回 QueueFull（默认）
    | Block // 1：阻塞等空位（**禁止**在 LVGL 线程/回调中使用）
    | DropOldest // 2：丢弃最旧的任务
}

func shutdownModeToCode(m: ShutdownMode): Int32 {
    match (m) {
        case Drain => 0
        case Discard => 1
        case DrainWithTimeout(_) => 2
    }
}

func queueFullPolicyToCode(p: QueueFullPolicy): Int32 {
    match (p) {
        case FailFast => 0
        case Block => 1
        case DropOldest => 2
    }
}

/// 当前排队中的任务数（观测用；正常应接近 0）
public func taskQueueSize(): Int32 {
    unsafe { lvglcj_queue_size() }
}

/// 设置队列容量。C 侧会保留已排队的任务；缩容时被挤掉的等待者会收到 QueueFull
public func setTaskQueueCapacity(capacity: Int32): Unit {
    throwIfError(unsafe { lvglcj_queue_set_capacity(capacity) },
        "setTaskQueueCapacity(${capacity})（容量需在 1..1048576）")
}

/// 设置队列满策略
public func setTaskQueueFullPolicy(policy: QueueFullPolicy): Unit {
    throwIfError(unsafe { lvglcj_queue_set_full_policy(queueFullPolicyToCode(policy)) }, "setTaskQueueFullPolicy")
}

/*
 * 把 body 包装成一次性闭包并投递到 LVGL 线程（异步，不等待执行）。
 * 返回错误码，不抛异常 —— 供上层 LvglRuntime 决定是否抛。
 */
func submitTask(body: () -> Unit): Int32 {
    let cid = registerOneShotClosure {_ => body()}
    /* 数值转换用 Int64(x)（仓颉无 toInt64() 成员方法） */
    let rc = unsafe { lvglcj_post_task(Int64(cid)) }
    if (rc != 0) {
        /*
         * 入队失败 → 该 cid 永远不会被分发，必须立刻回收。
         * 若返回非 0 是因为任务已入队但随后执行失败，则一次性闭包已在
         * dispatch 中摘除，这里再注销一次是幂等的，不会重复计数。
         */
        unregisterClosure(cid)
    }
    return rc
}

/*
 * 投递并**同步等待**执行完成。仅可在非 LVGL 线程调用
 * （在 LVGL 线程或回调内调用会由 C 侧等待图检测拒绝并返回 DeadlockRisk，
 *   而不是挂死 —— 这正是 §3.8.4 存在的意义）。
 *
 * 返回值同时承载两种语义，务必区分（§3.9）：
 *   · 入队失败（QueueFull / DeadlockRisk）：任务**没有执行**
 *   · 执行失败（如 CallbackThrew）：任务**执行了**，但闭包抛了异常
 */
func submitTaskAndWait(body: () -> Unit): Int32 {
    let cid = registerOneShotClosure {_ => body()}
    let rc = unsafe { lvglcj_post_and_wait(Int64(cid)) }
    if (rc != 0) {
        /*
         * 统一在失败路径注销，三种情况都安全：
         *   未入队     → 正是必须回收的情况（防泄漏）
         *   已执行失败 → 一次性闭包已回收，此处幂等无副作用
         *   等待超时   → 调用方已放弃，阻止其稍后被迟到执行更可预期
         */
        unregisterClosure(cid)
    }
    return rc
}
