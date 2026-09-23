/*
 * widgets.cj —— P0 控件（label / button）
 *
 * 按设计文档的门禁要求，P0 只提供 obj / label / button 三个。
 * 控件集扩展留待 P1：「先跑通 hello_cj 与八条断言再加控件，跑不稳不加控件」。
 *
 * 继承关系：LvLabel / LvButton 都继承 LvObject，因此几何、样式、事件
 * 这些通用能力只需在基类实现一次（这也是选继承而非组合的原因 ——
 * 用户写 btn.on(Clicked) { } 比 btn.obj.on(...) 自然）。
 */
package lvgl4cj

/*
 * String → CString 的桥接助手。
 *
 * 仓颉**不会**把 String 隐式转成 CString（编译报
 * 「expected 'CString', found 'String'」），必须显式分配一块 C 字符串内存。
 * 而这块内存的所有权必须由我们负责释放，因此用 try-finally 包住，
 * 保证即使 f 抛异常也不会泄漏。
 *
 * 之所以能「调用完立刻释放」：LVGL 的 lv_label_set_text 会**自己拷贝**
 * 一份文本，不保留我们传入的指针。
 */
func withCString(s: String, f: (CString) -> Int32): Int32 {
    /* LibC 的分配/释放都是 unsafe 函数，必须显式置于 unsafe 上下文 */
    let cs = unsafe { LibC.mallocCString(s) }
    try {
        return f(cs)
    } finally {
        unsafe { LibC.free(cs) }
    }
}

/*
 * 同上，但回调返回 Int64。
 * 存在的理由：句柄是 Int64，若复用 Int32 版本就得 `Int32(handle)` 截断 ——
 * 当前句柄值很小、截断看起来「没问题」，但那是巧合，不是保证。
 * 宁可多一个 6 行的重载，也不要在类型上留一个说不清的转换。
 */
func withCString64(s: String, f: (CString) -> Int64): Int64 {
    let cs = unsafe { LibC.mallocCString(s) }
    try {
        return f(cs)
    } finally {
        unsafe { LibC.free(cs) }
    }
}

/// 文本标签
public class LvLabel <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 label。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— LVGL 内存池已满；
     *         WrongThread —— 在非 LVGL 线程上调用。
     */
    public static func create(parent: LvObject): LvLabel {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_label_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "label 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvLabel(h)
    }

    /*
     * 设置文本。LVGL 会自己拷贝一份，因此传入的 String 生命周期不受约束 ——
     * 不需要像某些绑定那样要求调用方长期持有字符串。
     */
    public func setText(t: String): LvLabel {
        throwIfError(withCString(t) {cs => unsafe { lvglcj_label_set_text(handle, cs) }}, "label.setText")
        return this
    }

    /// 长文本模式（0=WRAP 1=DOT 2=SCROLL 3=SCROLL_CIRCULAR 4=CLIP）
    public func setLongMode(mode: Int32): LvLabel {
        throwIfError(unsafe { lvglcj_label_set_long_mode(handle, mode) }, "label.setLongMode")
        return this
    }
}

/// 按钮
public class LvButton <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 button。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— LVGL 内存池已满；
     *         WrongThread —— 在非 LVGL 线程上调用。
     */
    public static func create(parent: LvObject): LvButton {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_button_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "button 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvButton(h)
    }

    /*
     * 便捷：建一个 label 挂到按钮上（hello_cj 的常用写法）。
     * 返回 label 供后续 setText 更新文字。
     */
    public func withLabel(text: String): LvLabel {
        let l = LvLabel.create(this)
        l.setText(text)
        /* 文字居中：按钮默认会把 label 放在内容区中央，这里显式对齐一次更稳妥 */
        l.align(LvAlign.Center)
        return l
    }
}

/*
 * ===================== P1 批次 1 控件（§7.1）=====================
 * switch / checkbox / bar。
 *
 * 三者都继承 LvObject —— 几何/样式/事件不必重复实现，与 P0 的 label/button 同一取舍。
 *
 * ★ 关于「取状态返回三态」在 L1 的处理：
 *   0 是合法答案（"没勾上"），所以判错误只能写 `r < 0`，
 *   **不能**用 throwIfError 一把梭（它会视 0 为成功，于是"没勾上"与"查询成功"
 *   混成一个结果，调用方拿到的 Bool 无从分辨）。
 *   这与 LvFont.hasGlyph 的处理保持一致 —— 两处都是"0 是答案不是错误"。
 */

/// 二态开关
public class LvSwitch <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 switch。
     *
     * @throws[LvglException] OutOfMemory —— LVGL 内存池已满；
     *         WrongThread —— 非 LVGL 线程；InvalidHandle —— parent 句柄无效。
     *         （parent 传 screenActive() 或 0 表示挂到当前屏幕）
     */
    public static func create(parent: LvObject): LvSwitch {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_switch_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "switch 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvSwitch(h)
    }

    /// 置为开/关
    public func setChecked(on: Bool): LvSwitch {
        let v: Int32 = if (on) {
            1
        } else {
            0
        }
        throwIfError(unsafe { lvglcj_switch_set_checked(handle, v) }, "switch.setChecked")
        return this
    }

    /**
     * 是否处于开启态。
     *
     * ★ 「没有开启」不是异常，它返回 `false`；只有**查询本身**失败才抛异常。
     */
    public func isChecked(): Bool {
        let r = unsafe { lvglcj_switch_is_checked(handle) }
        if (r < 0) {
            throwIfError(r, "switch.isChecked")
        }
        return r == 1
    }
}

