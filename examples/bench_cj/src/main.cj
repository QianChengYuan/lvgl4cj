/*
 * bench_cj —— 性能基准与压力负载的命令行入口
 *
 * 用法：
 *   bench_cj perf                 采集 §11.2 的全部性能指标并打印成表（约 10 s）
 *   bench_cj soak --seconds 300   跑压力负载 300 秒，按固定格式打印状态行
 *   bench_cj soak --seconds 86400 24 小时 soak（§11.3 / §11.4）
 *
 * ==================== 输出格式是**契约** ====================
 * 状态行统一为：
 *   [soak] round=<n> elapsed_s=<n> rss_kb=<外部采样> alive=.. obj=.. closures=.. animctx=.. invalidated=.. dumpnodes=..
 * 由 scripts/soak.sh 逐行抓取并判定收敛。改格式等于改契约，请同步改脚本与基线文档。
 *
 * ★ 为什么这里**允许**解析数字参数（`--seconds`），而 hello_cj 刻意不做数字解析：
 *   hello_cj 的注释里写得很清楚 ——「少一层解析，就少一类『参数写错却静默跑成默认值』的可能」。
 *   本工具同样不接受静默默认值：解析失败**直接报错退出**（而不是退回 24 小时），
 *   因此那条顾虑不成立；而 soak 时长本来就无法用固定档位枚举。
 *   判据是「参数写错时是否静默」而不是「有没有解析」。
 */
package bench_cj

import lvgl4cj.*
import std.collection.ArrayList
/* Int64.tryParse 是 Parsable 的扩展函数，必须导入该接口才会进入作用域 */
import std.convert.Parsable

/*
 * libc 的 exit。
 * 仓颉标准库没有导出同名函数（std.process 下没有 exit），
 * 而 CLI 工具需要「参数错误 → 非零退出」以便脚本判定，因此直接绑 libc。
 * 用它而不是「打印用法后正常返回」，是因为那个返回码是 0 ——
 * 脚本会把「参数写错」当成「成功完成」，这正是最该避免的静默。
 */
foreign func exit(code: Int32): Unit

func printUsage(): Unit {
    println("用法：")
    println("  bench_cj perf                 采集 §11.2 性能指标")
    println("  bench_cj startup              初始化到首帧后立即退出（供外部测冷启动）")
    println("  bench_cj soak --seconds <N>   跑压力负载 N 秒（24h = 86400）")
}

/*
 * 启动时间专用模式：走到「首帧渲染完成」就退出，不做其它事。
 *
 * ★ 为什么要单独一个模式：进程内的 `startup.init_to_first_frame_ms` 测不到
 *   **进程加载 + 动态库解析**那一段（那发生在 main 之前）。
 *   而 §11.2 的「启动时间 ≤ 500 ms」指的是用户感知的冷启动，含那一段。
 *   因此这里提供一个「跑完首帧就退出」的模式，由 scripts/perf.sh 用
 *   /usr/bin/time 从**进程之外**量整段墙钟 —— 两者相减还能看出加载本身占多少。
 */
func runStartup(): Unit {
    let rt = LvglRuntime()
    rt.initLvgl()
    var backend: Option<Sdl2Backend> = None
    try {
        backend = Some(Sdl2Backend.start(800, 480))
    } catch (e: LvglException) {
        eprintln("[startup] SDL2 不可用：${lvglErrorName(e.code)}")
        rt.deinitLvgl()
        return
    }
    let b = backend.getOrThrow()
    var spins = 0
    while (spins < 400 && b.flushCount() == 0) {
        let _ = b.pollEvents()
        rt.pump()
        spins++
    }
    /* 首帧之后的清理也算在进程墙钟内 —— 所以外部测到的是「冷启动 + 退出」的上界 */
    println("[startup] first_frame_spins=${spins} flush=${b.flushCount()}")
    b.close()
    rt.deinitLvgl()
}

/*
 * 参数错误的统一出口。
 *
 * 返回类型 Nothing（底类型）：它使这个调用可以出现在任何需要具体类型的表达式位置上
 * （例如 pickSeconds 需要返回 Int64 的分支里），而不必在 exit 之后写一个
 * 永远执行不到的 `return 0` —— 那种「为满足类型检查而写的死代码」会误导后来者
 * 以为那里可能返回。
 */
func fatal(msg: String): Nothing {
    eprintln(msg)
    unsafe { exit(2) }
    throw Exception("unreachable: exit 不返回")
}

