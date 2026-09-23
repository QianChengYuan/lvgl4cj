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
            throw LvglException(OutOfMemory, "label 创建失败（LVGL 内存池可能已满）")
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
            throw LvglException(OutOfMemory, "button 创建失败（LVGL 内存池可能已满）")
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
            throw LvglException(OutOfMemory, "switch 创建失败（LVGL 内存池可能已满）")
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
            throw LvglException(OutOfMemory, "checkbox 创建失败（LVGL 内存池可能已满）")
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
            throw LvglException(OutOfMemory, "bar 创建失败（LVGL 内存池可能已满）")
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
