/*
 * bench.cj —— 性能基准与压力负载（设计文档 §11.2 / §11.3 / §11.4）
 *
 * ==================== 为什么测量逻辑在库里，而且是「返回值」而不是「打印」 ====================
 * 一开始这部分代码写在 src/perf_test.cj 里并直接 println。
 * 实测发现**仓颉测试框架会接管 stdout 与 stderr**（连 eprintln 也抓不到），
 * 于是「测出了数字」这件事在测试报告里完全不可见 —— 而产不出数字的基准没有意义。
 *
 * 改为：测量逻辑放在库里、以**数据**形式返回；两个消费方各取所需 ——
 *   · src/perf_test.cj  → 只看宽松护栏，纳入 `cjpm test` 做回归监控；
 *   · examples/bench_cj → 打印成表，由 scripts/perf.sh 抓取入库。
 * 这样同一份测量逻辑只写一次，不会出现「测试里的口径与基准里的口径悄悄分叉」。
 *
 * ==================== 口径（与 docs/benchmarks/ 的基线模板必须一致）====================
 *   ffi.raw   裸 foreign 调用的单次耗时 —— 纯跨语言边界成本
 *   ffi.l1    含 L1 包装（线程归属检查等）的单次耗时；与 raw 之差即安全检查成本
 *   event     自发事件 → 闭包内取时间戳的差值
 *   input     合成输入「松手递送」→ Clicked 回调的差值（不含硬件/驱动，本层观测不到那一段）
 *   fps       连续失效驱动下的每秒完成刷新数；局部 = 一个小对象，全屏 = 全屏对象的远距往复
 *   startup   initLvgl → 首帧渲染完成的墙钟耗时（不含进程加载，那部分由 /usr/bin/time 另测）
 *   mem       LVGL 内存池峰值/空闲占比（§11.3 门槛：峰值 ≤ 80%）
 *
 * ★ FPS 依赖真实刷新路径，因此需要 SDL2 后端；无显示环境时该项**明确标记为 skipped**
 *   而不是填 0 —— 「没测」与「测出来是 0」在数据上必须可区分。
 */
package lvgl4cj

import std.collection.ArrayList

/// 一条基准指标
public struct BenchMetric {
    public let name: String
    public let value: String

    public init(name: String, value: String) {
        this.name = name
        this.value = value
    }
}

/// 一轮压力负载后的状态快照（§11.3 的收敛判据读这些数）
public struct SoakStat {
    /// 已完成的轮数
    public let round: Int64
    /// 存活的句柄数（应稳定不增）
    public let handleAlive: Int32
    /// LVGL 对象数（应稳定不增）
    public let objCount: Int32
    /// 闭包表大小（对象删除后应回到基线）
    public let closureCount: Int32
    /// 活跃 anim_ctx 数（动画跑完应归零）
    public let animCtxCount: Int32
    /// 已失效但仍保留表项的句柄数（有界，见 handle_table.h）
    public let handleInvalidated: Int32
    /// 本轮 dumpTree 展开出的节点数。正常恒 >= 1（屏幕自身至少算一个节点）；
    /// 出现 0 说明 dump 链路失效 —— 由调用方判定，负载本身不抛异常
    /// （「负载负责报告、调用方负责判定」的分工，避免把诊断逻辑混进压力路径）
    public let dumpNodes: Int32

    public init(round: Int64, handleAlive: Int32, objCount: Int32, closureCount: Int32, animCtxCount: Int32,
        handleInvalidated: Int32, dumpNodes: Int32) {
        this.round = round
        this.handleAlive = handleAlive
        this.objCount = objCount
        this.closureCount = closureCount
        this.animCtxCount = animCtxCount
        this.handleInvalidated = handleInvalidated
        this.dumpNodes = dumpNodes
    }
}

public class LvBench {
    // ==================================================== 性能指标

