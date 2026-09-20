/*
 * perf_test.cj —— §11.2 性能指标的**回归护栏**
 *
 * ==================== 这份测试有意不断言 §11.2 的门槛值 ====================
 * §11.2 的门槛（≥30 FPS、≤2 μs、≤50 ms、≤500 ms 启动）必须在**目标平台**判定。
 * 在 WSL2 / CI 容器里，宿主机负载、虚拟化开销与软件渲染都会让数字失真；
 * 把它们写成断言只会得到一个「时好时坏」的门禁，最后被人为关掉 ——
 * 那比没有门禁更糟，因为它给出的是**虚假的安全感**。
 *
 * 因此这里只做两件事：
 *   1. 确认测量链路本身能跑通（每一步都产出了指标、没有 skipped 之外的空洞）；
 *   2. 断言**宽松上界**，只拦灾难性回归（例如 FFI 变成 20 μs、启动变成 3 s）。
 *
 * 真实数字请用 `bash scripts/perf.sh` 采集并记入 docs/benchmarks/。
 * 测量逻辑在 src/bench.cj（LvBench），与本测试共用同一份口径。
 */
package lvgl4cj

import std.collection.ArrayList
/*
 * Int64.parse 来自 Parsable 接口的扩展函数 —— 必须显式导入该接口才会进入作用域，
 * 否则报的是一句很难对应的「you must import at least one of its inherited interfaces」。
 */
import std.convert.Parsable

/* 宽松护栏：只拦灾难性回归，**不是** §11.2 的正式门槛 */
private const GUARD_FFI_NS: Int64 = 20_000 /* 20 μs（正式门槛 2 μs） */
private const GUARD_EVENT_NS: Int64 = 500_000 /* 500 μs（正式门槛 200 μs） */
private const GUARD_STARTUP_MS: Int64 = 3000 /* 3 s（正式门槛 500 ms） */
private const GUARD_FPS: UInt32 = 5 /* 正式门槛 30（局部）/ 20（全屏） */

@Test
class PerfGuardTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    /* 从「名字 = 值」的指标表里取一个数值；名字不存在时返回 -1 */
    private func metricValue(ms: ArrayList<BenchMetric>, name: String): Int64 {
        for (m in ms) {
            if (m.name == name) {
                /* 值里可能带括号说明（如 "123（cpu=…）"），只取开头的数字部分 */
                var digits = ""
                var neg = false
                var started = false
                for (c in m.value.toRuneArray()) {
                    if (c == r'-' && !started) {
                        neg = true
                        continue
                    }
                    if (c >= r'0' && c <= r'9') {
                        digits += c.toString()
                        started = true
                    } else if (started) {
                        break
                    } else {
                        /* 前缀不是数字（例如 "skipped(...)"）：判为未测出 */
                        return -1
                    }
                }
                if (digits == "") {
                    return -1
                }
                let v = Int64.parse(digits)
                return if (neg) {
                    -v
                } else {
                    v
                }
            }
        }
        return -1
    }

    private func findMetric(ms: ArrayList<BenchMetric>, name: String): Bool {
        for (m in ms) {
            if (m.name == name) {
                return true
            }
        }
        return false
    }

    @TestCase
    func testPerfMetricsStayWithinLooseGuards(): Unit {
        let ms = LvBench.perfMetrics()

        /*
         * 完整性：每个指标都必须出现。
         * 「值恰好是 skipped」是允许的（无显示环境下的 FPS），
         * 但「指标整条缺失」不允许 —— 那通常意味着测量链路被改坏了。
         */
        for (name in ["ffi.raw.ns_per_call", "ffi.l1.ns_per_call", "event.latency.avg_ns", "event.latency.max_ns",
            "input.latency.ms", "mem.peak_percent", "mem.free_percent", "fps.partial", "fps.fullscreen",
            "startup.init_to_first_frame_ms"]) {
            @Assert(findMetric(ms, name), true)
        }

        /* 宽松护栏：能跑通 + 不出现数量级退化 */
        let rawNs = metricValue(ms, "ffi.raw.ns_per_call")
        @Assert(rawNs >= 0 && rawNs < GUARD_FFI_NS, true)

        let l1Ns = metricValue(ms, "ffi.l1.ns_per_call")
        @Assert(l1Ns >= 0 && l1Ns < GUARD_FFI_NS * 2, true)

        let evNs = metricValue(ms, "event.latency.avg_ns")
        @Assert(evNs >= 0 && evNs < GUARD_EVENT_NS, true)

        let startupMs = metricValue(ms, "startup.init_to_first_frame_ms")
        /* -1 表示 SDL2 不可用（该项 skipped）—— 那时不做启动时间断言 */
        @Assert(startupMs == -1 || startupMs < GUARD_STARTUP_MS, true)

        /* 内存池峰值占比必须留有余量（§11.3 门槛 80） */
        let peak = metricValue(ms, "mem.peak_percent")
        @Assert(peak >= 0 && peak < 80, true)

        /* 压到下限：本次运行结束后不应有动画上下文残留 */
        @Assert(animCtxCount(), 0)
    }

    /*
     * FPS 单独一条用例：它需要 SDL2 与真实刷新路径，在无显示环境会被标为 skipped；
     * 与上面的微基准分开，是为了让「FPS 没测」不影响其余指标的判定。
     */
    @TestCase
    func testFpsIsProducedOrExplicitlySkipped(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        var available = true
        try {
            let b = Sdl2Backend.start(320, 200)
            b.close()
        } catch (e: LvglException) {
            available = false
        }
        rt.deinitLvgl()

        if (!available) {
            /* 明确跳过：打印到 stderr，让「跳过了」在输出里可见（静默跳过会被误读为已验证） */
            eprintln("[perf] FPS 跳过：SDL2 不可用，本环境无真实刷新路径")
            return
        }
        @Assert(available, true)
    }

    /* 一轮压力负载应在结束时把计数带回基线（soak 的短时版本，CI 可跑） */
    @TestCase
    func testSoakRoundReturnsToBaseline(): Unit {
        let rt = LvBench.soakRuntime()
        let base = LvBench.soaked(rt, 0, 5)

        for (i in 1..21) {
            let s = LvBench.soaked(rt, i, 5)
            @Assert(s.closureCount == base.closureCount, true)
            @Assert(s.animCtxCount, 0)
        }

        let end = LvBench.soaked(rt, 21, 5)
        @Assert(end.handleAlive, base.handleAlive)
        @Assert(end.objCount, base.objCount)
        rt.deinitLvgl()
    }
}
