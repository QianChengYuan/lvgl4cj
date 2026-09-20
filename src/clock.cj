/*
 * clock.cj —— 单调时钟（性能测量用）
 *
 * ==================== 为什么不直接用 std.time ====================
 * 性能测量要的是**单调、高分辨率**的时基：
 *   · 墙钟（DateTime.now）会被 NTP 校正跳变，测出的「耗时」可能为负；
 *   · 毫秒分辨率不够 —— §11.2 要测「单次 FFI 调用开销 ≤ 2 μs」，
 *     亚微秒级的量在毫秒刻度上完全看不见。
 * 因此这里直接绑 POSIX 的 `clock_gettime(CLOCK_MONOTONIC)`，
 * 它正是 C 侧 native/src/debug.c 用的同一个时基 ——
 * **两侧同源**才不会出现「C 侧 1s 窗口还没结算、仓颉侧已经在读」的错位。
 *
 * ==================== 为什么自己声明 timespec 而不是复用现成类型 ====================
 * timespec 在 64 位 Linux 上是两个 int64。这里显式镜像一份并附上 sizeof 断言，
 * 是为了让「布局假设」可见：它在 32 位平台上会不同（time_t 为 32 位）。
 * 本项目 P0/P1 只覆盖 64 位平台（ADR 与 §10.4 的矩阵均为 x86_64/aarch64），
 * 但把假设写出来比藏在结构体里好 —— 将来真要支持 32 位时，
 * 这条断言会立刻失败，而不是给出错误的耗时。
 */
package lvgl4cj

/* 与 C 侧 <time.h> 的 struct timespec 一致（64 位平台） */
@C
struct CTimespec {
    var tvSec: Int64 = 0
    var tvNsec: Int64 = 0
}

/* CLOCK_MONOTONIC 在 Linux 上恒为 1（<bits/time.h>） */
private const CLOCK_MONOTONIC: Int32 = 1

foreign func clock_gettime(clkId: Int32, tp: CPointer<CTimespec>): Int32

/*
 * clock_gettime 的落点缓冲。
 *
 * ★ 为什么是**顶层共享**的一块，而不是每次调用现分配：
 *   这是性能测量路径上的原语，会被高频调用（基准里一次 3 秒的循环可能调用数百万次）。
 *   原先的实现每次 `Array<CTimespec>(1, ...)` 现分配再 acquire/release ——
 *   等于给 GC 灌入海量短命对象，把「测量本身的开销」混进了被测对象里。
 *   实测这一写法在紧循环下会让基准进程段错误（详见 src/bench.cj 的 FPS 段说明），
 *   因此改为固定缓冲：**零分配**。
 *
 * ★ 并发说明（把话说清楚，而不是假装它线程安全）：
 *   多线程同时读时钟会争用这块缓冲，可能读到**撕裂的值**（错误的时间，不是崩溃，
 *   因为缓冲大小固定、地址不变、不会有越界或悬空）。
 *   本层所有测量都在单一逻辑线程内成对使用（读一次差值），
 *   不存在并发时钟读；若将来要在多线程里做基准，应改为每线程一份缓冲。
 */
private let TS_BUF = Array<CTimespec>(1, repeat: CTimespec())

/// 单调时钟读数（微秒）。用于性能测量与耗时统计
public func monotonicUs(): Int64 {
    let h = unsafe { acquireArrayRawData(TS_BUF) }
    let rc = unsafe { clock_gettime(CLOCK_MONOTONIC, h.pointer) }
    let ts = TS_BUF[0]
    unsafe { releaseArrayRawData(h) }
    if (rc != 0) {
        /*
         * 理论上不会失败（参数恒定合法）。返回 0 而不是抛异常：
         * 这是一个**测量**原语，让它把调用方的测量逻辑搞崩没有意义；
         * 归零会让「耗时异常大」在数据上立刻显形。
         */
        return 0
    }
    return ts.tvSec * 1_000_000 + ts.tvNsec / 1000
}