    /*
     * 跑一遍全部性能指标。返回可打印的 (名字, 值) 列表。
     * 每个子场景自带运行时与 display，互不干扰 —— 否则上一个场景的残留对象会影响下一个的 objCount。
     */
    public static func perfMetrics(): ArrayList<BenchMetric> {
        let m = ArrayList<BenchMetric>()

        /*
         * 微基准共用一套 headless 运行时：FFI 开销、事件回调延迟、输入延迟。
         * 三者都用不到真实刷新路径，headless 反而更干净（不受 SDL 影响）。
         *
         * 拆成三个函数是因为单函数超过 50 行会被判为过长（G.FUN.01），
         * 而它们各自的关注点确实不同：跨语言边界成本 / 回调派发成本 / 完整输入链路。
         */
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)
        let scr = LvObject.screenActive()

        let ffiTarget = LvObject.create(scr)
        ffiAndEventMetrics(m, ffiTarget)
        ffiTarget.close()
        inputLatencyMetric(m, rt, disp, scr)

        m.add(BenchMetric("mem.peak_percent", "${memPeakPercent()}（§11.3 门槛 <= 80）"))
        m.add(BenchMetric("mem.free_percent", "${memFreePercent()}"))
        m.add(BenchMetric("obj_count.headless", "${LvDebug.objCount()}"))
        m.add(BenchMetric("handle.alive", "${handleCount(HandleState.Alive)}"))

        rt.deinitLvgl()