/// 带文本的二态勾选框
public class LvCheckbox <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 checkbox。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvCheckbox {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_checkbox_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "checkbox 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvCheckbox(h)
    }

    /// 设置文本（LVGL 自己拷贝一份，传入的 String 生命周期不受约束）
    public func setText(t: String): LvCheckbox {
        throwIfError(withCString(t) {cs => unsafe { lvglcj_checkbox_set_text(handle, cs) }}, "checkbox.setText")
        return this
    }

    /// 置为勾选/未勾选
    public func setChecked(on: Bool): LvCheckbox {
        let v: Int32 = if (on) {
            1
        } else {
            0
        }
        throwIfError(unsafe { lvglcj_checkbox_set_checked(handle, v) }, "checkbox.setChecked")
        return this
    }

    /// 是否已勾选（同 LvSwitch.isChecked：「未勾选」不是异常）
    public func isChecked(): Bool {
        let r = unsafe { lvglcj_checkbox_is_checked(handle) }
        if (r < 0) {
            throwIfError(r, "checkbox.isChecked")
        }
        return r == 1
    }
}

/// 进度 / 数值显示条
public class LvBar <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 bar。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvBar {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_bar_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "bar 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvBar(h)
    }

    /**
     * 设置取值区间。
     *
     * @throws[LvglException] InvalidArgument —— min >= max（LVGL 要求 min < max；
     *         放行会让取值范围换算出现反向映射，表现为"设了值但显示不对"）
     */
    public func setRange(lo: Int32, hi: Int32): LvBar {
        throwIfError(unsafe { lvglcj_bar_set_range(handle, lo, hi) }, "bar.setRange")
        return this
    }

    /// 设当前值（**关动画**：设完即可读到新值，见桥接头的约定 2）
    public func setValue(v: Int32): LvBar {
        throwIfError(unsafe { lvglcj_bar_set_value(handle, v) }, "bar.setValue")
        return this
    }

    /**
     * 读当前值。
     *
     * ★ 出参用 `Array<T>` + `acquireArrayRawData`，与 src/debug.cj 同一取舍
     *   （那里比较过 LibC.malloc / 栈上数组 / 本方案三种做法，
     *    结论是它最不容易在异常路径上留下未释放的内存）。
     *   这里额外必须用出参的原因：条形的值域本身可为负，
     *   若把"负数"当错误码就与合法负值撞车。
     */
    public func value(): Int32 {
        let buf = Array<Int32>(1, repeat: 0)
        let h = unsafe { acquireArrayRawData(buf) }
        /* acquireArrayRawData 返回的是**句柄**，指针要取 h.pointer；
         * 释放时也交回句柄本身（与 src/debug.cj 的 memMonitor / perfSample 同一写法）。
         * 读值必须在 release **之前** —— 释放之后数组的可移动性恢复，指针即失效。 */
        let rc = unsafe { lvglcj_bar_get_value(handle, h.pointer) }
        let v = buf[0]
        unsafe { releaseArrayRawData(h) }
        throwIfError(rc, "bar.value")
        return v
    }
}

/*
 * ===================== P1 批次 2 控件（§7.1）=====================
 * slider / arc / led / spinner。
 *
 * ★ 这批有一半是**形状复用**：slider 与 arc 和上面的 LvBar 是同一形状（值 + 范围），
 *   所以下面把「设范围 / 设值 / 读值」三步抽成三个私有辅助函数，三者共用。
 *   这不是为了少写代码，而是：三份拷贝会各自漂移，而范围校验漏一处的症状是
 *   "设了值但显示不对"——极难归因。共用一个实现，就没有"某一份忘了校验"的可能。
 *
 * ★ 读值一律走出参（Array + acquireArrayRawData），理由同 LvBar.value()：
 *   slider / arc 的值域同样可为负，用"负数表示错误"会与合法负值撞车。
 */
private func valueWidgetSetRange(f: (Int32, Int32) -> Int32, lo: Int32, hi: Int32, what: String): Unit {
    throwIfError(f(lo, hi), what)
}

private func valueWidgetSetValue(f: (Int32) -> Int32, v: Int32, what: String): Unit {
    throwIfError(f(v), what)
}

private func valueWidgetRead(f: (CPointer<Int32>) -> Int32, what: String): Int32 {
    let buf = Array<Int32>(1, repeat: 0)
    let h = unsafe { acquireArrayRawData(buf) }
    let rc = unsafe { f(h.pointer) }
    let v = buf[0]
    unsafe { releaseArrayRawData(h) }
    throwIfError(rc, what)
    return v
}

/// 可拖动的取值控件（值域与 LvBar 相同）
public class LvSlider <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 slider。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvSlider {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_slider_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "slider 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvSlider(h)
    }

    /// 设置取值区间（要求 lo < hi，写反会被明确拒绝）
    public func setRange(lo: Int32, hi: Int32): LvSlider {
        valueWidgetSetRange({a, b => unsafe { lvglcj_slider_set_range(handle, a, b) }}, lo, hi, "slider.setRange")
        return this
    }

    /// 设当前值（关动画：设完立刻读回即为新值）
    public func setValue(v: Int32): LvSlider {
        valueWidgetSetValue({x => unsafe { lvglcj_slider_set_value(handle, x) }}, v, "slider.setValue")
        return this
    }

    /// 读当前值（出参形式，理由见 LvBar.value()）
    public func value(): Int32 {
        return valueWidgetRead({p => unsafe { lvglcj_slider_get_value(handle, p) }}, "slider.value")
    }
}

