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

    // ==================================================== 七、P1 批次 2

    /*
     * slider / arc / led / spinner。
     * 三者（含 bar）共用同一段「值 + 范围」实现，所以这里刻意让 slider 与 arc
     * 走到**同一组边界**上：如果只有一处校验生效，就说明"形状复用"名存实亡。
     */
    @TestCase
    func testSliderAndArcValueRoundTrip(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let holder = LvObject.create(LvObject.screenActive())

        let sl = LvSlider.create(holder)
        sl.setRange(0, 100)
        sl.setValue(42)
        @Assert(sl.value(), 42)
        /* 负值范围与负值往返：出参形式存在的理由 */
        sl.setRange(-50, 50)
        sl.setValue(-20)
        @Assert(sl.value(), -20)
        /* 与 bar 完全相同的校验：范围写反必须被拒 */
        @Assert(throwsLvgl({=> sl.setRange(100, 0) }), true)

        let ar = LvArc.create(holder)
        ar.setRange(0, 360)
        ar.setValue(90)
        @Assert(ar.value(), 90)
        @Assert(throwsLvgl({=> ar.setRange(5, 5) }), true)

        holder.close()
        rt.deinitLvgl()
    }

    @TestCase
    func testLedBrightnessBoundsAndSpinner(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let holder = LvObject.create(LvObject.screenActive())

        let ld = LvLed.create(holder)
        ld.setColor(0xFF0000)
        ld.setBrightness(0)
        ld.setBrightness(255)
        ld.setOn(true)
        ld.setOn(false)
        /* ★ 越界必须抛错：LVGL 收 uint8_t，放行会静默截断成另一个值 */
        @Assert(throwsLvgl({=> ld.setBrightness(256) }), true)
        @Assert(throwsLvgl({=> ld.setBrightness(-1) }), true)

        /* spinner 无属性可设 —— 能创建即其全部接口 */
        let sp = LvSpinner.create(holder)
        @Assert(sp.isAlive, true)

        holder.close()
        /* 四个新控件都随父级联失效 */
        @Assert(isAliveAt(ld), false)
        @Assert(isAliveAt(sp), false)
        rt.deinitLvgl()
    }

    /*
     * 小助手：把 isAlive 属性读成值。
     * ★ 名字必须是**小驼峰** —— 首版写成 sl_alive，cjlint 报
     *   G.NAM.04（Function name recommend to use lower camel case）。
     *   顺带说明为什么要有这个助手：isAlive 是**属性**，
     *   直接写在 @Assert 里容易与"看起来像方法调用"的写法混掉
     *   （写成 isAlive() 会报 no matching function，错误信息不指向属性/方法之别）。
     */
    private func isAliveAt(o: LvObject): Bool {
        return o.isAlive
    }

    // ==================================================== 八、P1 批次 3：dropdown

    /*
     * dropdown 引入新的值类型（字符串列表）。这里在 L1 层验证两件事：
     *   · 选项用 '\n' 分隔这一形态在 L1 是**如实透传**的（不偷偷改语义）；
     *   · 选中的索引读回 **0 不是错误** —— 这条正是 get_* 不必用出参的前提，
     *     也是它与 LvBar.value() 形式不同的原因（值域是否与错误码重叠）。
     */
    @TestCase
    func testDropdownOptionsAndSelection(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let dd = LvDropdown.create(LvObject.screenActive())

        dd.setOptions("一\n二\n三")
        @Assert(dd.optionCount(), 3)
        /* ★ 默认选中第 0 项：0 是合法答案，不应被当成错误 */
        @Assert(dd.selected(), 0)
        dd.setSelected(2)
        @Assert(dd.selected(), 2)

        /* 选项个数随重新设置而变 */
        dd.setOptions("a\nb")
        @Assert(dd.optionCount(), 2)

        /* 负索引必须抛错（底层收 uint32_t，放行会变成巨大索引） */
        @Assert(throwsLvgl({=> dd.setSelected(-1) }), true)

        /* 失效句柄上取值必须抛错，而不是返回 0 装作"选中第 0 项" */
        let holder = LvObject.create(LvObject.screenActive())
        let dd2 = LvDropdown.create(holder)
        dd2.setOptions("x\ny")
        holder.close()
        @Assert(throwsLvgl({=> dd2.selected() }), true)
        @Assert(throwsLvgl({=> dd2.optionCount() }), true)

        dd.close()
        rt.deinitLvgl()
    }

    // ==================================================== 九、P1 批次 4：line

    /*
     * line 是**第二类所有权**（LVGL 不拷贝点数组、只保存地址）。
     * L1 层要钉住的是这一层自己的责任：**形态校验**。
     * C 侧只知道"点的个数"，看不出坐标对是否成对 ——
     * 传进奇数个元素时，C 侧会把它当成 floor(n/2) 个点而**静默丢掉最后一个**。
     * 所以奇数长度必须在 L1 就拦住，而不是让它变成一个画得不太对的折线。
     */
    @TestCase
    func testLinePointsAndShapeValidation(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let ln = LvLine.create(LvObject.screenActive())

        /* 三个点：(0,0) (10,20) (30,0) */
        ln.setPoints([0, 0, 10, 20, 30, 0])
        ln.setYInvert(true)
        ln.setYInvert(false)
        /* 重设点集：旧的那块由 C 侧释放（泄漏会被 ASan 抓到） */
        ln.setPoints([0, 0, 5, 5])

        /* ★ 形态校验：奇数个元素必须被拦下，而不是静默丢掉最后一个坐标 */
        @Assert(throwsLvgl({=> ln.setPoints([0, 0, 5]) }), true)
        /* 少于一个点也必须被拦下 */
        @Assert(throwsLvgl({=> ln.setPoints([]) }), true)

        /* 删除后操作必须抛错 */
        let holder = LvObject.create(LvObject.screenActive())
        let ln2 = LvLine.create(holder)
        ln2.setPoints([0, 0, 1, 1])
        holder.close()
        @Assert(throwsLvgl({=> ln2.setPoints([0, 0, 1, 1]) }), true)

        ln.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十、P1 批次 5：image

    /*
     * image 的契约要点不在"能显示图片"（那需要真实文件与解码器，属端到端场景），
     * 而在三件容易想当然的事：
     *   · 路径被 LVGL **拷贝** —— 所以传临时构造的 String 是安全的；
     *   · 路径不存在**不抛异常** —— 这是 LVGL 的既定行为，本层如实透传；
     *     不写清楚，它在界面上表现为"设了 src 却一片空白"；
     *   · 缩放单位是 **256 = 100%**，不是百分比。
     */
    @TestCase
    func testImageSrcCopySemanticsAndScaleUnit(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let im = LvImage.create(LvObject.screenActive())

        /* 传一个临时构造的 String：安全的前提是 LVGL 会拷贝（本用例正是在钉这一点） */
        im.setSrc("A:/does/not/exist.bin")
        /* 路径不存在不抛异常 —— 这是既定行为而不是遗漏 */
        im.setSrc("A:/also/missing.png")

        im.setOffset(3, -4)
        im.setScale(256) /* 256 = 100% */
        im.setScale(128) /* 一半 */

        /* 非法缩放必须被拦：0 不是"无缩放"（那是 256），负值会被底层当 uint32 用 */
        @Assert(throwsLvgl({=> im.setScale(0) }), true)
        @Assert(throwsLvgl({=> im.setScale(-1) }), true)

        /* 失效句柄上操作必须抛错 */
        let holder = LvObject.create(LvObject.screenActive())
        let im2 = LvImage.create(holder)
        im2.setSrc("A:/x.bin")
        holder.close()
        @Assert(throwsLvgl({=> im2.setSrc("A:/y.bin") }), true)

        im.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十一、P1 批次 6：roller

    /*
     * roller 值得单独测的点，都不是"能不能滚"，而是三条读实现才确认的语义：
     *   · 两种模式都拷贝选项串（"无限模式要外部保活"是错的）；
     *   · options == NULL 是错误（与 image 的 NULL 表示清空正好相反）；
     *   · 选中索引必须本层校验，且校验基准在两种模式下都是**对外选项数**。
     */
    @TestCase
    func testRollerOptionOwnershipAndBounds(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let ro = LvRoller.create(LvObject.screenActive())

        /* 传临时构造的 String 是安全的（两种模式都拷贝） */
        ro.setOptions("One\nTwo\nThree")
        @Assert(ro.selected(), 0)
        ro.setSelected(2)
        @Assert(ro.selected(), 2)

        /* 越界与负值必须被拦：底层不钳制，放行后失败点会离调用点很远 */
        @Assert(throwsLvgl({=> ro.setSelected(3) }), true)
        @Assert(throwsLvgl({=> ro.setSelected(-1) }), true)

        ro.setVisibleRowCount(3)
        @Assert(throwsLvgl({=> ro.setVisibleRowCount(0) }), true)
        @Assert(throwsLvgl({=> ro.setVisibleRowCount(-2) }), true)

        /* 无限模式：对外索引语义与普通模式一致（校验基准是 get_option_count） */
        ro.setOptionsInfinite("A\nB\nC")
        ro.setSelected(1)
        @Assert(ro.selected(), 1)
        @Assert(throwsLvgl({=> ro.setSelected(3) }), true)

        /* 失效句柄上操作必须抛错 */
        let holder = LvObject.create(LvObject.screenActive())
        let ro2 = LvRoller.create(holder)
        ro2.setOptions("x\ny")
        holder.close()
        @Assert(throwsLvgl({=> ro2.setSelected(0) }), true)

        ro.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十二、P1 批次 7：textarea

    /*
     * text() 是本层第一个把字符串从 C 取回来的入口，因此重点不在"能读能写"，
     * 而在两条边界：
     *   · 长度按**字节**（中文一个字 3 字节）—— 按字符理解会在多字节文本上出现
     *     "看起来只少了一个字"这类难查的截断；
     *   · 句柄失效必须**抛异常而不是返回空串** —— "读不到"和"内容是空的"
     *     混成一件，调用方会把失效当成用户清空了输入。
     */
    @TestCase
    func testTextareaRoundTripAndFailureIsNotEmpty(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let ta = LvTextarea.create(LvObject.screenActive())

        ta.setText("hello")
        @Assert(ta.text(), "hello")

        /* 空文本单独走一条路径（探到长度 0 就不分配缓冲） */
        ta.setText("")
        @Assert(ta.text(), "")

        /* 多字节按字节计：3 个汉字 = 9 字节 */
        ta.setText("中文测")
        @Assert(ta.text(), "中文测")
        @Assert(ta.text().size, 9)

        ta.setOneLine(true)
        ta.setText("single")
        @Assert(ta.text(), "single")
        ta.setPlaceholderText("type here")

        /* 密码模式：显示成圆点，但取回的应是真实文本 */
        ta.setPasswordMode(true)
        ta.setText("secret")
        @Assert(ta.text(), "secret")
        ta.setPasswordMode(false)

        ta.setMaxLength(0) /* 0 = 不限制 */
        @Assert(throwsLvgl({=> ta.setMaxLength(-1) }), true)

        /* ★ 失效句柄：text() 抛异常，而不是返回空串 */
        let holder = LvObject.create(LvObject.screenActive())
        let ta2 = LvTextarea.create(holder)
        ta2.setText("x")
        holder.close()
        @Assert(throwsLvgl({=> ta2.text() }), true)
        @Assert(throwsLvgl({=> ta2.setText("y") }), true)

        ta.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十三、P1 批次 8：table

    /*
     * 本用例的中心是**一条更正**：我先前把"表格单元格文本只存指针、不拷贝"
     * （v8 的旧行为）当成了 v9 的事实。实际是拷贝的。
     * 代价说明：若按那个错误前提去实现，会多出一套"替 LVGL 持有字符串"的机制，
     * 并在替换单元格时引入新的释放时序问题 —— 代码量增加、风险增加，
     * 而收益为零。所以这里把"传临时 String 后立刻丢弃"写进测试钉住。
     *
     * 另一条是真正的坑：setCellValue 会自动扩容，负值转 uint32 会变成约 42 亿。
     */
    @TestCase
    func testTableCellCopyAndNegativeIndexGuard(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let tb = LvTable.create(LvObject.screenActive())

        tb.setCellValue(0, 0, "A1")
        @Assert(tb.cellValue(0, 0), "A1")

        /* 自动扩容：不必先设行列数 */
        tb.setCellValue(2, 3, "D3")
        @Assert(tb.rowCount() >= 3, true)
        @Assert(tb.columnCount() >= 4, true)
        @Assert(tb.cellValue(2, 3), "D3")

        /* 取用不扩容：越界抛异常，且不改结构 */
        let rowsBefore = tb.rowCount()
        @Assert(throwsLvgl({=> tb.cellValue(99, 0) }), true)
        @Assert(tb.rowCount(), rowsBefore)

        /* 空单元格是空串，不是异常 */
        @Assert(tb.cellValue(1, 1), "")

        /* 负索引必须拦：否则"设一格"会变成"申请 42 亿行" */
        @Assert(throwsLvgl({=> tb.setCellValue(-1, 0, "x") }), true)
        @Assert(throwsLvgl({=> tb.setCellValue(0, -1, "x") }), true)
        @Assert(throwsLvgl({=> tb.setRowCount(-1) }), true)
        @Assert(throwsLvgl({=> tb.setColumnCount(-1) }), true)

        /* 多字节按字节计 */
        tb.setCellValue(0, 0, "表格")
        @Assert(tb.cellValue(0, 0), "表格")
        @Assert(tb.cellValue(0, 0).size, 6)

        tb.setRowCount(2)
        tb.setColumnCount(2)
        @Assert(tb.rowCount(), 2)
        @Assert(tb.columnCount(), 2)

        /* 控制位：合并与裁剪，只验接口可用 */
        tb.mergeRight(0, 0)
        tb.unmergeRight(0, 0)
        tb.setTextCrop(0, 1, true)

        /* 失效句柄：读单元格抛异常，而不是返回空串 */
        let holder = LvObject.create(LvObject.screenActive())
        let tb2 = LvTable.create(holder)
        tb2.setCellValue(0, 0, "x")
        holder.close()
        @Assert(throwsLvgl({=> tb2.cellValue(0, 0) }), true)
        @Assert(throwsLvgl({=> tb2.setCellValue(0, 0, "y") }), true)

        tb.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十四、P1 批次 9：keyboard

    /*
     * keyboard 的接口本身没有所有权问题，本用例要钉的是**绑定关系**这条契约：
     *   LVGL 不会在 textarea 被删时清理 keyboard→textarea 的绑定（键盘持有裸指针，
     *   被绑方完全不知道这件事），而按键处理会解引用它 —— 所以删除 textarea 之前
     *   必须先解绑。本用例只走**安全顺序**；危险顺序是契约禁止的，写成用例就等于
     *   让测试自己去踩悬空指针。
     */
    @TestCase
    func testKeyboardBindingContractAndSafeTeardown(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let kb = LvKeyboard.create(LvObject.screenActive())
        let ta = LvTextarea.create(LvObject.screenActive())

        /* 未绑定 → None（而不是造一个假对象或抛异常） */
        @Assert(kb.textarea().isNone(), true)

        kb.setTextarea(ta)
        @Assert(kb.textarea().isSome(), true)
        /* ★ 用**行为**验证"取回的是同一个对象"，而不是比较句柄数字：
         *   对取回的对象写一次，再看原对象是否变了 —— 这比比数字更能说明问题。
         *   （更正一处旧注释：句柄字段其实是 public 的（LvObject.handle），
         *     当初写"未必可见"是我没有核实就下的判断。这里不比较句柄是刻意的：
         *     行为一致才是"同一个对象"，句柄相同只是必要条件。） */
        kb.textarea().getOrThrow().setText("typed")
        @Assert(ta.text(), "typed")

        /* 类型系统挡住的：setTextarea 只收 LvTextarea，传别的对象编译不过 */
        kb.setMode(LVGLCJ_KEYBOARD_MODE_NUMBER)
        @Assert(kb.mode(), LVGLCJ_KEYBOARD_MODE_NUMBER)
        @Assert(throwsLvgl({=> kb.setMode(99) }), true)
        kb.setPopovers(true)

        /* 解绑：clearTextarea 之后回到 None */
        kb.clearTextarea()
        @Assert(kb.textarea().isNone(), true)

        /* 失效句柄：绑定一个已删除的 textarea 必须抛错，而不是把悬空指针存进去 */
        let holder = LvObject.create(LvObject.screenActive())
        let ta2 = LvTextarea.create(holder)
        holder.close()
        @Assert(throwsLvgl({=> kb.setTextarea(ta2) }), true)
        @Assert(kb.textarea().isNone(), true)

        /* 安全顺序：先解绑、再删 textarea，之后键盘仍然可用 */
        kb.setTextarea(ta)
        kb.clearTextarea()
        ta.close()
        kb.setMode(LVGLCJ_KEYBOARD_MODE_TEXT_UPPER)
        @Assert(kb.mode(), LVGLCJ_KEYBOARD_MODE_TEXT_UPPER)

        kb.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十五、P1 批次 10：chart

    /*
     * chart 是 P1 里唯一需要动"句柄层设计"的：series 不是对象，进不了句柄表，
     * 所以本层自己维护索引。本用例钉的是这个索引的语义（它比接口本身更容易出错）：
     *   · 图表被删除（含级联）后索引不可用；
     *   · remove() 之后索引**永久**失效，新加的拿新索引 —— 旧索引不会悄悄指向新曲线；
     *   · 点下标必须被校验（底层不检查，越界是界外写，属于最坏的一类输入）。
     */
    @TestCase
    func testChartSeriesIndexContract(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let ch = LvChart.create(LvObject.screenActive())

        ch.setType(LVGLCJ_CHART_TYPE_LINE)
        ch.setPointCount(4)
        @Assert(ch.pointCount(), 4)
        @Assert(throwsLvgl({=> ch.setPointCount(0) }), true)
        ch.setRange(LVGLCJ_CHART_AXIS_PRIMARY_Y, 0, 100)

        /* X 轴不能挂 series（LVGL 文档限定 Y 轴） */
        @Assert(throwsLvgl({=> ch.addSeries(0x0000FF, axis: LVGLCJ_CHART_AXIS_PRIMARY_X) }), true)

        let s0 = ch.addSeries(0xFF0000)
        let s1 = ch.addSeries(0x00FF00, axis: LVGLCJ_CHART_AXIS_SECONDARY_Y)
        @Assert(s0.index(), 0)
        @Assert(s1.index(), 1)

        s0.push(10)
        s1.push(20)
        s0.setValue(3, 42)
        /* 点下标越界：底层不检查，必须由本层拦 */
        @Assert(throwsLvgl({=> s0.setValue(4, 1) }), true)
        @Assert(throwsLvgl({=> s0.setValue(-1, 1) }), true)

        /* remove 后该索引永久失效；新加的拿新索引，不复用旧号 */
        s1.remove()
        @Assert(throwsLvgl({=> s1.push(5) }), true)
        @Assert(throwsLvgl({=> s1.remove() }), true)
        let s2 = ch.addSeries(0xFFFF00)
        @Assert(s2.index(), 2)
        s2.push(7)
        s0.push(11) /* 原有曲线不受影响 */

        /* 级联删除：图表没了，索引一并不可用 */
        let box = LvObject.create(LvObject.screenActive())
        let ch2 = LvChart.create(box)
        let s20 = ch2.addSeries(0xFF00FF)
        s20.push(1)
        box.close()
        @Assert(throwsLvgl({=> s20.push(1) }), true)

        ch.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十五、滚动（滚轮的落点）

    /*
     * 这一批加的是 `LvObject.scrollBy` —— 滚轮之所以需要它：LVGL 的滚轮（encoder 型
     * indev）在导航模式下只**移动焦点**，指针通路只认拖动，库里没有"滚轮滚动视图"的
     * 原生通路，所以由后端收 SDL_MOUSEWHEEL、调用方显式滚动。
     *
     * ★ 用例强度的说明（不夸大）：L1 侧**读不到**滚动位置 —— 本层刻意没把
     *   get_scroll_y 做成 ABI（它与负错误码共用取值空间，无法用返回码区分成败）。
     *   所以这里只能钉住"调用契约"（合法句柄不抛、失效句柄抛），
     *   而"滚了多少、方向对不对"由 C 侧的 7l 段用 lv_obj_get_scroll_y 直接量。
     *   把它们分开写，是为了不把"没验证的东西"包装成已验证。
     */
    @TestCase
    func testScrollByContract(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let box = LvObject.create(LvObject.screenActive())
        box.setSize(200, 100)
        let inner = LvObject.create(box)
        inner.setSize(200, 500) /* 超出容器 → 可滚动 */

        /* 合法句柄：滚动成功，且内容没超出时也只是被钳制、不抛错 */
        box.scrollBy(0, -60)
        box.scrollBy(0, 60)
        box.scrollBy(0, -100000) /* 远超内容高度：钳制，不抛错 */

        /* 失效句柄：必须抛错（而不是静默什么都不做） */
        let holder = LvObject.create(LvObject.screenActive())
        let other = LvObject.create(holder)
        holder.close()
        @Assert(throwsLvgl({=> other.scrollBy(0, -10) }), true)

        box.close()
        rt.deinitLvgl()
    }

    // ==================================================== 十六、Group（键盘/编码器导航）

    /*
     * group 的用途：键盘与编码器型 indev **不看坐标**，输入先送进 group、由 group
     * 决定"当前焦点对象"，再发给那个对象。没有 group，键盘按下去什么也不会发生 ——
     * 设计文档 §12 的 P1 阶段把它列为交付项；此前 C 侧实现与 foreign 声明都已存在，
     * 只缺 L1 封装（这一批补上）。
     *
     * 判定方式刻意只用**行为**：把当前焦点对象取回来写一次文本，再看两个 textarea
     * 各自的内容 —— 这比比句柄数字更能说明"焦点真的落在谁身上"，
     * 也不依赖句柄的内部表示（句柄计数在别处已单独钉过）。
     */
    @TestCase
    func testGroupFocusNavigation(): Unit {
        let rt = LvglRuntime()
        let _ = rt.startHeadless(320, 240)
        let scr = LvObject.screenActive()
        let a = LvTextarea.create(scr)
        let b = LvTextarea.create(scr)
        let g = LvGroup.create()

        /* 还没加入任何对象时，focused() 应是 None（而不是造一个假对象）。
         * ★ 这条断言原先被我放在两句 add **之后**，结果失败 —— 那不是实现的问题，
         *   而是我漏了 LVGL 的行为：加入空组时会**自动聚焦第一个**加入的对象。
         *   下面紧接着就把这条行为也钉住。 */
        @Assert(g.focused().isNone(), true)

        g.add(a)
        g.add(b)

        /* 加入空组 → 自动聚焦第一个加入的对象（实测行为，不是猜的） */
        @Assert(g.focused().getOrThrow().handle, a.handle)

        /* 焦点在 a：focused() 取回的应是 a 的句柄 */
        g.focus(a)
        @Assert(g.focused().isSome(), true)
        @Assert(g.focused().getOrThrow().handle, a.handle)

        /* focusNext → 焦点换到 b */
        g.focusNext()
        @Assert(g.focused().getOrThrow().handle, b.handle)

        /* focusPrev 回到 a */
        g.focusPrev()
        @Assert(g.focused().getOrThrow().handle, a.handle)

        /* 移出组后，遍历不再落到 b 身上（组内只剩 a，回绕后仍是 a） */
        g.remove(b)
        g.focusNext()
        @Assert(g.focused().getOrThrow().handle, a.handle)

        /* 失效句柄：关闭后操作必须抛错；再关一次是幂等的（不抛） */
        g.close()
        @Assert(throwsLvgl({=> g.focusNext() }), true)
        g.close()

        a.close()
        b.close()
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