        /* FPS + 启动时间需要真实刷新路径，单独一套运行时（见 fpsAndStartup 的说明） */
        fpsAndStartup(m)
        return m
    }

    /* 跨语言边界开销与事件回调派发延迟。两者共用同一个「靶对象」，故放在一起 */
    private static func ffiAndEventMetrics(m: ArrayList<BenchMetric>, target: LvObject): Unit {
        // ---- FFI 开销
        let n: Int64 = 100_000
        /* 预热：让句柄表查找、指令缓存与分支预测进入稳态，否则前若干次明显偏慢 */
        for (i in 0..10_000) {
            unsafe { lvglcj_obj_set_pos(target.handle, Int32(i % 100), 0) }
        }
        let tA = monotonicUs()
        for (i in 0..n) {
            unsafe { lvglcj_obj_set_pos(target.handle, Int32(i % 100), 0) }
        }
        let tB = monotonicUs()
        let rawNs = (tB - tA) * 1000 / n

        let tC = monotonicUs()
        for (i in 0..n) {
            target.setPos(Int32(i % 100), 0)
        }
        let tD = monotonicUs()
        let l1Ns = (tD - tC) * 1000 / n

        m.add(BenchMetric("ffi.raw.ns_per_call", "${rawNs}"))
        m.add(BenchMetric("ffi.l1.ns_per_call", "${l1Ns}"))
        m.add(BenchMetric("ffi.l1_overhead.ns", "${l1Ns - rawNs}（L1 的线程归属检查成本）"))
        m.add(BenchMetric("ffi.call_count", "${n}"))

        // ---- 事件回调延迟
        let evCount = ArrayList<Int64>()
        target.on(LvEventCode.Clicked) {
            _ => evCount.add(monotonicUs())
        }
        for (_ in 0..1000) {
            target.sendEvent(LvEventCode.Clicked)
        }
        let evN: Int64 = 10_000
        let evSamples = ArrayList<Int64>()
        for (_ in 0..evN) {
            evCount.clear()
            let t0 = monotonicUs()
            target.sendEvent(LvEventCode.Clicked)
            evSamples.add(evCount[0] - t0)
        }
        /*
         * ★ 单位务必看清：monotonicUs() 返回**微秒**，
         *   而指标名是 ns，因此 avg 与 max 都要 ×1000。
         *   曾经漏乘 max，产出「max=55 < avg=1913」这种自相矛盾的数据 ——
         *   这类错误在只看数字时很难发现，所以两条都写成同一形式并在此点明。
         */
        var evSum: Int64 = 0
        var evMax: Int64 = 0
        for (s in evSamples) {
            evSum += s
            if (s > evMax) {
                evMax = s
            }
        }
        m.add(BenchMetric("event.latency.avg_ns", "${evSum * 1000 / evN}"))
        m.add(BenchMetric("event.latency.max_ns", "${evMax * 1000}"))
    }

    /* 输入延迟：合成点击从「松手递送」到 Clicked 回调触发（口径见文件头） */
    private static func inputLatencyMetric(m: ArrayList<BenchMetric>, rt: LvglRuntime, disp: LvDisplay, scr: LvObject): Unit {
        let btn = LvButton.create(scr)
        btn.setPos(100, 80).setSize(120, 50)
        /* 先跑几帧让布局结算，否则几何为 0、命中测试必失败 */
        for (_ in 0..8) {
            rt.pump()
        }
        let feed = ArrayList<Int32>()
        feed.add(btn.x() + btn.width() / 2)
        feed.add(btn.y() + btn.height() / 2)
        feed.add(0)

        let fireUs = ArrayList<Int64>()
        btn.on(LvEventCode.Clicked) {
            _ => fireUs.add(monotonicUs())
        }
        let indev = LvIndev.create(LvIndevType.Pointer).bind(disp)
        indev.onRead {
            setIndevPoint(feed[0], feed[1], feed[2] != 0)
        }

        /*
         * ★ 帧间必须 sleep：indev 的读取由定时器驱动（周期约 30 ms），
         *   紧循环 pump 时 lv_timer_handler 看到时间没推进，定时器不到期，
         *   「按下」永远不会被读到（实测踩到过，表现为点击毫无反应而代码看不出问题）。
         */
        feed[2] = 1
        for (_ in 0..30) {
            rt.pump()
            sleep(Duration.millisecond * 5)
        }
        feed[2] = 0
        let tInput = monotonicUs()
        var waited = 0
        while (fireUs.size == 0 && waited < 200) {
            rt.pump()
            sleep(Duration.millisecond * 5)
            waited++
        }
        if (fireUs.size > 0) {
            m.add(BenchMetric("input.latency.ms", "${(fireUs[0] - tInput) / 1000}"))
        } else {
            m.add(BenchMetric("input.latency.ms", "skipped（合成点击未触发回调）"))
        }
        indev.close()
        btn.close()
    }

    /*
     * FPS 与启动时间。与上面的微基准分开两个运行时：
     * SDL2 后端要求「创建窗口的线程」与 LVGL 线程的关系符合其假设，
     * 与 headless 混在同一个运行时里会让线程归属判断变得难以推理。
     */
    private static func fpsAndStartup(m: ArrayList<BenchMetric>): Unit {
        let tStart = monotonicUs()
        let rt = LvglRuntime()
        rt.initLvgl()

        var backend: Option<Sdl2Backend> = None
        try {
            backend = Some(Sdl2Backend.start(800, 480))
        } catch (e: LvglException) {
            /* 无显示环境：明确标记 skipped，不填 0（「没测」与「是 0」必须可区分） */
            m.add(BenchMetric("fps.partial", "skipped（SDL2 不可用：${lvglErrorName(e.code)}）"))
            m.add(BenchMetric("fps.fullscreen", "skipped"))
            m.add(BenchMetric("startup.init_to_first_frame_ms", "skipped"))
            rt.deinitLvgl()
            return
        }
        let b = backend.getOrThrow()
        let scr = LvObject.screenActive()

        /* 约 50 个对象铺开，制造真实重绘面（§11.2 的测量条件） */
        let boxes = ArrayList<LvObject>()
        for (i in 0..50) {
            let o = LvObject.create(scr)
            o.setPos(Int32(i % 10) * 78, Int32(i / 10) * 90).setSize(70, 80)
            boxes.add(o)
        }
        let mover = boxes[0]
        /* 全屏对象：用于制造「整个屏幕失效」的场景 */
        let fullScreen = LvObject.create(scr)
        fullScreen.setPos(0, 0).setSize(800, 480)
        /* 全屏对象先藏起来，避免影响局部刷新场景的失效范围 */
        fullScreen.setSize(1, 1)
        fullScreen.setPos(0, 0)

        /* ---- 启动时间：initLvgl → 首帧完成 ---- */
        var spins = 0
        var gotFirst = false
        while (spins < 400 && !gotFirst) {
            let _ = b.pollEvents()
            rt.pump()
            if (b.flushCount() > 0) {
                gotFirst = true
            }
            spins++
        }
        m.add(BenchMetric("startup.init_to_first_frame_ms", "${(monotonicUs() - tStart) / 1000}"))
        m.add(BenchMetric("startup.frames_until_first", "${spins}"))

        /* ---- 局部刷新 FPS ---- */
        fpsPartial(m, b, rt, mover)

        /* ---- 全屏动画 FPS ---- */
        fpsFullscreen(m, b, rt, fullScreen)

        /* 收尾：先删子对象再删后端（顺序与 hello_cj 一致） */
        fullScreen.close()
        for (o in boxes) {
            o.close()
        }
        b.close()
        rt.deinitLvgl()
    }

    /*
     * 局部刷新 FPS：只动一个小对象（约 70×80），每帧只失效两条窄带。
     *
     * ★ 循环用**固定迭代数**驱动，而不是「每轮查一次时钟直到到时」。
     *   两个原因，都不是风格问题：
     *   1. 基准语义：固定工作量再量耗时，比固定时间再数工作量更可比
     *      （后者会让不同机器跑到不同的帧数）。时钟只在**分块之间**读。
     *   2. 每轮查时钟会把「时钟读取」本身算进被测路径 —— 早期实现里
     *      这一句曾让紧循环下的进程段错误（详见 src/clock.cj 对 TS_BUF 的说明）。
     *
     *   每块 2000 帧；跑满 3 秒（C 侧 FPS 是 1 秒滑窗，至少需要 2 个窗口）。
     */
    private static func fpsPartial(m: ArrayList<BenchMetric>, b: Sdl2Backend, rt: LvglRuntime, mover: LvObject): Unit {
        var i = 0
        let t0 = monotonicUs()
        while (true) {
            for (_ in 0..2000) {
                mover.setPos(10 + Int32(i % 30), 0)
                let _ = b.pollEvents()
                rt.pump()
                i++
            }
            if (monotonicUs() - t0 >= 3_000_000) {
                break
            }
        }
        /*
         * ★ 只报 FPS 与 CPU，**不报循环迭代数**。
         *   FPS 由 C 侧按「完成的刷新次数 / 秒」统计（debug.c），
         *   而循环迭代数是另一个量级的东西（3 秒里可能几十万次）——
         *   早期把迭代数标成 frames 输出，得到一个「frames=688000 而 fps=31」的
         *   自相矛盾的表，极易被误读为「渲染很快但 FPS 很低」。
         *   两者不是同一个概念，就不要放在同一个数字旁边。
         */
        let p = LvDebug.perfSample()
        m.add(
            BenchMetric("fps.partial", "${p.fps}（cpu=${p.cpuPercent}% refr=${p.refrTimeMs}ms draw=${p.drawTimeMs}ms）"))
    }

    /*
     * 全屏动画 FPS：把全屏对象放大回来并在两个远距位置往复。
     *
     * ★ 必须让**失效区域覆盖全屏**，否则这个场景名不副实：
     *   LVGL 只重绘「新旧位置的并集」。一个 800 宽的对象若只位移几个像素，
     *   并集只是两条细带（局部刷新），FPS 会虚高。
     *   因此放到 800×480 后在 x=0/100 之间跳 —— 并集覆盖 x=0..899，
     *   超出 800 的部分被裁剪，实际就是整屏。
     */
    private static func fpsFullscreen(m: ArrayList<BenchMetric>, b: Sdl2Backend, rt: LvglRuntime, obj: LvObject): Unit {
        obj.setSize(800, 480)
        var j = 0
        let t0 = monotonicUs()
        while (true) {
            for (_ in 0..2000) {
                obj.setPos(if (j % 2 == 0) {
                    0
                } else {
                    100
                }, 0)
                let _ = b.pollEvents()
                rt.pump()
                j++
            }
            if (monotonicUs() - t0 >= 3_000_000) {
                break
            }
        }
        let p = LvDebug.perfSample()
        m.add(
            BenchMetric("fps.fullscreen",
                "${p.fps}（cpu=${p.cpuPercent}% refr=${p.refrTimeMs}ms draw=${p.drawTimeMs}ms）"))
        m.add(BenchMetric("obj_count.sdl2", "${p.objCount}"))
    }

    // ==================================================== 压力负载

    /*
     * soak 需要一个**长期存活**的运行时与 display，因此把「建运行时」与「跑一轮」拆开：
     * 调用方每轮复用同一个 rt，只在结束时 deinit。
     * ★ 这一点是 soak 有效性的前提 —— 若每轮重建运行时，测到的就只是
     *   「每次从零开始」，而 §11.3 要看的恰恰是**长时间运行的漂移**。
     *   （因此这里**不提供**「一行跑一轮」的便捷接口：
     *     那种接口会诱导调用方每轮新建运行时，把判据悄悄测废。）
     */
    public static func soakRuntime(): LvglRuntime {
        let rt = LvglRuntime()
        rt.initLvgl()
        let _ = rt.startHeadless(320, 240)
        return rt
    }

    public static func soaked(rt: LvglRuntime, round: Int64, payload: Int64): SoakStat {
        soakBody(rt, round, payload)
    }

    private static func soakBody(rt: LvglRuntime, round: Int64, payload: Int64): SoakStat {
        let scr = LvObject.screenActive()

        /* ---- 1. 建容器 + 控件子树（深度 2，模拟真实布局） ---- */
        let box = LvObject.create(scr)
        box.setPos(10, 10).setSize(200, 120)

        /* ---- 2. 事件回调 + 删除自身后重建（§12 的压力形态） ---- */
        let clicks = ArrayList<Int64>()
        let btn = LvButton.create(box)
        btn.setPos(20, 20).setSize(80, 40)
        btn.on(LvEventCode.Clicked) {
            e => clicks.add(e.target())
        }
        btn.sendEvent(LvEventCode.Clicked)
        if (clicks.size > 0) {
            /* 回调里不删自身（那属于延迟删除语义，已由 p0 断言覆盖），
               这里只做「删除后重建」的往复，保持负载形态可控 */
            btn.close()
            let btn2 = LvButton.create(box)
            btn2.setPos(20, 20).setSize(80, 40)
        }

        /* ---- 3. 动画：跑到自然结束，验证 anim_ctx 被回收 ---- */
        let anim = LvAnim.create().target(box).values(0, 20).duration(30).repeat(1).onExec {_ => ()}
        anim.start()
        /* 推几帧让它自然结束（30ms 动画；每帧 sleep 5ms 即可越过） */
        for (_ in 0..12) {
            rt.pump()
            sleep(Duration.millisecond * 5)
        }

        /* ---- 4. 负载：按 payload 决定本轮要不要多建几层子树 ---- */
        let extra = ArrayList<LvObject>()
        for (i in 0..payload) {
            let p = LvObject.create(scr)
            p.setPos(Int32(i % 20) * 10, Int32(i / 20) * 10).setSize(8, 8)
            extra.add(p)
        }

        /* ---- 5. dump 一次对象树 ----
         * 每轮都做：C 侧的 dump 会 malloc 数组（由 free_tree_dump 释放），
         * 高频调用才能把「分配/释放没配对」这类泄漏压出来。
         * 树规模只有几十个节点，开销可忽略。 */
        let nodes = LvDebug.dumpTree(root: scr, maxDepth: 4)

        /* ---- 6. 清理：先子后父（级联失效由父删除驱动） ---- */
        for (p in extra) {
            p.close()
        }
        box.close()

        /* ---- 7. 快照 ---- */
        SoakStat(round, handleCount(HandleState.Alive), LvDebug.objCount(), closureCount(), animCtxCount(),
            handleCount(HandleState.Invalidated), Int32(nodes.size))
    }

    // ==================================================== 随机序列（§11.4 模糊测试）

    /*
     * 随机操作序列。执行 steps 步随机操作后**全部清空**，返回清空后的稳态计数。
     *
     * 【为什么必须由仓颉侧做，而不是复用 C 侧的同名能力】
     *   C 层的 test_soak_asan --fuzz 覆盖不到回调链路：closure_id 由仓颉侧分配
     *   （callback.h 明确约定），纯 C 进程里没有任何闭包，
     *   因此 obj_add_event / anim_set_exec_cb 都驱动不了。
     *   而**闭包表**恰恰是跨语言最容易泄漏的地方 ——
     *   「C 侧 ctx 释放了、仓颉侧闭包还留在表里」这类泄漏只能在这里测出来。
     *
     * 【每个随机操作自成一次完整生命周期】
     *   每步都以「建 → 用 → 全拆」收尾，因此**每一步结束后都是稳态**，
     *   调用方可以直接对账。若让对象在池里逐步堆积，计数会随步数增长，
     *   「漂移」与「还没清」就分不清了 —— 而本函数要抓的正是「清了但没回收」。
     *
     * 【为什么要覆盖负向路径（撞引用守卫）】
     *   只测「顺着来」会漏掉一整类缺陷：守卫从不被触发，就永远不知道它在
     *   被触发时是否正确。t8 的样式 UAF 正是靠守卫拦下的。
     *
     * @param seed  随机种子。同一 seed 必然复现同一序列（失败可精确重放）。
     * @param steps 本次执行的随机操作步数；长跑由调用方循环调用，每轮都会清空。
     */
    public static func fuzzed(rt: LvglRuntime, seed: Int64, steps: Int64): SoakStat {
        let scr = LvObject.screenActive()
        var s = if (seed == 0) {
            88172645463325252
        } else {
            seed
        }
        var done: Int64 = 0

        while (done < steps) {
            let op = fuzzIn(s, 0, 4)
            s = fuzzNext(s)
            s = fuzzOneOp(rt, scr, op, s)
            done++
        }

        /* 收尾：推帧让延迟删除结算，再取稳态快照 */
        rt.pump()
        SoakStat(0, handleCount(HandleState.Alive), LvDebug.objCount(), closureCount(), animCtxCount(),
            handleCount(HandleState.Invalidated), 0)
    }

    /*
     * 派发一个随机操作，返回推进后的随机状态。
     * 拆出来的理由与 perfMetrics 相同（单函数 > 50 行会被 cjlint 判过长），
     * 而且每类操作的注释都值得独立成段。
     */
    private static func fuzzOneOp(rt: LvglRuntime, scr: LvObject, op: Int64, s0: Int64): Int64 {
        if (op == 0) {
            return fuzzOpEvent(scr, s0)
        }
        if (op == 1) {
            return fuzzOpStyle(scr, s0)
        }
        if (op == 2) {
            return fuzzOpAnimRun(rt, scr, s0)
        }
        if (op == 3) {
            return fuzzOpAnimDrop(scr, s0)
        }
        fuzzOpCascade(rt, scr, s0)
    }

    /* op0：建树 → 发事件（走完整「派发 → 闭包调用」）→ 删容器（级联注销子树闭包） */
    private static func fuzzOpEvent(scr: LvObject, s0: Int64): Int64 {
        var s = fuzzNext(s0)
        let box = fuzzTree(scr, fuzzIn(s, 1, 4))
        s = fuzzNext(s)
        box.sendEvent(LvEventCode.Clicked)
        box.close()
        s
    }

    /*
     * op1：样式往返 —— 两个 selector 绑定、解绑其一、**撞引用守卫**、删容器、再释放。
     *
     * 「撞守卫」这一步不能省：只测"无人引用时可释放"会完全错过 UAF 会发生的那条路
     * （守卫从不被触发的用例，永远不知道它被触发时是否正确）。
     */
    private static func fuzzOpStyle(scr: LvObject, s0: Int64): Int64 {
        var s = fuzzNext(s0)
        let box = fuzzTree(scr, fuzzIn(s, 1, 3))
        let st = LvStyle.create()
        s = fuzzNext(s)
        let _ = st.radius(Int32(fuzzIn(s, 0, 40)))
        s = fuzzNext(s)
        box.addStyle(st)
        box.removeStyle(st, selector: LV_PART_MAIN | LV_STATE_PRESSED)
        try {
            /* 此处 st 仍被 box（MAIN 那份）引用，释放**必须被拒**：
               若放行，box 的样式链表里会留下悬空指针 —— 删 box 时即 UAF */
            st.close()
        } catch (e: LvglException) {
            () /* 期望路径：守卫拒绝。这里不做动作是对的，不是"吞异常" */
        }
        box.close()
        /* 容器已删、引用已解除，这次应当成功 */
        st.close()
        s
    }

    /* op2：动画跑到自然结束（internal deleted_cb 释放 anim_ctx） */
    private static func fuzzOpAnimRun(rt: LvglRuntime, scr: LvObject, s0: Int64): Int64 {
        var s = fuzzNext(s0)
        let box = fuzzTree(scr, fuzzIn(s, 1, 3))
        s = fuzzNext(s)
        let anim = LvAnim.create().target(box).values(0, 20).duration(20).repeat(1).onExec {_ => ()}
        anim.start()
        for (_ in 0..10) {
            rt.pump()
            sleep(Duration.millisecond * 5)
        }
        box.close()
        s
    }

    /* op3：动画在 **start 之前**删除 —— 一条不走 deleted_cb 的路径，
       必须由 delete 自己释放 ctx，否则每步稳定泄漏一个 anim_ctx */
    private static func fuzzOpAnimDrop(scr: LvObject, s0: Int64): Int64 {
        var s = fuzzNext(s0)
        let box = fuzzTree(scr, fuzzIn(s, 1, 3))
        s = fuzzNext(s)
        let anim = LvAnim.create().target(box).values(0, 50).duration(50)
        anim.close()
        box.close()
        s
    }

    /* op4：纯级联 + 推进帧（无样式、无动画、无回调）——
       它是"漂移与样式/动画/闭包无关"这个结论的关键对照组 */
    private static func fuzzOpCascade(rt: LvglRuntime, scr: LvObject, s0: Int64): Int64 {
        var s = fuzzNext(s0)
        let box = fuzzTree(scr, fuzzIn(s, 1, 4))
        s = fuzzNext(s)
        box.close()
        rt.pump()
        s
    }

    /*
     * 建一棵小树：容器 + k 个子对象，**每个都挂一个 Clicked 回调**。
     *
     * 子对象**不返回给调用方**：它们随容器级联消失，把句柄交出去会变成悬空句柄。
     * 而级联本身正是要覆盖的路径 —— 子对象的闭包必须被 DELETE 钩子逐个注销，
     * 否则闭包表会随「建容器→删容器」的循环单调增长。
     */
    private static func fuzzTree(scr: LvObject, k: Int64): LvObject {
        let box = LvObject.create(scr)
        box.setPos(0, 0).setSize(40, 40)
        box.on(LvEventCode.Clicked) {_ => ()}
        for (i in 0..k) {
            if (i % 3 == 0) {
                let l = LvLabel.create(box)
                /* ASCII 文本：默认字体没有中文字形，写中文会让每帧刷缺字警告，
                   把本用例的输出淹掉；这里测的是生命周期，不是文本渲染 */
                let _ = l.setText("ok")
                l.on(LvEventCode.Clicked) {_ => ()}
            } else {
                let c = LvObject.create(box)
                c.on(LvEventCode.Clicked) {_ => ()}
            }
        }
        box
    }
}

/* ---------------------------------------------------- fuzz 用的极简随机数 */

/*
 * xorshift64。自带实现而不是用库随机数：随机序列测试要求「同 seed 必然复现」，
 * 而库随机数的实现可能随 SDK 版本变化 —— 那会让"复现某次失败"这件事失效。
 */
private func fuzzNext(s: Int64): Int64 {
    var x = s
    x ^= x << 13
    x ^= x >> 7
    x ^= x << 17
    x
}

/* 返回 [lo, hi] 闭区间内的整数（负数取绝对值，避免算术右移导致的负模） */
private func fuzzIn(s: Int64, lo: Int64, hi: Int64): Int64 {
    let span = hi - lo + 1
    if (span <= 1) {
        return lo
    }
    let v = fuzzNext(s) % span
    lo + (if (v < 0) {
        -v
    } else {
        v
    })
}
