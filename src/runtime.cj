/*
 * runtime.cj —— L1 运行时入口（设计文档 §6.1 / §3.6 / §3.9）
 *
 * ===================== 线程模型：为什么是「双轨」 =====================
 * LVGL 非线程安全，所有 lv_* 调用必须落在**同一个 OS 线程**。
 * 而仓颉的 spawn 是 M:N 轻量级线程，V3 探针实测会在同一段逻辑里落到不同
 * OS 线程上（2000 次迭代观察到 2 个不同 TID）—— 因此**不能**用 spawn 来
 * 保证 LVGL 线程亲和性。
 *
 * 所以主循环只有两条可选路径，两者**共用同一套任务队列与回调抽象**：
 *   · RunMode.Threaded（方案 A）：C 侧 pthread_create 出独立 OS 线程跑主循环。
 *     V1 探针实测「该线程能安全执行仓颉闭包」（2000/2000 成功），
 *     因此 ADR-013 归档为方案 A，也是 P0 的默认。
 *   · RunMode.Pumped（方案 C）：不启线程，由调用方按帧驱动 pump()。
 *     单测与 headless 场景用它来**确定性推进** —— 避免后台线程在断言之间
 *     偷偷多跑几帧造成 flaky。
 *
 * 两条路径的**帧内顺序完全一致**（延迟删除 → 任务队列 → lv_timer_handler），
 * 这一点由 C 侧 thread.c 保证，因此「pumped 下通过」对「threaded 下也通过」
 * 具有参考价值，而不是两套彼此无关的时序。
 *
 * ===================== 跨线程规则（§6.7） =====================
 * 从非 LVGL 线程直接调用 lv_* 是错的，必须经 post/postAndWait 投递。
 * 本类的 post 系列不代用户判断线程，而是把判断留给 C 侧 ——
 * 因为「哪个线程是 LVGL 线程」在方案 A 下只有 C 侧知道（主循环线程自登记）。
 */
package lvgl4cj

import std.sync.Mutex
import std.deriving.Derive
/* CONF_HASH 等常量由 gen_conf_const.py 生成在子包 lvgl4cj.generated 中 */
import lvgl4cj.generated.{CONF_HASH}

/// 主循环驱动方式（ADR-013 实测归档为 Threaded）
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum RunMode {
    | Threaded // 方案 A：C 侧独立 OS 线程
    | Pumped // 方案 C：调用方驱动
}

/*
 * 全局运行模式标记（供 L1 各处判断是否需要重新确认线程归属）。
 *
 * 为什么需要全局量：RunMode 存在 LvglRuntime 实例上，而 L1 的对象/控件/样式
 * 构造与关闭路径并不知道自己属于哪个 runtime 实例 —— 同一个进程里也不该出现
 * 两个驱动模式并存的 runtime。用一个进程级标记表达「当前是泵模式」最直接。
 */
var gPumpedMode: Bool = false // cjlint-ignore !G.VAR.02 见上方说明：L1 各入口不知道自己属于哪个 runtime 实例，进程级标记是最直接的表达

/*
 * 泵模式下重新确认「当前线程即 LVGL 线程」。
 *
 * ★ 为什么所有 L1 入口都需要它（而不是只在 pump 里做）：
 *   仓颉是 M:N 轻量级线程，V3 探针实测同一条逻辑会落到**不同 OS 线程**上。
 *   用户程序的逻辑主循环没变，但底层 OS 线程可能在任意一次阻塞调用（sleep、
 *   阻塞读…）之后就被换掉了 —— **用户无法预知这件事发生在哪一行**。
 *   因此「归属确认」必须由库来兜底，不能推给用户。
 *
 *   实测症状：断言⑥用例跑了约 0.4s（循环里多次 sleep）后，在收尾的
 *   `indev.close()` 上偶发 WrongThread；而界面逻辑本身完全正常。
 *   加在 pump 上之后仍能偶发（因为收尾操作不在 pump 里），故扩大到
 *   创建/关闭这类**必然会跨越 sleep**的路径。
 *
 * 安全性：泵模式下不存在第二个 LVGL 使用者（多线程模型是方案 A），
 *   所以重新确认归属不会掩盖真实的跨线程竞争 ——
 *   从**别的**线程直接调 lv_* 仍会被断言拦下（断言拦的是「非所有者」，
 *   不是「换了个人来当所有者」）。
 */
