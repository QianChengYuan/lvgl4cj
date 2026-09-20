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