/// 环形取值控件（值域与 LvBar 相同）
public class LvArc <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 arc。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvArc {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_arc_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "arc 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvArc(h)
    }

    /// 设置取值区间（要求 lo < hi）
    public func setRange(lo: Int32, hi: Int32): LvArc {
        valueWidgetSetRange({a, b => unsafe { lvglcj_arc_set_range(handle, a, b) }}, lo, hi, "arc.setRange")
        return this
    }

    /// 设当前值（LVGL 侧 lv_arc_set_value 本就没有 anim 参数，对外与 slider 一致）
    public func setValue(v: Int32): LvArc {
        valueWidgetSetValue({x => unsafe { lvglcj_arc_set_value(handle, x) }}, v, "arc.setValue")
        return this
    }

    /// 读当前值（出参形式）
    public func value(): Int32 {
        return valueWidgetRead({p => unsafe { lvglcj_arc_get_value(handle, p) }}, "arc.value")
    }
}

/// 指示灯
public class LvLed <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 led。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvLed {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_led_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "led 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvLed(h)
    }

    /// 颜色 **0xRRGGBB**（高 8 位不解释，与 Canvas 绘制类一致）
    public func setColor(color: UInt32): LvLed {
        throwIfError(unsafe { lvglcj_led_set_color(handle, color) }, "led.setColor")
        return this
    }

    /**
     * 亮度 0..255。
     *
     * @throws[LvglException] InvalidArgument —— 越界。LVGL 收 uint8_t，
     *         放行会静默截断（设 300 与设 44 结果一样），所以必须在这里拦。
     */
    public func setBrightness(b: Int32): LvLed {
        throwIfError(unsafe { lvglcj_led_set_brightness(handle, b) }, "led.setBrightness")
        return this
    }

    /// 开 / 关
    public func setOn(on: Bool): LvLed {
        let v: Int32 = if (on) {
            1
        } else {
            0
        }
        throwIfError(unsafe { lvglcj_led_set_on(handle, v) }, "led.setOn")
        return this
    }
}

/*
 * ===================== P1 批次 3：dropdown（§7.1）=====================
 * 这一批引入**新的值类型**：dropdown 带来的是「字符串列表」。
 * 它是三类新值类型里所有权最轻的一类，因为 LVGL **会拷贝**选项字符串 ——
 * 所以 L1 可以直接收 String，不必要求调用方长期持有。
 * （这一点是核对过 LVGL 文档的，不是推测：set_options 的注释明写
 *  "The options will be copied ... the options can be destroyed after calling"。）
 */

/// 下拉选择列表
public class LvDropdown <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 dropdown。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvDropdown {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_dropdown_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "dropdown 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvDropdown(h)
    }

    /**
     * 设置选项。
     *
     * ★ **多项之间用 `\n` 分隔**（这是 LVGL 的原生形态，如 `"一\n二\n三"`）。
     *   之所以沿用这个形态而不是收一个数组：LVGL 的内部表示就是这个字符串，
     *   在这里拆成数组、到 C 侧再拼回去，只会多一次拷贝而没有任何收益。
     *   需要从 `Array<String>` 构造时，调用方自行用 `\n` 连接即可。
     *
     * 选项字符串会被 LVGL 拷贝一份，因此传入的 String 生命周期不受约束。
     */
    public func setOptions(text: String): LvDropdown {
        throwIfError(withCString(text) {cs => unsafe { lvglcj_dropdown_set_options(handle, cs) }}, "dropdown.setOptions")
        return this
    }

    /**
     * 选中第 idx 项（0 起）。
     *
     * @throws[LvglException] InvalidArgument —— idx 为负
     *         （底层收 uint32_t，放行会被隐式转成一个巨大的索引）
     */
    public func setSelected(idx: Int32): LvDropdown {
        throwIfError(unsafe { lvglcj_dropdown_set_selected(handle, idx) }, "dropdown.setSelected")
        return this
    }

    /**
     * 当前选中项索引。
     *
     * ★ 只判负值：索引本身非负（0 是合法答案），所以不需要出参形式 ——
     *   这与 `LvBar.value()` 必须用出参并不矛盾，判据都是**值域是否与错误码重叠**。
     */
    public func selected(): Int32 {
        let r = unsafe { lvglcj_dropdown_get_selected(handle) }
        if (r < 0) {
            throwIfError(r, "dropdown.selected")
        }
        return r
    }

    /// 选项个数（同样：非负、负数是错误）
    public func optionCount(): Int32 {
        let r = unsafe { lvglcj_dropdown_get_option_count(handle) }
        if (r < 0) {
            throwIfError(r, "dropdown.optionCount")
        }
        return r
    }
}

/*
 * ===================== P1 批次 4：line（§7.1）=====================
 * line 引入**第二类所有权**：LVGL **不拷贝点数组**，只保存我们给的地址。
 * 因此 C 侧把坐标复制进自己的缓冲、随对象释放 —— 与 Canvas 的像素缓冲同一套模式。
 * 对 L1 使用者的影响：传进去的数组可以立刻丢弃，不需要一直持有（这一点写在下面对注释里）。
 */