func ensureLvglOwnership(): Unit {
    if (gPumpedMode) {
        unsafe { lvglcj_rebind_current_thread() }
    }
}

public class LvglRuntime {
    private let lifecycleMutex = Mutex()

    private var inited = false
    private var running = false
    private var mode = RunMode.Pumped
    private var headless = false

    public init() {}

    /// 当前驱动方式（未启动时为 Pumped，仅表示默认值）
    public prop currentMode: RunMode {
        get() {
            mode
        }
    }

    public prop isRunning: Bool {
        get() {
            running
        }
    }

    // ------------------------------------------------------------ 初始化

    /// 仅初始化 LVGL（幂等）。不决定线程模型，也不建 display。
    public func initLvgl(): Unit {
        synchronized(lifecycleMutex) {
            if (inited) {
                return
            }
            throwIfError(unsafe { lvglcj_init() }, "lvglcj_init")

            /*
             * 配置一致性校验（§3.5.3）必须在**任何其他调用之前**完成：
             * 哈希不匹配意味着「C 库编译时用的 lv_conf.h」与「仓颉侧由
             * gen_conf_const.py 读到的常量」不是同一份配置。此时继续运行会
             * 出现各种无法解释的行为差异（内存池大小、颜色深度、渲染开关），
             * 在这里拦住远比事后排查便宜。
             */
            verifyConfigCompat()

            throwIfError(unsafe { lvglcj_set_tick_cb() }, "lvglcj_set_tick_cb")
            /* 安装分发器：注册闭包时会自动幂等安装，这里显式装一次让失败尽早暴露 */
            throwIfError(installClosureDispatcher(), "安装闭包分发器（lvglcj_set_dispatch）")

            inited = true
        }
    }

    /**
     * 校验仓颉侧编译期常量与 C 库实际配置是否一致（§3.5 配置一致性）。
     *
     * 「仓颉侧常量」由 gen_conf_const.py 从 lv_conf.h 生成，两边可能因
     * 「改了配置但只重建了一侧」而失配 —— 那种失配不会报错，只会让
     * 结构体布局或枚举取值对不上，表现为随机的内存错乱。因此启动时强校验。
     *
     * @throws[LvglException] 类型为 InvalidConfig —— 哈希不一致，要求整体重建。
     */
    private func verifyConfigCompat(): Unit {
        let live = unsafe { lvglcj_conf_hash() }
        if (live != CONF_HASH) {
            throw LvglException(InvalidConfig,
                "lv_conf 哈希不匹配：库=${live}，仓颉侧=${CONF_HASH}。" +
                    "请重新执行 scripts/gen_lv_conf.sh 与 scripts/gen_conf_const.py，并整体重建原生库与仓颉工程")
        }
    }

    // ------------------------------------------------------------ 启动

    /// 方案 A：启动 C 侧主循环线程。P0 默认路径（V1 探针已实证可行）。
    public func startThreaded(): Unit {
        initLvgl()
        synchronized(lifecycleMutex) {
            throwIfError(unsafe { lvglcj_start_thread() }, "lvglcj_start_thread")
            mode = RunMode.Threaded
            /* 方案 A：主循环线程自己登记，L1 不应代为重绑 */
            gPumpedMode = false
            running = true
        }
    }