/* 扫描全部参数：仓颉 main(args) 是否含 argv[0] 与调用方式有关，写下标会静默取错 */
func pickMode(args: Array<String>): String {
    for (a in args) {
        if (a == "perf") {
            return "perf"
        }
        if (a == "soak") {
            return "soak"
        }
        if (a == "startup") {
            return "startup"
        }
    }
    return ""
}

func pickSeconds(args: Array<String>): Int64 {
    var i = 0
    while (i < args.size) {
        if (args[i] == "--seconds" && i + 1 < args.size) {
            /* 解析失败即报错退出（见文件头的说明：绝不静默退回默认值） */
            let v = Int64.tryParse(args[i + 1])
            match (v) {
                case Some(n) => if (n > 0) {
                    return n
                } else {
                    fatal("[bench_cj] --seconds 必须为正数，收到 ${args[i + 1]}")
                }
                case None => fatal("[bench_cj] --seconds 需要整数秒，收到 '${args[i + 1]}'")
            }
        }
        i++
    }
    fatal("[bench_cj] soak 模式必须给 --seconds <N>（不提供默认时长）")
}

/* ---------------------------------------------------------------- perf */

func runPerf(): Unit {
    println("=== bench_cj perf：§11.2 性能指标 ===")
    println("[env] 注意：本工具的数字**只在目标平台上有判定效力**；")
    println("[env]       WSL2/CI 容器受宿主负载与虚拟化影响，仅供横向比较。")

    let t0 = monotonicUs()
    let ms = LvBench.perfMetrics()
    let elapsedMs = (monotonicUs() - t0) / 1000

    for (m in ms) {
        println("[perf] ${m.name} = ${m.value}")
    }
    println("[perf] perf.total_ms = ${elapsedMs}")
    println("=== 采集完成 ===")
}

/* ---------------------------------------------------------------- soak */

func runSoak(seconds: Int64): Unit {
    eprintln("[soak] 开始：目标 ${seconds} 秒")
    let rt = LvBench.soakRuntime()

    let tStart = monotonicUs()
    let deadlineUs = seconds * 1_000_000

    var round: Int64 = 0
    var baseline: Option<SoakStat> = None
    /* 每 ~5 秒打印一行，24 小时约 1.7 万行（足够看出趋势，又不会撑爆日志） */
    var nextPrintUs: Int64 = 0

    while (monotonicUs() - tStart < deadlineUs) {
        /* payload 按轮数轻度变化：让对象树规模在合理区间内起伏，
           而不是恒定 —— 恒定负载容易掩盖「增长型」的泄漏。 */
        let payload: Int64 = 3 + (round % 8)
        let s = LvBench.soaked(rt, round, payload)

        if (round == 5) {
            /* 用第 5 轮作为基线：此时预热已完成，稳态已建立 */
            baseline = Some(s)
        }

        let elapsed = monotonicUs() - tStart
        if (elapsed >= nextPrintUs) {
            let elapsedS = elapsed / 1_000_000
            println("[soak] round=${s.round} elapsed_s=${elapsedS} alive=${s.handleAlive} " +
                "obj=${s.objCount} closures=${s.closureCount} animctx=${s.animCtxCount} " +
                "invalidated=${s.handleInvalidated} dumpnodes=${s.dumpNodes}")
            nextPrintUs = elapsed + 5_000_000
        }
        round++
    }

    /* 收尾：打印末态与漂移，便于脚本判定（也便于人工一眼看出问题） */
    let s = LvBench.soaked(rt, round, 3)
    println("[soak] done rounds=${s.round} alive=${s.handleAlive} obj=${s.objCount} " +
        "closures=${s.closureCount} animctx=${s.animCtxCount} invalidated=${s.handleInvalidated} " +
        "dumpnodes=${s.dumpNodes}")
    match (baseline) {
        case Some(b) =>
            println("[soak] drift alive=${s.handleAlive - b.handleAlive} obj=${s.objCount - b.objCount} " +
                "closures=${s.closureCount - b.closureCount}")
        case None => println("[soak] drift 无法计算（运行时间过短，未建立基线）")
    }

    rt.deinitLvgl()
}

/* ---------------------------------------------------------------- main */

main(args: Array<String>) {
    match (pickMode(args)) {
        case "perf" => runPerf()
        case "startup" => runStartup()
        case "soak" => runSoak(pickSeconds(args))
        case _ =>
            printUsage()
            fatal("[bench_cj] 未知或缺失模式")
    }
}