/// 折线
public class LvLine <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 line。**创建后必须先 setPoints 才有内容可画**
     * （与 Canvas 必须先 setBuffer 同理）。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvLine {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_line_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "line 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvLine(h)
    }

    /**
     * 设置点集。
     *
     * `xy` 是**扁平的坐标对**：`x0,y0,x1,y1,...` ——
     * 长度必须是偶数且至少 2（即至少一个点）。
     *
     * 为什么不是 `Array<(Int32, Int32)>`：元组数组跨 FFI 需要一次内存布局假设，
     * 而扁平数组没有这个问题，且与 C 侧契约逐字对应。
     *
     * ★ 坐标会被**复制**到 C 侧缓冲，因此传入的数组可以随即释放 ——
     *   不需要像某些绑定那样要求调用方长期持有它。
     *
     * @throws[LvglException] InvalidArgument —— 元素个数为奇数或少于 2
     */
    public func setPoints(xy: Array<Int32>): LvLine {
        /* L1 就拦住形态错误：C 侧只知道"点的个数"，看不出坐标对是否成对 */
        if (xy.size < 2 || xy.size % 2 != 0) {
            throw LvglException(InvalidArgument, "点集必须是扁平的坐标对 x0,y0,x1,y1,...（偶数个元素且至少 2 个）")
        }
        let n = Int32(xy.size / 2)
        let h = unsafe { acquireArrayRawData(xy) }
        let rc = unsafe { lvglcj_line_set_points(handle, h.pointer, n) }
        unsafe { releaseArrayRawData(h) }
        throwIfError(rc, "line.setPoints")
        return this
    }

    /// y 轴反向（LVGL 默认 y 向下增长；画折线图常需翻转）
    public func setYInvert(on: Bool): LvLine {
        let v: Int32 = if (on) {
            1
        } else {
            0
        }
        throwIfError(unsafe { lvglcj_line_set_y_invert(handle, v) }, "line.setYInvert")
        return this
    }
}

/*
 * ===================== P1 批次 5：image（§7.1）=====================
 * 所有权上 image 属于**轻**的一类：LVGL 会拷贝路径字符串（读实现确认：lv_strdup），
 * 因此 L1 直接收 String 即可，不要求调用方长期持有。
 * 这与 line（LVGL 只存指针、我们持有）形成对照 —— 同一个库里有两种完全相反的语义，
 * 判断依据只能是**逐个核对实现**，不能按控件类别外推。
 *
 * ★ 本类**不提供清空图像的方法**：C 侧 set_src 支持 NULL 表示清空（那是合法语义），
 *   但仓颉的 CString 无法表达空指针，为它单加一个 ABI 入口只是为了绕开 FFI 的表达限制，
 *   属于把工具的形状泄进契约。需要清空时用 setSrc 换一个源即可；
 *   确有需要时再按契约变更补一个显式的 clear。
 */

/// 图像
public class LvImage <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 image。创建后需 setSrc 才有内容显示。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvImage {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_image_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "image 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvImage(h)
    }

    /**
     * 设置图像源（文件路径，需配合已注册的文件系统驱动，如 POSIX 驱动）。
     *
     * ★ **路径不存在时不会抛异常** —— LVGL 只是不显示该图像（它自己打一行日志）。
     *   本层无法在不额外付出代价的前提下提前发现（POSIX 驱动没有 exists 接口），
     *   所以调用方若要判定"到底加载上没有"，需自行确认文件存在。
     *   不把这件事写清楚，它在界面上会表现为"设了 src 却一片空白"。
     *
     * 路径字符串会被 LVGL 拷贝，传入的 String 生命周期不受约束。
     */
    public func setSrc(path: String): LvImage {
        throwIfError(withCString(path) {cs => unsafe { lvglcj_image_set_src(handle, cs) }}, "image.setSrc")
        return this
    }

    /// 偏移（像素）
    public func setOffset(x: Int32, y: Int32): LvImage {
        throwIfError(unsafe { lvglcj_image_set_offset(handle, x, y) }, "image.setOffset")
        return this
    }

    /**
     * 缩放。
     *
     * ★ **单位不是百分比**：`256` = 100%，`128` = 一半，`512` = 两倍。
     *   传 `100` 会得到约 39% —— 这是 LVGL 的既有约定，此处如实透传而非换算，
     *   因为把"百分比"这个更直观的单位引进来的同时，也就把"到底哪个是 100%"这个问题
     *   永久地留在了两套单位之间。
     *
     * @throws[LvglException] InvalidArgument —— 值非正（0 不是"无缩放"，那是 256）
     */
    public func setScale(zoom: Int32): LvImage {
        throwIfError(unsafe { lvglcj_image_set_scale(handle, zoom) }, "image.setScale")
        return this
    }
}

/*
 * ===================== P1 批次 6：roller =====================
 * roller 的所有权语义又一次与直觉相反，记录了核对过程：
 * 以为"无限模式要调用方保活这份选项串"，读实现后发现两种模式**都拷贝** ——
 * NORMAL 走 lv_label_set_text，INFINITE 自己复制出重复串后立刻 lv_free。
 * 同一份代码里 image 的 NULL 表示清空、roller 的 NULL 表示错误；
 * image 是拷贝、canvas/line 是外部保活 —— 没有可外推的规律，只能逐个核对。
 */

/// 滚轮选择器（选项以 '\n' 分隔）
public class LvRoller <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 roller。创建后需 setOptions 才有内容。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvRoller {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_roller_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "roller 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvRoller(h)
    }

    /**
     * 设置选项（普通模式：滚到底即停）。选项以 `\n` 分隔。
     *
     * 选项串会被 LVGL 拷贝（两种模式都是），传入的 String 生命周期不受约束。
     */
    public func setOptions(options: String): LvRoller {
        throwIfError(
            withCString(options) {cs => unsafe { lvglcj_roller_set_options(handle, cs, LVGLCJ_ROLLER_MODE_NORMAL) }},
            "roller.setOptions")
        return this
    }

    /// 设置选项（无限模式：可一直滚）
    public func setOptionsInfinite(options: String): LvRoller {
        throwIfError(
            withCString(options) {cs => unsafe { lvglcj_roller_set_options(handle, cs, LVGLCJ_ROLLER_MODE_INFINITE) }},
            "roller.setOptionsInfinite")
        return this
    }

    /**
     * 选中第 sel 项（0 基）。
     *
     * 越界会抛 InvalidArgument —— 底层的 lv_roller_set_selected 收 uint32_t 且不钳制，
     * 越界不报错而是把内部 id 设大，等渲染时才越界读，失败点离调用点很远。
     * 选中过程**关闭动画**，因此 setSelected 返回后 selected() 立即是稳定值。
     *
     * @throws[LvglException] InvalidArgument —— 索引越界
     */
    public func setSelected(sel: Int32): LvRoller {
        throwIfError(unsafe { lvglcj_roller_set_selected(handle, sel) }, "roller.setSelected")
        return this
    }

    /**
     * 当前选中索引（0 基）
     *
     * @throws[LvglException] InvalidArgument —— 句柄失效等导致读取失败
     */
    public func selected(): Int32 {
        let r = unsafe { lvglcj_roller_get_selected(handle) }
        if (r < 0) {
            throw LvglException(InvalidArgument, "roller.selected 失败")
        }
        return r
    }

    /**
     * 可见行数（须为正）。
     *
     * @throws[LvglException] InvalidArgument —— 值非正
     */
    public func setVisibleRowCount(rows: Int32): LvRoller {
        throwIfError(unsafe { lvglcj_roller_set_visible_row_count(handle, rows) }, "roller.setVisibleRowCount")
        return this
    }
}

