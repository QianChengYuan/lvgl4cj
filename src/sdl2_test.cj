/*
 * sdl2_test.cj —— SDL2 后端 L1 包装的测试（设计文档 §8 / §10.3）
 *
 * ===================== 这个测试「测什么」要说清楚 =====================
 * 后端逻辑本身已由 C 侧 test_sdl2_backend 覆盖（31 项，含超时/即时解锁等时序）。
 * 本用例的目标是**另一件事**：让 L1 包装的每个 foreign 声明被真正**执行**一次。
 *
 *   只做到「编译 + 链接通过」是不够的 —— 链接只检查符号存在，
 *   **不检查签名匹配**。返回类型或参数写错（例如 Int32 写成 Int64）
 *   在链接期毫无征兆，只在运行时给出错误结果，是这类绑定最难查的一类缺陷。
 *
 * ===================== 关于显示环境 =====================
 * 无显示环境（纯 headless CI）下 SDL_Init 会失败。此时本用例打印提示后
 * 正常返回（跳过），而不是让整套测试变红 —— 因为「有没有窗口」不是
 * L1 包装的正确性问题。headless 下的等价验证请看 C 侧 test_sdl2_backend
 * （它用 SDL_VIDEODRIVER=dummy，无需窗口）。
 *
 * 想在本机以 headless 方式跑本用例：
 *   SDL_VIDEODRIVER=dummy cjpm test --filter='*Sdl2*'
 */
package lvgl4cj

@Test
class Sdl2Tests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    @TestCase
    func testSdl2LifecycleAndCounters(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()

        var backend: Option<Sdl2Backend> = None
        try {
            /* 小尺寸即可：本用例只验证接线与计数，不验证可观赏性 */
            backend = Some(Sdl2Backend.start(160, 120))
        } catch (e: LvglException) {
            /*
             * 无显示环境：明确跳过而不是伪装通过。
             * 打印出来是为了让「跳过了」这件事在测试输出里可见 ——
             * 静默跳过会让人误以为这条链路已被验证过。
             */
            eprintln("[sdl2_test] 跳过：SDL2 初始化失败（${lvglErrorName(e.code)}）——无可用显示环境")
            rt.deinitLvgl()
            return
        }

        let b = backend.getOrThrow()
        @Assert(b.display().handle != 0, true)
        @Assert(b.isRenderThread(), true) /* start 由当前线程调用，故当前线程即渲染线程 */
        @Assert(b.isPaused(), false)
        @Assert(b.flushCount(), 0)

        /*
         * 单线程推进几帧：pollEvents + pump 交替。
         * 这里就体现了「单线程分流」的价值 —— 若 flush 走「投递请求 + 等待」，
         * 这几帧每帧都会白等 100ms 并累加 waitTimeoutCount。
         */
        for (_ in 0..40) {
            if (b.pollEvents()) {
                break
            }
            rt.pump()
            sleep(Duration.millisecond * 3)
            if (b.presentCount() > 0) {
                break
            }
        }
        @Assert(b.flushCount() > 0, true) /* flush 链路真的走到了 */
        @Assert(b.presentCount() > 0, true) /* 真的呈现过 */
        @Assert(b.waitTimeoutCount(), 0) /* ★ 单线程下不应有任何超时 */

        /* 注入退出：模拟窗口关闭，等待方应被立即放行（断言 7 的机制） */
        b.injectQuit()
        @Assert(b.isPaused(), true)

        b.close()
        /* 幂等：重复关闭无副作用 */
        b.close()
        rt.deinitLvgl()
    }

    @TestCase
    func testUnsupportedColorFormatThrows(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()

        /*
         * RGB888 在 C 侧被明确拒绝（像素格式无一一对应，猜错即花屏）。
         * 这里验证 L1 把该错误码正确翻译成了 LvglException(InvalidArgument)。
         *
         * 注意：异常在**非回调帧**内构造，因此不涉及 C4 的构造时机问题。
         */
        var caught: Option<LvglError> = None
        try {
            let _ = Sdl2Backend.start(64, 64, colorFormat: LV_COLOR_FORMAT_RGB888)
        } catch (e: LvglException) {
            caught = Some(e.code)
        }
        @Assert(caught.getOrThrow(), InvalidArgument)

        rt.deinitLvgl()
    }
}
