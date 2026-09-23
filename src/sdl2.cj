/*
 * sdl2.cj —— SDL2 桌面后端的 L1 包装（设计文档 §6.6 / §8）
 *
 * ===================== L1 在这里做什么 =====================
 * 只做两件事：
 *   1. 把裸 C 调用包成有类型、有异常的接口（错误码 → 异常的统一翻译）
 *   2. 明确**不暴露**任何像素缓冲入口 —— 后端在 C 侧处理缓冲，
 *      仓颉侧只表达「何时推进一帧、何时该退出」
 *
 * 这是刻意的取舍：L1 越薄，「headless 验证过」与「窗口下也对」之间的
 * 差异面就越小。flush 的实现在 C 侧（sink），仓颉侧连回调都不需要注册。
 *
 * ===================== 线程关系（最容易搞错的一点）=====================
 * SDL 的事件循环必须跑在**创建窗口的线程**，因此：
 *   · start() 必须由「将要跑事件循环的那个线程」调用
 *   · 事件循环（pollEvents）与 LVGL 主循环（runtime 的 startThreaded）分别在两条线程上
 *   · 若用方案 C（单线程，测试常用），flush 会自动走内联渲染分支，
 *     不会出现「投递请求又自己等」的自锁 —— 这一点在 C 侧 backend 内处理，
 *     仓颉侧不需要也不应该关心
 */
package lvgl4cj

public class Sdl2Backend {
    private var inited = false

    private init() {}

    /*
     * 启动后端：建窗口/渲染器/纹理，并创建 display（缓冲由 C 侧持有）。
     *
     * ★ 必须由将要跑事件循环的线程调用 —— 该线程会被登记为渲染线程
     *   （既用于单线程 flush 分流，也用于 §3.8.4 的等待图判据 3）。
     *
     * bufLines <= 0 表示由桥接层按 PARTIAL 的 1/10 屏下限自动选取。
     */
    public static func start(width: Int32, height: Int32, colorFormat!: Int32 = LV_COLOR_FORMAT_RGB565,
        bufLines!: Int32 = 0): Sdl2Backend {
        throwIfError(unsafe { lvglcj_sdl2_init(width, height, colorFormat, bufLines) },
            "lvglcj_sdl2_init（宽=${width} 高=${height} 格式=${colorFormat}）；" +
                "无显示环境可设 SDL_VIDEODRIVER=dummy")

        let b = Sdl2Backend()
        b.inited = true
        return b
    }

    /// 后端创建的 display；未启动时句柄为 0
    public func display(): LvDisplay {
        LvDisplay.wrap(unsafe { lvglcj_sdl2_display() })
    }

    /**
     * 处理一轮 SDL 事件（渲染请求、输入、窗口事件）。
     * 返回 true 表示收到退出请求（窗口关闭 / SDL_QUIT），调用方应结束循环。
     *
     * 调用方应在循环里持续调用它 —— 它就是 SDL 事件循环本身。
     *
     * @throws[LvglException] 异常类型与 C 侧返回码对应（见 lvglErrorFromCode）；
     *         SDL 侧失败通常是 BackendFailure。
     */
    public func pollEvents(): Bool {
        let rc = unsafe { lvglcj_sdl2_poll_events() }
        if (rc < 0) {
            throw LvglException(lvglErrorFromCode(rc), "lvglcj_sdl2_poll_events")
        }
        return rc == 1
    }

    /*
     * 把 SDL 采集到的输入写进当前 indev 读取上下文。
     * 只能在 indev_read 回调内调用，否则返回 NotSupported（T5 的约束）。
     *
     * 有了它，仓颉侧的读回调只需一行：{ _ => backend.feedIndev() }
     */
    public func feedIndev(): Unit {
        unsafe { lvglcj_sdl2_feed_indev() }
    }

    // ---------------------------------------------------------- 观测

    /// flush 回调被调用次数
    public func flushCount(): Int32 {
        unsafe { lvglcj_sdl2_flush_count() }
    }

    /// 画面真正被 Present 的次数
    public func presentCount(): Int32 {
        unsafe { lvglcj_sdl2_present_count() }
    }

    /// 有界等待超时次数。正常长跑中应**恒为 0**；非 0 说明渲染线程跟不上
    public func waitTimeoutCount(): Int32 {
        unsafe { lvglcj_sdl2_wait_timeout_count() }
    }

    /**
     * 渲染事件与暂存数据序号不匹配的次数。正常应**恒为 0**。
     *
     * 非 0 的含义很具体：某次 flush 的等待被提前满足了（等到的是**上一次**留下的额度），
     * 于是它覆盖的条带没被渲染、也从此不会再被重画 —— 画面上表现为残留旧像素
     * （滚动之后出现"重影"：同一控件出现在多个位置）。
     * 后端已确保这种不匹配**不会画错位置**（只按暂存区自带的矩形应用），
     * 但计数增长仍说明协议漏过一次，值得追。
     */
    public func staleApplyCount(): Int32 {
        unsafe { lvglcj_sdl2_stale_apply_count() }
    }

    /// 窗口是否已关闭/最小化（此时后端暂停渲染并立即放行等待方）
    public func isPaused(): Bool {
        unsafe { lvglcj_sdl2_is_paused() } != 0
    }

    /// 当前线程是否为渲染线程（即创建窗口的那个线程）
    public func isRenderThread(): Bool {
        unsafe { lvglcj_sdl2_is_render_thread() } != 0
    }

    /*
     * 注入退出请求（测试用）。
     * 它模拟「窗口被关闭」这一步，用于验证等待方会立即返回而不是等超时。
     */
    public func injectQuit(): Unit {
        unsafe { lvglcj_sdl2_inject_quit() }
    }

    /*
     * 关闭后端。
     *
     * 不抛异常而是记录日志，理由与 LvglRuntime.stop 相同：
     * 关闭路径抛异常会让「已经停了一半」的状态更难收拾。
     * 幂等 —— 重复调用无副作用。
     */
    public func close(): Unit {
        if (!inited) {
            return
        }
        inited = false
        let rc = unsafe { lvglcj_sdl2_deinit() }
        if (rc != 0) {
            eprintln("[lvgl4cj] SDL2 后端反初始化返回 ${lvglErrorName(lvglErrorFromCode(rc))}(${rc})")
        }
    }
}