/*
 * ===================== P1 批次 7：textarea =====================
 * 文本所有权与 label 同类：LVGL 会拷贝（NORMAL 走 label，密码模式先 lv_strdup）。
 *
 * ★ text() 的取回方式是本层第一个"把字符串从 C 取回来"的入口，做法写在这里：
 *   先传空指针探长度，再按长度取 —— 两次调用换掉"猜缓冲该多大"这件事。
 *   缓冲用 Array<UInt8> + acquireArrayRawData（与 LvDebug 同一做法：
 *   不必自己做 LibC.malloc，也就不存在异常路径漏 free 的问题）。
 *   长度单位是**字节**而不是字符（中文一个字 3 字节），这与 C 侧契约一致。
 */

/// 文本输入框
public class LvTextarea <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /*
     * 把一个**已存在**的 textarea（句柄来自反查，如 LvKeyboard.textarea()）包成对象。
     *
     * 刻意用同包可见的静态入口，而不是把构造放宽成 internal：
     *   · 构造一旦放宽，"凭一个句柄造对象"就成了所有同包代码都能做的事，
     *     而这是个应当被限制在少数明确场景（这里是"键盘取回绑定目标"）的能力；
     *   · 也不做成 public —— 外部用户不该有"凭句柄造对象"的入口，
     *     那会绕开 create 语义（比如以为它会新建对象）。
     */
    static func fromExistingHandle(h: Int64): LvTextarea {
        return LvTextarea(h)
    }

    /**
     * 创建一个 textarea。创建后需 setText 才有内容（空框会显示占位文本）。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvTextarea {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_textarea_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "textarea 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvTextarea(h)
    }

    /**
     * 设置文本。`\n` 表示换行（单行模式下会被折成空格）。
     *
     * 文本会被 LVGL 拷贝，传入的 String 生命周期不受约束。
     *
     * @throws[LvglException] 句柄失效等
     */
    public func setText(txt: String): LvTextarea {
        throwIfError(withCString(txt) {
            cs => unsafe { lvglcj_textarea_set_text(handle, cs) }
        }, "textarea.setText")
        return this
    }

    /**
     * 取当前文本。
     *
     * 密码模式下拿到的是**真实文本**（LVGL 把真文本另存了一份），不是显示用的圆点。
     *
     * @throws[LvglException] 句柄失效时抛错 —— 刻意**不返回空串**：
     *   "读不到"与"内容是空的"是两件事，混成一件会让调用方把失效当成空输入。
     */
    public func text(): String {
        /* 先探长度：传空指针，C 侧只回报所需字节数 */
        let n = unsafe { lvglcj_textarea_get_text(handle, CPointer<UInt8>(), 0) }
        if (n < 0) {
            throw LvglException(InvalidArgument, "textarea.text 失败（句柄失效？）")
        }
        if (n == 0) {
            return ""
        }
        /* 多留 1 字节给结尾 NUL。注意 Array 的尺寸参数是 Int64，
         * 而 ABI 的长度是 Int32 —— 这里必须显式转换，不能指望隐式提升。 */
        let len = Int64(n)
        let buf = Array<UInt8>(len + 1, repeat: 0)
        let h = unsafe { acquireArrayRawData(buf) }
        let rc = unsafe { lvglcj_textarea_get_text(handle, h.pointer, n + 1) }
        unsafe { releaseArrayRawData(h) }
        /*
         * ★★ 这里**不能**用 throwIfError。
         *
         *   它把"非 0 / 非 1 的返回值"一律当成错误码，而本函数成功时返回的是
         *   **字节数** —— 于是取回 "secret"（5 字节）会被读成错误码 5，
         *   抛出一个 code=5 的 UnknownCode 异常。这个坑本项目已有先例：
         *   LvDebug.dumpTree 的注释里写着"返回值是节点数，不能交给 throwIfError"。
         *   （写这段时我正是读过那条注释之后仍然踩了 —— 所以这里把它写成规则：
         *    凡是"成功时返回计数/长度"的入口，判错一律用 `< 0`，不交给 throwIfError。）
         */
        if (rc < 0) {
            throw LvglException(InvalidArgument, "textarea.text 失败（错误码 ${rc}）")
        }
        /* 长度按字节给，故这里取前 n 个字节交给 UTF-8 解码 */
        return String.fromUtf8(buf[0..len])
    }

    /**
     * 占位文本（文本为空时显示）。会被拷贝。
     *
     * @throws[LvglException] 句柄失效等
     */
    public func setPlaceholderText(txt: String): LvTextarea {
        throwIfError(withCString(txt) {
            cs => unsafe { lvglcj_textarea_set_placeholder_text(handle, cs) }
        }, "textarea.setPlaceholderText")
        return this
    }

    /// 单行模式（回车不换行、且不显示换行）
    public func setOneLine(on: Bool): LvTextarea {
        throwIfError(unsafe { lvglcj_textarea_set_one_line(handle, if (on) {
            1
        } else {
            0
        }) }, "textarea.setOneLine")
        return this
    }

    /// 密码模式：显示为圆点，内部保留真实文本（text() 仍能取到真实内容）
    public func setPasswordMode(on: Bool): LvTextarea {
        throwIfError(unsafe { lvglcj_textarea_set_password_mode(handle, if (on) {
            1
        } else {
            0
        }) }, "textarea.setPasswordMode")
        return this
    }

    /**
     * 最大字符数。`0` 表示不限制。
     *
     * 注意 LVGL 的 setText 会绕过这个上限（其文档明确说明）——上限约束的是用户输入。
     *
     * @throws[LvglException] InvalidArgument —— 负值（0 才是不限制）
     */
    public func setMaxLength(len: Int32): LvTextarea {
        throwIfError(unsafe { lvglcj_textarea_set_max_length(handle, len) }, "textarea.setMaxLength")
        return this
    }
}

