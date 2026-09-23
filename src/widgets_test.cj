/*
 * widgets_test.cj —— 控件 L1 的测试（P0 三个 + P1 批次 1 三个）
 *
 * ============================================================================
 * 【为什么新控件必须有 L1 测试，而不是"C 侧断言过了就够"】
 * ============================================================================
 * C 侧断言证明的是 **ABI 层**的行为；L1 包装是另一层代码，它会引入
 * 不在这条链路上的错误。本批就有两个活例子：
 *
 *   1. `isChecked()` 的底层返回是**三态**（1=勾选 / 0=未勾选 / 负数=错误）。
 *      包装里若图省事写成 `return lvglcj_..._is_checked(h) == 1`，
 *      那么**句柄已失效**时它会静默返回 `false` —— 把"操作错了"伪装成
 *      "没勾上"。C 侧断言测不到这件事，因为 C 侧的返回值本身是对的。
 *      → 本文件的 testInvalidatedHandleThrows 专门钉住这一点。
 *
 *   2. `bar.value()` 要走**出参**（Array + acquireArrayRawData），
 *      而读值必须发生在 release **之前**，否则读到的是已恢复可移动性的内存。
 *      这类错误只可能出现在 L1，C 侧完全看不见。
 *      → testBarValueRoundTrip / testBarNegativeValueRoundTrip 覆盖。
 *
 * 结论：ABI 层与 L1 层各自需要自己的断言，两者不是重复而是互补。
 */

package lvgl4cj