    /*
     * 方案 C：不启线程，由**调用方线程**驱动（pump / runForever）。
     *
     * 之所以要显式 rebind：lvglcj_init 是幂等的，重复调用**不会**重新登记
     * 线程归属。而仓颉是 M:N 轻量级线程，V3 探针实测同一段逻辑会落到不同
     * OS 线程上 —— 于是「第二次 startPumped 的调用线程」很可能不是
     * 「第一次登记的那个 LVGL 线程」，此后所有调用都会被线程断言拒绝。
     * 方案 C 的语义本来就是「谁驱动谁就是 LVGL 线程」，因此这里必须重新登记。
     *
     * 方案 A 下**不能**这么做：主循环线程会自己登记，随意重绑会破坏线程断言。
     */
    public func startPumped(): Unit {
        initLvgl()
        synchronized(lifecycleMutex) {
            throwIfError(unsafe { lvglcj_rebind_current_thread() },
                "lvglcj_rebind_current_thread（方案 C 声明当前线程为 LVGL 线程）")
            mode = RunMode.Pumped
            gPumpedMode = true
            running = true
        }
    }

    /*
     * headless 启动（单测 / CI / 无窗口环境）。
     *
     * ★ 必须先建 display —— 这是探针结论 G1 的硬前置：
     *   无 display 时 lv_screen_active() 返回 NULL，而 lv_obj_create(NULL)
     *   会触发 LVGL 断言并**直接终止进程**。
     *   因此「headless 也要先有 display」不是风格问题，而是能否建对象的前提。
     *
     * 不启动线程：headless 场景需要确定性推进帧，由调用方用 pumpFrames 驱动。
     */
    public func startHeadless(width: Int32, height: Int32, colorFormat!: Int32 = LV_COLOR_FORMAT_RGB565,
        bufLines!: Int32 = 0): LvDisplay {
        initLvgl()
        synchronized(lifecycleMutex) {
            /* 同 startPumped：headless 也是「调用方驱动」，必须重新登记线程归属 */
            throwIfError(unsafe { lvglcj_rebind_current_thread() }, "lvglcj_rebind_current_thread")
            throwIfError(unsafe { lvglcj_null_init(width, height, colorFormat, bufLines) }, "lvglcj_null_init")
            headless = true
            mode = RunMode.Pumped
            gPumpedMode = true
            running = true
        }
        return LvDisplay.wrap(unsafe { lvglcj_null_display() })
    }

    // ------------------------------------------------------------ 帧驱动

    /*
     * 泵模式下重新确认「当前线程就是 LVGL 线程」。
     *
     * ★ 为什么每次 pump 都要确认：
     *   方案 C 的定义就是「谁驱动 pump，谁就是 LVGL 线程」。而仓颉是 M:N
     *   轻量级线程，V3 探针实测同一条逻辑会落到**不同 OS 线程**上 ——
     *   于是「startPumped 时登记的那个线程」与「此刻正在 pump 的线程」可能不同，
     *   此后所有 LVGL 调用都会被线程断言拒绝（WrongThread）。
     *
     *   实测症状：断言⑥用例跑了约 0.4s（含多次 sleep）后，在收尾的
     *   `indev.close()` 上偶发 WrongThread —— 而界面逻辑本身完全正常。
     *   若不在这里重新确认，**任何**使用泵模式的仓颉程序都会在某个不确定的
     *   时刻随机报 WrongThread，这类不确定性是没法交给用户去规避的。
     *
     * 安全性：泵模式下不存在第二个 LVGL 使用者（多线程模型是方案 A），
     *   因此重新确认归属不会掩盖真实的跨线程竞争；从**别的**线程直接调 lv_*
     *   仍会被断言拦下 —— 断言拦的是「非所有者」，不是「换了人」。
     */
    private func rebindIfPumped(): Int32 {
        if (mode == RunMode.Pumped) {
            return unsafe { lvglcj_rebind_current_thread() }
        }
        return 0
    }