/*
 * ===================== P1 批次 8：table =====================
 * 所有权上 table 属于**轻**的一类（LVGL 会拷贝单元格文本）。
 *
 * ★ 这里更正一个我先前讲错的前提：我曾说"表格单元格文本只存指针、不拷贝"。
 *   实际 v9 是拷贝的（文档 + 实现双重确认）。若照那个错误前提去做，
 *   会凭空多造一套"替 LVGL 持有字符串"的机制 —— 既多余，又会在替换单元格时
 *   引入新的释放时序问题。教训不是"要小心"，而是"不许按库的旧版本印象外推"。
 *
 * ★ 真正的坑在 setCellValue：LVGL 对超范围行列**自动扩容**，
 *   于是负值（转 uint32 变约 42 亿）会让"设一格"变成"申请 42 亿列"。C 侧已拦。
 */

/// 表格
public class LvTable <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 table。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvTable {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_table_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "table 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvTable(h)
    }

    /**
     * 设置单元格文本。
     *
     * 文本会被 LVGL 拷贝，传入的 String 生命周期不受约束。
     * 行/列**超出当前规模时会自动扩容**（LVGL 的既定行为）——
     * 也就是说可以先 setCellValue(2, 3, ...) 再不管规模，表格会自己长到能装下它。
     *
     * @throws[LvglException] InvalidArgument —— 行列索引为负
     */
    public func setCellValue(row: Int32, col: Int32, txt: String): LvTable {
        throwIfError(withCString(txt) {
            cs => unsafe { lvglcj_table_set_cell_value(handle, row, col, cs) }
        }, "table.setCellValue")
        return this
    }

    /**
     * 取单元格文本。
     *
     * 取用**不会**自动扩容：索引超出当前行列范围会抛 InvalidArgument，
     * 而不是先替你造出一张空表 —— 否则"读错一格"就变成静默改变了对象结构。
     *
     * @throws[LvglException] InvalidArgument —— 索引越界或句柄失效
     */
    public func cellValue(row: Int32, col: Int32): String {
        let n = unsafe { lvglcj_table_get_cell_value(handle, row, col, CPointer<UInt8>(), 0) }
        if (n < 0) {
            throw LvglException(InvalidArgument, "table.cellValue 失败（索引越界或句柄失效？）")
        }
        if (n == 0) {
            return ""
        }
        let len = Int64(n)
        let buf = Array<UInt8>(len + 1, repeat: 0)
        let h = unsafe { acquireArrayRawData(buf) }
        let rc = unsafe { lvglcj_table_get_cell_value(handle, row, col, h.pointer, n + 1) }
        unsafe { releaseArrayRawData(h) }
        /* 同 textarea.text：成功时返回的是**字节数**，不能用 throwIfError 判错 */
        if (rc < 0) {
            throw LvglException(InvalidArgument, "table.cellValue 失败（错误码 ${rc}）")
        }
        return String.fromUtf8(buf[0..len])
    }

    /**
     * 设置行数。`0` 合法（相当于清空）。
     *
     * @throws[LvglException] InvalidArgument —— 负值
     */
    public func setRowCount(rows: Int32): LvTable {
        throwIfError(unsafe { lvglcj_table_set_row_count(handle, rows) }, "table.setRowCount")
        return this
    }

    /**
     * 设置列数。`0` 合法（相当于清空）。
     *
     * @throws[LvglException] InvalidArgument —— 负值
     */
    public func setColumnCount(cols: Int32): LvTable {
        throwIfError(unsafe { lvglcj_table_set_column_count(handle, cols) }, "table.setColumnCount")
        return this
    }

    /**
     * 当前行数
     *
     * @throws[LvglException] InvalidArgument —— 句柄失效
     */
    public func rowCount(): Int32 {
        let r = unsafe { lvglcj_table_get_row_count(handle) }
        if (r < 0) {
            throw LvglException(InvalidArgument, "table.rowCount 失败")
        }
        return r
    }

    /**
     * 当前列数
     *
     * @throws[LvglException] InvalidArgument —— 句柄失效
     */
    public func columnCount(): Int32 {
        let r = unsafe { lvglcj_table_get_column_count(handle) }
        if (r < 0) {
            throw LvglException(InvalidArgument, "table.columnCount 失败")
        }
        return r
    }

    /**
     * 把 (row, col) 与右边一格合并。
     *
     * 合并后右邻格的内容不再显示（但值仍在，取消合并即可复现）。
     *
     * @throws[LvglException] InvalidArgument —— 索引为负
     */
    public func mergeRight(row: Int32, col: Int32): LvTable {
        throwIfError(unsafe {
            lvglcj_table_add_cell_ctrl(handle, row, col, LVGLCJ_TABLE_CTRL_MERGE_RIGHT)
        }, "table.mergeRight")
        return this
    }

    /// 取消与右邻格的合并
    public func unmergeRight(row: Int32, col: Int32): LvTable {
        throwIfError(unsafe {
            lvglcj_table_clear_cell_ctrl(handle, row, col, LVGLCJ_TABLE_CTRL_MERGE_RIGHT)
        }, "table.unmergeRight")
        return this
    }

    /**
     * 文本超出单元格时裁剪（而不是换行撑高该行）。
     *
     * @throws[LvglException] InvalidArgument —— 索引为负
     */
    public func setTextCrop(row: Int32, col: Int32, on: Bool): LvTable {
        /* 不把 foreign func 取成函数值再调用：那是跨 FFI 的一等函数，
         * 更容易在签名/调用约定上出意外。两个分支各写一次调用，直白且无歧义。 */
        let rc = if (on) {
            unsafe { lvglcj_table_add_cell_ctrl(handle, row, col, LVGLCJ_TABLE_CTRL_TEXT_CROP) }
        } else {
            unsafe { lvglcj_table_clear_cell_ctrl(handle, row, col, LVGLCJ_TABLE_CTRL_TEXT_CROP) }
        }
        throwIfError(rc, "table.setTextCrop")
        return this
    }
}

