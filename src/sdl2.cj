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
     * 渲染事件序号与"最新暂存序号"不匹配的次数。
     *
     * ★ 含义要说准（这一点是被实测纠正的）：它**只说明生产者跑在消费者前面**。
     *   在慢消费者上（例如 KMSDRM 上昂贵的 Present）出现是**正常**的，
     *   **不等于画错** —— 事件自带槽号与矩形，贴上去的仍是它自己那块数据。
     *   真正"丢数据"要看 {@link slotClobberCount}。
     */
    public func staleApplyCount(): Int32 {
        unsafe { lvglcj_sdl2_stale_apply_count() }
    }

    /**
     * **真正丢数据**的次数：取到的暂存槽还没被应用，就又被写了一次。
     * 长期必须为 **0**。
     *
     * 为什么它与 staleApply 必须分开：修复前暂存区只有一块，每次等待超时都必然
     * 覆盖还没被消费的数据 —— 在 KMSDRM 上表现为渲染条带高度整数倍的**纯黑带**
     * （实测它与 waitTimeout 同步增长；换 dummy 驱动（Present 免费）则两者归零、
     * 黑带消失）。现在暂存区是多槽轮转，"超时"在结构上不再等于"丢数据"，
     * 所以这一项才是"画面有没有条带没被写过"的直接判据。
     */
    public func slotClobberCount(): Int32 {
        unsafe { lvglcj_sdl2_slot_clobber_count() }
    }

    /**
     * 当前**实际生效**的视频驱动名（`"x11"` / `"KMSDRM"` / `"dummy"` …）。
     * 未初始化时返回 `"(未初始化)"`。
     *
     * ★ 这个方法是**判定"真的在 kmsdrm 上"的唯一依据**。
     *   环境变量 `SDL_VIDEODRIVER=kmsdrm` 只是**请求**：驱动不可用时
     *   （没有已连接的显示器、DRM master 被桌面占着、库没编 KMS 支持）
     *   SDL **不报错**，而是静默回落到它还能用的驱动 —— 程序照跑、帧照 Present。
     *   不查这一项，就会把"回落成功"误读成"kmsdrm 成功"。
     */
    public func videoDriverName(): String {
        let n = unsafe { lvglcj_sdl2_video_driver().toString() }
        return if (n.size == 0) {
            "(未初始化)"
        } else {
            n
        }
    }

    /**
     * 取走自上次调用以来累计的滚轮格数（取走后清零）。正值 = 滚轮向上。
     *
     * ★ 它**取代**了"把滚轮喂给 indev"这种做法：读 LVGL 的 lv_indev.c 可见，
     *   encoder 型 indev 的 `enc_diff` 在导航模式下走的是 `lv_group_focus_prev/next`
     *   —— **移动焦点，不滚动视图**。所以滚轮只能由调用方取走后自行滚动，
     *   例如在 indev 读回调里：`let w = backend.takeWheelSteps(); w != 0 && root.scrollBy(0, w * 96)`。
     *
     * 必须在**同一条线程**上消费（示例是 LVGL 线程的读回调）——"取走"是读-改-写。
     */
    public func takeWheelSteps(): Int32 {
        unsafe { lvglcj_sdl2_take_wheel_steps() }
    }

    /**
     * 取走最近一次按下的键（SDL keysym，如 `'s'` 就是 `0x73`）；没有则返回 `0`。
     *
     * ★ 它补的是一个**半接线的缺口**：后端一直在收集按键，但此前没有读出入口，
     *   而示例只建了 pointer indev —— 于是键盘在应用侧完全不可见。
     * 语义：只记**按下**、只保留**最近一次**、取走即清零（与 `takeWheelSteps` 一致）。
     * 要走 LVGL 的键位导航（keypad/encoder），请用 `LvGroup` + `LvIndev` 那条路。
     */
    public func takeKey(): UInt32 {
        unsafe { lvglcj_sdl2_take_key() }
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
