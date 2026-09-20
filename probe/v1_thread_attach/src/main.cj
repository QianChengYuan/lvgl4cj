/*
 * V1 探针 —— 「C 创建的 OS 线程能否执行仓颉代码」
 *
 * 为什么这是 P0 最重要的一个探针（设计文档 §3.6.3 / ADR-013）：
 *   方案 A 要求在一个「由 C 的 pthread_create 创建、仓颉运行时不认识」的
 *   OS 线程里执行仓颉闭包。这需要运行时提供线程附着机制
 *   （类比 JNI AttachCurrentThread）。
 *   如果做不到，就必须退回方案 C（泵模式），
 *   而 §3.2 / §3.6 的代码结构会随之改变 —— 所以必须在写业务代码之前定案。
 *
 * 测试设计上的两个要点：
 *   1. 回调里**做真实的堆分配**（字符串插值），
 *      因为「能返回一个常量」不等于「运行时在该线程上功能正常」；
 *   2. 回调是顶层不捕获的 CFunc（仓颉的 CFunc Lambda 不能捕获变量），
 *      状态通过全局原子量传递 —— 这也是分支 B 之所以必需的原因。
 *
 * 判读：
 *   调用次数 == 期望次数 且 返回 0 且 TID 与主线程不同 → 方案 A 可行
 *   崩溃 / 超时 / 非 0 → 方案 A 不可行，退回方案 C
 */
package v1_thread_attach

import std.sync.*

/* ------------------------------------------------------------------ FFI
 * ★ 为什么这些声明写在探针模块内，而不是复用 lvgl4cj 模块的声明：
 *   仓颉的 foreign 声明**不能带访问修饰符**（public/protected 均与 foreign 冲突），
 *   可见性上限是 internal =「当前包及子包」。因此它既不能跨包、也不能跨模块共享。
 *   探针是独立模块，只能自己声明所需的 C 符号。
 *   这一约束反而与设计立场一致：L1 只对外暴露包装好的公开 API，裸 FFI 不出模块。
 */
foreign func lvglcj_probe_set_cangjie_entry(fn: CFunc<(Int32, Int64) -> Int32>): Int32
foreign func lvglcj_probe_run_c_thread(iterations: Int32): Int32
foreign func lvglcj_probe_join_c_thread(timeoutMs: Int32): Int32
foreign func lvglcj_probe_c_thread_result(): Int32
foreign func lvglcj_probe_c_thread_calls(): Int32
foreign func lvglcj_probe_c_thread_tid(): Int64
foreign func lvglcj_probe_os_tid(): Int64

/* ---------------------------------------------------------------- 全局状态 */
/*
 * 顶层不捕获的 CFunc：只读写全局原子量，不捕获任何局部变量。
 * 这正是仓颉 CFunc 的硬约束，也是分支 B（闭包表）存在的根本原因。
 */
let gCalls = AtomicInt32(0)
let gLastTid = AtomicInt64(0)
let gLastStrLen = AtomicInt32(0)
let gAllocFailures = AtomicInt32(0)

let gCangjieEntry: CFunc<(Int32, Int64) -> Int32> = { i, callerTid =>
    /* ★ 故意做一次堆分配：验证运行时在该线程上不只是「能返回」而是「能工作」 */
    let s = "v1-tick-${i}"
    gLastStrLen.store(Int32(s.size))
    if (s.size <= 0) {
        gAllocFailures.fetchAdd(1)
    }
    gLastTid.store(callerTid)
    gCalls.fetchAdd(1)
    0
}

main() {
    println("=== V1: 外部 OS 线程执行仓颉代码 ===")

    let mainTid = unsafe { lvglcj_probe_os_tid() }
    println("[V1] 主线程 OS TID          : ${mainTid}")

    /* 1. 把仓颉函数指针交给 C 侧 */
    let setRc = unsafe { lvglcj_probe_set_cangjie_entry(gCangjieEntry) }
    println("[V1] 注册仓颉入口 rc        : ${setRc}")

    if (setRc != 0) {
        println("[V1] 结论：无法注册仓颉入口 → 方案 A 不可行")
        return
    }

    /* 2. 让 C 侧用 pthread_create 创建一条**独立 OS 线程**并调用它 */
    let iterations: Int32 = 2000
    let runRc = unsafe { lvglcj_probe_run_c_thread(iterations) }
    println("[V1] 启动 C 线程 rc         : ${runRc}")

    /* 3. 带超时地取回结果 —— 若仓颉运行时不支持，这里会超时或进程直接崩溃 */
    let joinRc = unsafe { lvglcj_probe_join_c_thread(5000) }
    let result = unsafe { lvglcj_probe_c_thread_result() }
    let calls = unsafe { lvglcj_probe_c_thread_calls() }
    let cTid = unsafe { lvglcj_probe_c_thread_tid() }
    let observedTid = gLastTid.load()
    let strLen = gLastStrLen.load()
    let allocFail = gAllocFailures.load()

    println("[V1] join rc                : ${joinRc}（0=正常返回，-9=超时）")
    println("[V1] C 线程返回码           : ${result}（0=成功）")
    println("[V1] 仓颉回调被调用次数     : ${calls} / ${iterations}")
    println("[V1] C 线程 OS TID          : ${cTid}")
    println("[V1] 回调内观察到的 TID     : ${observedTid}")
    println("[V1] 回调内分配的字符串长度 : ${strLen}（>0 说明堆分配正常）")
    println("[V1] 分配失败次数           : ${allocFail}")

    /* ------------------------------------------------------------ 判读 */
    let called = calls == iterations
    let returnedOk = result == 0
    let differentThread = (cTid != mainTid) && (cTid != 0)
    let heapWorked = (strLen > 0) && (allocFail == 0)
    let noTimeout = joinRc == 0

    println("")
    println("[V1] 判定：")
    println("       完整调用    : ${called}")
    println("       返回成功    : ${returnedOk}")
    println("       确为另一线程: ${differentThread}")
    println("       堆分配正常  : ${heapWorked}")
    println("       未超时      : ${noTimeout}")

    if (called && returnedOk && differentThread && heapWorked && noTimeout) {
        println("[V1] 结论：方案 A 可行 —— C 创建的 OS 线程可以执行仓颉代码")
        println("[V1]       采用 lvglcj_start_thread 独立 OS 线程主循环")
    } else {
        println("[V1] 结论：方案 A 不可行 —— 退回方案 C（泵模式）")
        println("[V1]       由仓颉主循环周期性调 pump()")
        println("[V1]       注意：方案 C 下禁止宣传「实时 HMI」（§11.2）")
    }
}