/*
 * ===================== P1 批次 9：keyboard =====================
 * 键盘本身的接口没有所有权问题（全是存指针或枚举），真正要讲清楚的是**绑定关系**。
 *
 * ★★ 绑定是单向的，而且 LVGL 不清理它 —— 读实现确认：
 *   lv_keyboard.c 没有任何 LV_EVENT_DELETE 处理；全文件唯一给 keyboard->ta 赋值的地方
 *   就是 lv_keyboard_set_textarea；而 lv_textarea.c 完全不引用 keyboard。
 *   按键处理却直接解引用 keyboard->ta。
 *   ⇒ **删除 textarea 之前必须先解绑**（clearTextarea），否则之后任何一次按键
 *     都会踩悬空指针，且崩溃发生在 LVGL 的事件回调里 —— 本层没有拦截的位置。
 *   这是"契约能挡住的、代码挡不住"的典型情况，所以把它写在最显眼处。
 */

/// 软键盘
public class LvKeyboard <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 keyboard。创建后需 setTextarea 才能真正输入。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvKeyboard {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_keyboard_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "keyboard 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvKeyboard(h)
    }

    /**
     * 绑定输入目标。
     *
     * ★★ **删除 textarea 之前必须先 clearTextarea()。**
     *   LVGL 不会在 textarea 被删时清理这个绑定（键盘持有裸指针，而被绑方不知道
     *   自己被绑了），按键处理会直接解引用它 —— 删掉 textarea 再按键就是踩悬空指针，
     *   崩溃点在 LVGL 的事件回调里，本层无法拦截。
     *
     * 参数类型是 `LvTextarea` 而不是 `LvObject`：LVGL 对这一处有类断言，
     * 传错类型会直接中止。用类型把这条约束放到编译期，比写文档提醒可靠。
     *
     * @throws[LvglException] 句柄失效等
     */
    public func setTextarea(ta: LvTextarea): LvKeyboard {
        throwIfError(unsafe { lvglcj_keyboard_set_textarea(handle, ta.handle) }, "keyboard.setTextarea")
        return this
    }

    /**
     * 解绑当前输入目标。删除 textarea 之前必须调用（见 setTextarea 的说明）。
     *
     * @throws[LvglException] 句柄失效
     */
    public func clearTextarea(): LvKeyboard {
        throwIfError(unsafe { lvglcj_keyboard_set_textarea(handle, 0) }, "keyboard.clearTextarea")
        return this
    }

    /**
     * 当前绑定的输入目标；未绑定时为 `None`。
     *
     * @throws[LvglException] 句柄失效
     */
    public func textarea(): Option<LvTextarea> {
        let h = unsafe { lvglcj_keyboard_get_textarea(handle) }
        if (h < 0) {
            throw LvglException(InvalidArgument, "keyboard.textarea 失败")
        }
        if (h == 0) {
            return None
        }
        /* 这里包装的是一个**已存在**的 textarea，不能走 create（那会新建对象），
         * 故走 LvTextarea 的同包包装入口。若该 textarea 已被删除，
         * 拿到的是它那个已失效的句柄，对其操作会照常报错 —— 这正是想要的。 */
        return Some(LvTextarea.fromExistingHandle(h))
    }

    /**
     * 设置键盘布局模式。
     *
     * @throws[LvglException] InvalidArgument —— 取值越界（合法 0..7）
     */
    public func setMode(mode: Int32): LvKeyboard {
        throwIfError(unsafe { lvglcj_keyboard_set_mode(handle, mode) }, "keyboard.setMode")
        return this
    }

    /**
     * 当前模式（取值见 LVGLCJ_KEYBOARD_MODE_*）
     *
     * @throws[LvglException] InvalidArgument —— 句柄失效
     */
    public func mode(): Int32 {
        let m = unsafe { lvglcj_keyboard_get_mode(handle) }
        if (m < 0) {
            throw LvglException(InvalidArgument, "keyboard.mode 失败")
        }
        return m
    }

    /// 按键弹出预览
    public func setPopovers(on: Bool): LvKeyboard {
        throwIfError(unsafe { lvglcj_keyboard_set_popovers(handle, if (on) {
            1
        } else {
            0
        }) }, "keyboard.setPopovers")
        return this
    }
}