    /// 处理一帧：延迟删除 → 任务队列 → 定时器。返回错误码。
    public func pump(): Int32 {
        let rrc = rebindIfPumped()
        if (rrc != 0) {
            return rrc
        }
        unsafe { lvglcj_pump() }
    }

    /// 确定性推进 n 帧（仅 headless 后端提供；每帧带帧间隔让刷新周期到期）
    public func pumpFrames(frames: Int32): Int32 {
        /* 同一个理由：推进期间也可能被调度器迁移，先确认归属再进 C 侧循环 */
        let rrc = rebindIfPumped()
        if (rrc != 0) {
            return rrc
        }
        unsafe { lvglcj_null_pump(frames) }
    }

    /**
     * 方案 C 的主循环。方案 A 下**不应调用** ——
     * 那时主循环已由 C 侧线程承担，再跑一个会在两个线程上同时驱动 LVGL。
     *
     * @throws[LvglException] 类型为 WrongThread —— 方案 A 下调用；
     *         以及主循环体内 pump() 可能返回的其他错误。
     */
    public func runForever(frameIntervalMs!: Int32 = 5): Unit {
        if (mode == RunMode.Threaded) {
            throw LvglException(WrongThread,
                "方案 A（startThreaded）下主循环由 C 侧线程驱动，不应再调用 runForever；" +
                    "若确实想自己驱动，请改用 startPumped")
        }
        while (running) {
            let rc = pump()
            if (rc != 0) {
                /* 单帧失败不终止循环：把错误上报后继续，避免一次抖动就停掉界面 */
                eprintln("[lvgl4cj] pump 失败：${lvglErrorName(lvglErrorFromCode(rc))}(${rc})")
            }
            sleep(Duration.millisecond * Int64(frameIntervalMs))
        }
    }

    // ------------------------------------------------------------ 跨线程投递

    /*
     * 投递一段代码到 LVGL 线程执行（异步，不等待）。失败抛异常。
     *
     * ★ 使用注意（仓颉语言约束，实测踩到过）：
     *   本方法的闭包是**逃逸**的（要被存进队列、稍后在另一帧执行），
     *   而仓颉不允许「捕获**可变局部变量**的 lambda」逃逸 ——
     *   写 `var n = 0; rt.post { n = n + 1 }` 会编译失败：
     *     "lambda capturing mutable variables needs to be called directly"
     *
     *   正确写法是让它捕获一个 **let 绑定的可变容器**，或捕获对象字段：
     *     let n = ArrayList<Int32>()          // let 绑定，内容可变
     *     rt.post { n.add(1) }                // ✓ 捕获 n 这个不可变绑定
     *
     *   这不是本封装的限制，而是语言层面的规则；文档里写清楚比让用户
     *   在编译错误里猜要省事得多。
     */
    public func post(body: () -> Unit): Unit {
        throwIfError(submitTask(body), "post（投递任务到 LVGL 线程）")
    }

    /// 同 post，但返回错误码而不抛 —— 供不想处理异常的调用点使用
    public func tryPost(body: () -> Unit): Int32 {
        submitTask(body)
    }

    /*
     * 投递并同步等待执行完成。仅可在**非 LVGL 线程**调用。
     * 在 LVGL 线程或回调内调用会立刻返回 DeadlockRisk（而不是挂死）——
     * 这是 C 侧等待图检测的价值所在（§3.8.4）。
     */
    public func postAndWait(body: () -> Unit): Unit {
        let rc = submitTaskAndWait(body)
        throwIfError(rc, "postAndWait（同步等待任务执行）")
    }

    public func tryPostAndWait(body: () -> Unit): Int32 {
        submitTaskAndWait(body)
    }

    /*
     * 异步投递，返回 Future<Int32> 表示「本次投递+执行的最终结果」。
     * 实现方式是在仓颉轻量级线程上做等待 —— 因此它**不保证**在 LVGL 线程上
     * 等待，也不会阻塞调用方线程。注意它同样不能在 LVGL 线程调用
     * （内部仍走 postAndWait 的等待图检测，会得到 DeadlockRisk）。
     */
    public func postAsync(body: () -> Unit): Future<Int32> {
        spawn {
            => submitTaskAndWait(body)
        }
    }

