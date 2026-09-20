/*
 * V3 探针 —— 仓颉轻量级线程的 OS 亲和性（设计文档 §3.6.4）
 *
 * 要回答的问题：
 *   仓颉的 spawn 是 M:N 轻量级线程。当它阻塞（如 sleep）后再次就绪时，
 *   运行时可能把它调度到**另一条 OS 线程**上。
 *   如果确实是这样，那么「让仓颉 spawn 出来的线程直接跑 LVGL 主循环」
 *   就是不可靠的 —— 这正是 §3.6 排除方案 B、坚持 C 侧掌控线程的直接依据。
 *
 * 方法：
 *   在 spawn 出的线程里反复记录 gettid(2) 得到的 OS 线程号，
 *   统计出现过的不同 TID 数量。
 *
 * 判读：
 *   = 1  → 亲和性稳定，方案 B 理论可行（但本项目仍用方案 A，因为 A 更强）
 *   > 1  → 发生迁移，方案 B 不可行（与预期一致）
 */
package v3_affinity

import std.collection.*
import std.sync.*

foreign func lvglcj_probe_os_tid(): Int64

let gIterations: Int64 = 2000

main() {
    println("=== V3: 仓颉轻量级线程的 OS 亲和性 ===")

    let mainTid = unsafe { lvglcj_probe_os_tid() }
    println("[V3] 主线程 OS TID : ${mainTid}")
    println("[V3] 迭代次数      : ${gIterations}（每次迭代 sleep 1ms 制造阻塞点）")

    let tids = HashSet<Int64>()
    let mtx = Mutex()
    let done = SyncCounter(1)

    spawn {
        =>
        for (i in 0..gIterations) {
            let tid = unsafe { lvglcj_probe_os_tid() }
            synchronized(mtx) {
                tids.add(tid)
            }
            /* 阻塞点：正是它让运行时有机会把该轻量级线程迁到别的 OS 线程 */
            sleep(Duration.millisecond)
        }
        done.dec()
    }

    done.waitUntilZero()

    let distinct = synchronized(mtx) { tids.size }

    println("[V3] 观察到的不同 OS 线程数 : ${distinct}")

    let tidListText = synchronized(mtx) {
        let sb = StringBuilder()
        var first = true
        for (t in tids) {
            if (!first) {
                sb.append(", ")
            }
            sb.append("${t}")
            first = false
        }
        sb.toString()
    }
    println("[V3] TID 集合      : [${tidListText}]")

    println("")
    if (distinct <= 1) {
        println("[V3] 结论：亲和性稳定（未观察到迁移）")
        println("[V3]       但本项目仍采用方案 A（C 侧 OS 线程），因为它的确定性更强、与运行时演化解耦")
    } else {
        println("[V3] 结论：发生 M:N 迁移（${distinct} 个不同 OS 线程）")
        println("[V3]       证实方案 B（仓颉 spawn 线程直接跑 LVGL 主循环）不可行")
        println("[V3]       印证 ADR-003 / ADR-004：主循环线程必须由 C 侧掌控")
    }
}