/*
 * ===================== P1 批次 10：chart =====================
 * 这是 P1 清单里唯一需要动"句柄层设计"的一个：series 不是对象。
 *
 * ★ 对外用**索引**而不是指针，理由见 ffi_bridge.cj 的说明。对使用者而言有两条后果
 *   必须知道（都写进了下面各方法的文档）：
 *     · 图表被删除（含被父对象级联删掉）后，它的 series 索引一律不可用；
 *     · remove() 之后该索引**永久**失效，新加的 series 会拿到新的索引 ——
 *       旧索引不会"悄悄指向新曲线"。这个取舍是刻意的：报错比给出错误数据好。
 */

/// 图表
public class LvChart <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 chart。
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvChart {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_chart_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "chart 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvChart(h)
    }

    /**
     * 图表类型（LVGLCJ_CHART_TYPE_*）
     *
     * @throws[LvglException] InvalidArgument —— 取值越界
     */
    public func setType(t: Int32): LvChart {
        throwIfError(unsafe { lvglcj_chart_set_type(handle, t) }, "chart.setType")
        return this
    }

    /**
     * 数据点数。
     *
     * @throws[LvglException] InvalidArgument —— 非正（点数决定内部数组尺寸）
     */
    public func setPointCount(cnt: Int32): LvChart {
        throwIfError(unsafe { lvglcj_chart_set_point_count(handle, cnt) }, "chart.setPointCount")
        return this
    }

    /**
     * 当前点数
     *
     * @throws[LvglException] InvalidArgument —— 句柄失效
     */
    public func pointCount(): Int32 {
        let n = unsafe { lvglcj_chart_get_point_count(handle) }
        if (n < 0) {
            throw LvglException(InvalidArgument, "chart.pointCount 失败")
        }
        return n
    }

    /**
     * 设置坐标轴范围。
     *
     * ★ 轴取值是**位标志**（0/1/2/4），不是连号 —— 见 LVGLCJ_CHART_AXIS_*。
     *
     * @throws[LvglException] InvalidArgument —— 轴取值非法
     */
    public func setRange(axis: Int32, lo: Int32, hi: Int32): LvChart {
        throwIfError(unsafe { lvglcj_chart_set_range(handle, axis, lo, hi) }, "chart.setRange")
        return this
    }

    /**
     * 添加一条曲线，返回它的句柄对象。
     *
     * ★ 曲线只能挂在 Y 轴上（`LVGLCJ_CHART_AXIS_PRIMARY_Y` / `_SECONDARY_Y`）。
     *
     * @throws[LvglException] InvalidArgument —— 轴取值不是 Y 轴
     */
    public func addSeries(color: UInt32, axis!: Int32 = LVGLCJ_CHART_AXIS_PRIMARY_Y): LvChartSeries {
        let idx = unsafe { lvglcj_chart_add_series(handle, color, axis) }
        if (idx < 0) {
            throw LvglException(InvalidArgument, "chart.addSeries 失败（轴取值须为 Y 轴，或 LVGL 内存池已满）")
        }
        return LvChartSeries(handle, idx)
    }
}

/**
 * 图表中的一条曲线。
 *
 * 它**不是** `LvObject` —— 在 LVGL 里 series 不是对象。本层用一个索引来标识它，
 * 索引由所属图表负责解释，所以这个对象只在"图表仍然存活"期间有意义。
 */
public class LvChartSeries {
    private let chartHandle: Int64
    private let idx: Int32

    init(chartHandle: Int64, idx: Int32) {
        this.chartHandle = chartHandle
        this.idx = idx
    }

    /// 这条曲线在所属图表中的索引（从 0 开始）
    public func index(): Int32 {
        return idx
    }

    /**
     * 追加一个数据点（按序推进）。
     *
     * @throws[LvglException] InvalidArgument —— 图谱已删除，或该索引已被移除
     */
    public func push(value: Int32): LvChartSeries {
        throwIfError(unsafe { lvglcj_chart_set_next_value(chartHandle, idx, value) }, "chartSeries.push")
        return this
    }

    /**
     * 按下标写一个数据点。
     *
     * @throws[LvglException] InvalidArgument —— 点下标越界（底层不检查，越界是界外写），
     *   或图谱已删除、该索引已被移除
     */
    public func setValue(pointId: Int32, value: Int32): LvChartSeries {
        throwIfError(unsafe { lvglcj_chart_set_value_by_id(chartHandle, idx, pointId, value) }, "chartSeries.setValue")
        return this
    }

    /**
     * 移除这条曲线。
     *
     * ★ 移除后**该索引永久失效**：新添加的曲线会拿到新索引，不会复用这个号。
     *   这是刻意的取舍 —— 让"旧索引指向新曲线、于是静默画出错数据"不可能发生。
     *
     * @throws[LvglException] InvalidArgument —— 图谱已删除，或该索引已被移除
     */
    public func remove(): Unit {
        throwIfError(unsafe { lvglcj_chart_remove_series(chartHandle, idx) }, "chartSeries.remove")
    }
}

/// 忙碌指示器
public class LvSpinner <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 spinner。
     *
     * ★ 它**没有任何可设的属性**：旋转由 LVGL 内部的弧动画自行驱动，
     *   不经过本层 —— 所以这里除 create 之外不提供任何方法，这不是遗漏。
     *   （与其它控件不同，它也没有 setValue/范围：那些属于 arc，不属于 spinner。）
     *
     * @throws[LvglException] 同 LvSwitch.create
     */
    public static func create(parent: LvObject): LvSpinner {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_spinner_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "spinner 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvSpinner(h)
    }
}