@Test
class WidgetTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    /*
     * 判断某个操作是否抛出了 LvglException。
     *
     * 不直接用 try/catch 包住整段断言，是因为"抛出"本身就是要断言的对象；
     * 若断言写在 catch 里，一个意外抛出会被当成"符合预期"而蒙混过去。
     */
    private func throwsLvgl(f: () -> Unit): Bool {
        try {
            f()
        } catch (e: LvglException) {
            return true
        }
        return false
    }

    // ==================================================== 一、P0 控件（回归护栏）

    @TestCase
    func testLabelTextAndInvalidMode(): Unit {
        let rt = LvglRuntime()
        /*
         * ★ 必须 startHeadless，不能只 initLvgl()：
         *   只初始化 LVGL 而没有显示时，创建控件会因为**没有默认屏幕**而失败。
         *   一开始这里写成 initLvgl()，得到的报错是
         *     LvglException(OutOfMemory): label 创建失败（LVGL 内存池可能已满）
         *   —— 把"没启动显示"说成了"内存池已满"，方向完全错。
         *   所以这里补了显示，同时把那条错误文本改成会指出这个成因
         *   （见 native/src/widgets.c 与 src/widgets.cj 的对应文案）。
         */
        let _ = rt.startHeadless(320, 240)
        let lbl = LvLabel.create(LvObject.screenActive())
        lbl.setText("你好")

        /* 合法模式：0..4（WRAP/DOT/SCROLL/SCROLL_CIRCULAR/CLIP）全部放行 */
        /* ★ 显式 Int32(m)：仓颉整型字面量与范围迭代的默认类型是 Int64，
         *   而 setLongMode 收 Int32 —— 不显式转换会以"mismatched types"失败。 */
        for (m in 0..=4) {
            lbl.setLongMode(Int32(m))
        }
        /* 越界必须抛错，而不是静默取一个默认值 */
        @Assert(throwsLvgl({=> lbl.setLongMode(99) }), true)

        lbl.close()
        rt.deinitLvgl()
    }

    @TestCase
    func testButtonWithLabel(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let btn = LvButton.create(LvObject.screenActive())
        /* withLabel 是便捷写法：建 label + 设文本 + 居中，返回 label 供后续更新 */
        btn.withLabel("点我").setText("点了 1 次")
        btn.close()
        rt.deinitLvgl()
    }

    // ==================================================== 二、P1 批次 1：switch

    @TestCase
    func testSwitchCheckedRoundTrip(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let sw = LvSwitch.create(LvObject.screenActive())

        /* 初始未勾选 —— 注意这是**正常返回值**，不是错误 */
        @Assert(sw.isChecked(), false)
        sw.setChecked(true)
        @Assert(sw.isChecked(), true)
        sw.setChecked(false)
        @Assert(sw.isChecked(), false)
        /* 幂等：重复设同一个值不应出错 */
        sw.setChecked(false)
        @Assert(sw.isChecked(), false)

        sw.close()
        rt.deinitLvgl()
    }

    // ==================================================== 三、P1 批次 1：checkbox

    @TestCase
    func testCheckboxTextAndCheckedRoundTrip(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let cb = LvCheckbox.create(LvObject.screenActive())

        cb.setText("选项 A")
        @Assert(cb.isChecked(), false)
        cb.setChecked(true)
        @Assert(cb.isChecked(), true)

        cb.close()
        rt.deinitLvgl()
    }

    // ==================================================== 四、P1 批次 1：bar

    @TestCase
    func testBarValueRoundTrip(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let bar = LvBar.create(LvObject.screenActive())

        bar.setRange(0, 100)
        /* ★ 数组元素类型必须显式写成 Array<Int32>：
         *   否则字面量推断为 Int64，与 setValue/value 的 Int32 不匹配。 */
        let cases: Array<Int32> = [0, 1, 42, 99, 100]
        for (v in cases) {
            bar.setValue(v)
            /* ★ 设完立刻读回即为新值 —— 这一条钉住 setValue 关动画的决定 */
            @Assert(bar.value(), v)
        }

        bar.close()
        rt.deinitLvgl()
    }

    /*
     * ★ 负值往返：这是 bar.value() 必须用**出参**的直接原因。
     *   若把 L1 的实现改成"返回值即结果、负数当错误"，
     *   这条断言会立刻失败 —— 而那种写法在只看 0..100 的测试里永远暴露不出来。
     */
    @TestCase
    func testBarNegativeValueRoundTrip(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let bar = LvBar.create(LvObject.screenActive())

        bar.setRange(-50, 50)
        bar.setValue(-20)
        @Assert(bar.value(), -20)

        bar.close()
        rt.deinitLvgl()
    }

    /* 范围顺序写反必须明确报错，而不是变成"设了值但显示不对" */
    @TestCase
    func testBarInvalidRangeThrows(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let bar = LvBar.create(LvObject.screenActive())

        @Assert(throwsLvgl({=> bar.setRange(100, 0) }), true)
        @Assert(throwsLvgl({=> bar.setRange(5, 5) }), true)
        /* 负值范围是合法的，不该被误拦 —— 与上面成对，防止校验写成"必须为正" */
        bar.setRange(-10, -1)

        bar.close()
        rt.deinitLvgl()
    }

    // ==================================================== 六、Canvas（§3.11.2）

    /*
     * Canvas 是**自定义绘制的唯一出口**（§3.11.1 把 lv_draw_* 列为非目标）。
     * 这里在 L1 层验证三件事：
     *   · 未设缓冲就绘制 → 在 L1 就抛错，而不是崩在 LVGL 内部；
     *   · 绘制与调色板参数校验会一路传到 L1；
     *   · 重复设缓冲不报错（缓冲由 C 侧持有并随之释放，L1 不接触任何指针）。
     */
    @TestCase
    func testCanvasDrawOpsAndBufferOwnership(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let cv = LvCanvas.create(LvObject.screenActive())

        /* ★ 未设缓冲 → 明确抛错 */
        @Assert(throwsLvgl({=> cv.drawPoint(0, 0, 0xFF0000) }), true)
        @Assert(throwsLvgl({=> cv.fillBg(0x112233) }), true)

        cv.setBuffer(64, 64)
        cv.fillBg(0x1A1D2E) /* 单参重载 = 不透明 */
        cv.fillBg(0x1A1D2E, 128)
        cv.drawPoint(1, 1, 0xFF0000)
        cv.drawLine(0, 0, 63, 63, 0xFFFFFF)
        cv.drawRect(4, 4, 8, 8, 0x00FF00)
        cv.drawArc(32, 32, 20, 0, 180, 0x00D9B5)
        cv.setPalette(1, 0x80FF0000)

        /* 参数校验：尺寸、半径、调色板索引 */
        @Assert(throwsLvgl({=> cv.setBuffer(0, 0) }), true)
        @Assert(throwsLvgl({=> cv.drawRect(0, 0, 0, 8, 0xFFFFFF) }), true)
        @Assert(throwsLvgl({=> cv.drawArc(0, 0, 70000, 0, 90, 0xFFFFFF) }), true)
        @Assert(throwsLvgl({=> cv.setPalette(256, 0) }), true)

        /* ★ 改尺寸：缓冲由 C 侧持有，L1 只给尺寸，重复调用不应出错也不应泄漏 */
        cv.setBuffer(32, 32)
        cv.drawPoint(5, 5, 0xFFFFFF)

        cv.close()
        rt.deinitLvgl()
    }

    // ==================================================== 五、★ 失效句柄必须抛错

    /*
     * ★ 本文件最重要的一组断言。
     *
     *   「句柄已失效」与「状态为 false」是**两件事**，L1 必须让它们可区分：
     *     · 前者是**调用方用错了**，必须抛异常；
     *     · 后者是**合法答案**，返回 false。
     *   若包装把底层三态压成 `rc == 1`，失效句柄会被静默当成"没勾上"，
     *   于是"用错句柄"这条线在整条链路上消失 —— 这正是本项目要堵的那类静默。
     */
    @TestCase
    func testInvalidatedHandleThrows(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)

        let holder = LvObject.create(LvObject.screenActive())
        let sw = LvSwitch.create(holder)
        let cb = LvCheckbox.create(holder)
        let bar = LvBar.create(holder)

        @Assert(sw.isChecked(), false)

        /* 删除承载父对象 → 三个子控件的句柄级联失效 */
        holder.close()
        /* isAlive 是**属性**不是方法（写成 isAlive() 会报 no matching function） */
        @Assert(sw.isAlive, false)
        @Assert(cb.isAlive, false)
        @Assert(bar.isAlive, false)

        /* 已失效的句柄上做任何操作都必须抛错，而不是静默返回 false/0 */
        @Assert(throwsLvgl({=> sw.isChecked() }), true)
        @Assert(throwsLvgl({=> cb.isChecked() }), true)
        @Assert(throwsLvgl({=> bar.value() }), true)
        @Assert(throwsLvgl({=> sw.setChecked(true) }), true)

        /* close() 对已失效句柄应幂等（不抛），否则清理代码会变得很难写 */
        sw.close()
        cb.close()
        bar.close()

        rt.deinitLvgl()
    }
}