    // ------------------------------------------------------------ 关闭

    /*
     * 停止主循环并按策略处置队列。
     *
     * 这里**不抛异常**而是记录日志：关闭路径抛异常会让「已经停了一半」的状态
     * 更难收拾。调用方若要感知失败，看日志或改用 deinitLvgl 的返回值。
     */
    public func stop(shutdownMode!: ShutdownMode = ShutdownMode.Drain): Unit {
        synchronized(lifecycleMutex) {
            if (!inited) {
                return
            }
            running = false
            let rc = unsafe { lvglcj_stop_thread(shutdownModeToCode(shutdownMode)) }
            if (rc != 0) {
                eprintln("[lvgl4cj] stop 返回 ${lvglErrorName(lvglErrorFromCode(rc))}(${rc})")
            }

            /*
             * ★ 主循环停掉后，把 LVGL 线程身份**交回调用方线程**。
             *
             * 为什么必须这么做：方案 A 下 startThreaded() 把 LVGL 线程身份转移给了
             * C 侧主循环线程。该线程一旦 join，就再没有线程持有这个身份 ——
             * 于是此后**任何**清理动作（删 display、删对象、lvglcj_deinit）
             * 都会被线程断言拒绝，报 WrongThread。
             * 实测症状：hello_cj 退出时打印
             *   「display 删除返回非成功」——
             * 界面看起来正常，但 display 与绘制缓冲根本没被释放。
             *
             * 安全性：此刻主循环已 join，不存在并发访问 LVGL 的线程，
             * 因此把身份交给调用方是安全的（语义等价于方案 C 的「谁驱动谁是 LVGL 线程」）。
             */
            throwIfError(unsafe { lvglcj_rebind_current_thread() },
                "lvglcj_rebind_current_thread（主循环已停，接管清理）")
        }
    }

    /*
     * 完整反初始化。
     * 顺序固定：先停主循环（否则清理会与 LVGL 线程并发）→ 删 headless display
     * → lvglcj_deinit（内部再做泄漏自检）。
     */
    public func deinitLvgl(shutdownMode!: ShutdownMode = ShutdownMode.Drain): Unit {
        synchronized(lifecycleMutex) {
            if (!inited) {
                return
            }
            /* 按名传参：shutdownMode 是命名参数，显式写出更清晰 */
            stop(shutdownMode: shutdownMode)

            if (headless) {
                let drc = unsafe { lvglcj_null_deinit() }
                if (drc != 0) {
                    eprintln("[lvgl4cj] null_deinit 返回 ${drc}")
                }
                headless = false
            }

            let rc = unsafe { lvglcj_deinit() }
            if (rc != 0) {
                eprintln("[lvgl4cj] deinit 返回 ${lvglErrorName(lvglErrorFromCode(rc))}(${rc})")
            }
            inited = false
        }
    }

    // ------------------------------------------------------------ 查询

    /// 当前线程是否为 LVGL 线程（方案 A 下由 C 侧主循环线程登记）
    public func isLvglThread(): Bool {
        unsafe { lvglcj_is_lvgl_thread() } != 0
    }

    /// 当前 OS 线程号（诊断用）
    public func currentOsTid(): Int64 {
        unsafe { lvglcj_current_os_tid() }
    }

    /// 活动屏幕句柄；未建 display 时为 0（此时**不可**创建对象，见 G1）
    public func activeScreen(): Int64 {
        unsafe { lvglcj_screen_active() }
    }

    /// 最近一次定时器处理报告的下次触发间隔（ms），可用于自适应休眠
    public func timerNextMs(): Int32 {
        unsafe { lvglcj_timer_next_ms() }
    }
}
