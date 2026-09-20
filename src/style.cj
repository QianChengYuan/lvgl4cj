/*
 * style.cj —— 样式 DSL（设计文档 §5.7 / §6.3）
 *
 * ===================== v9 selector 的语义（写错会静默失效）=====================
 *   setter：lv_style_set_bg_color(style, color)            ← **不带 selector**
 *   应用：  lv_obj_add_style(obj, style, selector)          ← selector 在这里给
 *
 * 因此本类的属性设置函数都只有「值」一个参数，selector 出现在 applyTo/removeFrom。
 * 若照 v8 的习惯在 setter 上写 selector，会变成「设置到某个不存在的状态」——
 * 不报错、样式也完全看不到效果。
 *
 * ===================== selector 是什么 =====================
 *   selector = part | state   （两部分按位或）
 *   · part  ：控件内部的哪一块（主体/指示器/滚动条…）
 *   · state ：哪种状态下生效（默认/按下/聚焦/禁用…）
 * P0 只用到「主部件 + 默认状态」= 0，这是所有控件最常用的组合。
 * 其它 part/state 常量在真正需要时再补，且必须先去 LVGL 头文件核对取值 ——
 * 这些魔数猜错同样是静默失效。
 */
package lvgl4cj

/* part：控件内部区域。主部件恒为 0（LV_PART_MAIN） */
public const LV_PART_MAIN: Int32 = 0

/*
 * state：状态位。与 part 按位或，组合成 selector 传给 addStyle/removeStyle/applyTo。
 *
 * 取值来源：third_party/lvgl/src/core/lv_obj.h 的 `enum { LV_STATE_* }`（逐项一致）。
 *
 * ★ 为什么必须把这组位暴露出来：
 *   selector = (part | state) 的位或，而那几个 API 的 selector 参数**只在默认态**
 *   有默认值。若调用方拿不到 DEFAULT 以外的状态位，就**无法给控件做任何非默认态样式**
 *   —— 连「按下时变色」这种最基本的需求都表达不出来。
 *   本层早先只定义了 DEFAULT，等于把整个状态维度藏了起来
 *   （t8 的随机序列测试需要两个不同的 selector 时才发现 ——
 *    这也是"用不到就不会发现"的一类缺口）。
 */
public const LV_STATE_DEFAULT: Int32 = 0x0000
public const LV_STATE_CHECKED: Int32 = 0x0001
public const LV_STATE_FOCUSED: Int32 = 0x0002
public const LV_STATE_FOCUS_KEY: Int32 = 0x0004
public const LV_STATE_EDITED: Int32 = 0x0008
public const LV_STATE_HOVERED: Int32 = 0x0010
public const LV_STATE_PRESSED: Int32 = 0x0020
public const LV_STATE_SCROLLED: Int32 = 0x0040
public const LV_STATE_DISABLED: Int32 = 0x0080
public const LV_STATE_USER_1: Int32 = 0x1000
public const LV_STATE_USER_2: Int32 = 0x2000
public const LV_STATE_USER_3: Int32 = 0x4000
public const LV_STATE_USER_4: Int32 = 0x8000

/// 组合 selector：part | state
public func selector(part: Int32, state: Int32): Int32 {
    part | state
}

/// 由 RGB 分量构造 0xRRGGBB（与 C 侧颜色口径一致，避免手写十六进制出错）
public func rgb(r: Int32, g: Int32, b: Int32): UInt32 {
    (((UInt32(r) & 0xFFu32) << 16) | ((UInt32(g) & 0xFFu32) << 8) | (UInt32(b) & 0xFFu32))
}

/*
 * 样式对象。属性在本地累积，applyTo 时连同 selector 一起挂到对象上。
 *
 * ★ 内存所有权：lv_style_t 由 C 侧 malloc 并句柄化，close() 才释放。
 *   一个 style 可以挂到任意多个对象上（LVGL 内部按引用使用），
 *   因此**在所有使用者都移除它（或对象被删除）之前不能 close**。
 *
 *   ★ 这条约束由 C 侧**强制**而不是靠使用者自觉（t8 起）：
 *     样式是引用型资源，先释放样式再删除对象会让对象的样式链表留下悬空指针，
 *     删除对象时即 heap-use-after-free（ASan 实测发现）。
 *     现在 close() 会先查引用计数，仍被引用则抛 LvglException(InvalidArgument)，
 *     报错信息里带有引用者数量 —— 把一次静默的内存破坏变成一条可操作的错误。
 *     （这与「不做引用计数的魔法」并不矛盾：我们不做自动释放，
 *       只是拒绝一个必定导致内存破坏的操作。）
 */
public class LvStyle {
    public let handle: Int64

    private init(h: Int64) {
        handle = h
    }

    /**
     * 创建一个空样式（不含任何属性；属性用 setXxx 累积）。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— C 侧无法分配样式结构时。
     */
    public static func create(): LvStyle {
        let h = unsafe { lvglcj_style_create() }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "style 创建失败")
        }
        return LvStyle(h)
    }

    // ---------------------------------------------------------- 背景
    public func bgColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_bg_color(handle, c) }, "bgColor")
        return this
    }

    /// 背景不透明度：0..255（255 = LV_OPA_COVER）
    public func bgOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_bg_opa(handle, v) }, "bgOpa")
        return this
    }

    public func bgGradColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_bg_grad_color(handle, c) }, "bgGradColor")
        return this
    }

    // ---------------------------------------------------------- 边框
    public func borderWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_border_width(handle, v) }, "borderWidth")
        return this
    }

    public func borderColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_border_color(handle, c) }, "borderColor")
        return this
    }

    // ---------------------------------------------------------- 圆角与内边距
    public func radius(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_radius(handle, v) }, "radius")
        return this
    }

    public func padAll(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_all(handle, v) }, "padAll")
        return this
    }

    public func padRow(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_row(handle, v) }, "padRow")
        return this
    }

    public func padColumn(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_column(handle, v) }, "padColumn")
        return this
    }

    // ---------------------------------------------------------- 尺寸约束
    public func width(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_width(handle, v) }, "width")
        return this
    }

    public func height(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_height(handle, v) }, "height")
        return this
    }

    public func minWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_min_width(handle, v) }, "minWidth")
        return this
    }

    public func minHeight(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_min_height(handle, v) }, "minHeight")
        return this
    }

    // ---------------------------------------------------------- 文本
    public func textColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_text_color(handle, c) }, "textColor")
        return this
    }

    public func textOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_text_opa(handle, v) }, "textOpa")
        return this
    }

    public func textLetterSpace(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_text_letter_space(handle, v) }, "textLetterSpace")
        return this
    }

    /*
     * 设置字体。
     *
     * ★ 两件事都要知道（与 src/font.cj 文件头的「两个坑」是同一件事）：
     *   1. 默认的 Montserrat **没有中文字形**，汉字会静默地画不出来 ——
     *      所以含中文的界面需要显式设 CJK 字体；
     *   2. 但 Cjk16 **不是「中文可用」的开关**，它本身有缺字
     *      （实测缺 问/厅/灯/调/窗/帘/显……，而 問/調/窓 在册）。
     *      真正要显示的文本请用 LvFont.missingGlyphs() 或
     *      scripts/cjk_audit.py 逐个字符核对。
     */
    public func textFont(f: LvFont): LvStyle {
        check(unsafe { lvglcj_style_set_text_font(handle, f.handle) }, "textFont")
        return this
    }

    // ---------------------------------------------------------- 阴影
    public func shadowWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_shadow_width(handle, v) }, "shadowWidth")
        return this
    }

    public func shadowColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_shadow_color(handle, c) }, "shadowColor")
        return this
    }

    public func shadowOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_shadow_opa(handle, v) }, "shadowOpa")
        return this
    }

    public func shadowOffsetY(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_shadow_offset_y(handle, v) }, "shadowOffsetY")
        return this
    }

    // ---------------------------------------------------------- 外轮廓
    public func outlineWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_outline_width(handle, v) }, "outlineWidth")
        return this
    }

    public func outlineColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_outline_color(handle, c) }, "outlineColor")
        return this
    }

    public func outlinePad(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_outline_pad(handle, v) }, "outlinePad")
        return this
    }

    // ---------------------------------------------------------- 整体
    public func opa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_opa(handle, v) }, "opa")
        return this
    }

    public func translateY(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_translate_y(handle, v) }, "translateY")
        return this
    }

    // ------------------------------------------------ 补齐的属性（由生成器产出）
    /*
     * ★ 下面这一段是 **scripts/gen_bindings.py 生成的**，不要手工改。
     *
     * 为什么它们是被"补"出来的：本层的样式包装此前只覆盖了 C 侧 ABI 的
     * 26/53 —— 缺的里面有 padLeft/padRight/padTop/padBottom、
     * shadowOffsetX（而 shadowOffsetY 有）、lineWidth、borderOpa 等。
     * 「某个属性能不能用」当时取决于谁顺手写了哪个，而不是取决于设计。
     *
     * 生成器的输入是**C 侧 ABI**（唯一真源），因此 L1 与 ABI 的一致性
     * 从此是自动的。校验：`python3 scripts/gen_bindings.py --check`
     * （它会列出「ABI 有而 L1 没有」的单向漂移，并核对写法一致）。
     *
     * 特例：textFont 仍由上方手写提供 —— 它收 LvFont 而不是裸 Int64 句柄，
     * 把类型安全留在 L1，因此生成器显式跳过（见脚本里的 SPECIAL_CASE）。
     */
    public func bgGradDir(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_bg_grad_dir(handle, v) }, "bgGradDir")
        return this
    }

    public func borderOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_border_opa(handle, v) }, "borderOpa")
        return this
    }

    public func padTop(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_top(handle, v) }, "padTop")
        return this
    }

    public func padBottom(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_bottom(handle, v) }, "padBottom")
        return this
    }

    public func padLeft(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_left(handle, v) }, "padLeft")
        return this
    }

    public func padRight(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_pad_right(handle, v) }, "padRight")
        return this
    }

    public func maxWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_max_width(handle, v) }, "maxWidth")
        return this
    }

    public func maxHeight(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_max_height(handle, v) }, "maxHeight")
        return this
    }

    public func textAlign(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_text_align(handle, v) }, "textAlign")
        return this
    }

    public func textLineSpace(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_text_line_space(handle, v) }, "textLineSpace")
        return this
    }

    public func shadowOffsetX(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_shadow_offset_x(handle, v) }, "shadowOffsetX")
        return this
    }

    public func outlineOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_outline_opa(handle, v) }, "outlineOpa")
        return this
    }

    public func translateX(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_translate_x(handle, v) }, "translateX")
        return this
    }

    public func lineWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_line_width(handle, v) }, "lineWidth")
        return this
    }

    public func lineDashWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_line_dash_width(handle, v) }, "lineDashWidth")
        return this
    }

    public func lineDashGap(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_line_dash_gap(handle, v) }, "lineDashGap")
        return this
    }

    public func arcWidth(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_arc_width(handle, v) }, "arcWidth")
        return this
    }

    public func arcColor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_arc_color(handle, c) }, "arcColor")
        return this
    }

    public func arcRounded(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_arc_rounded(handle, v) }, "arcRounded")
        return this
    }

    public func imageOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_image_opa(handle, v) }, "imageOpa")
        return this
    }

    public func imageRecolor(c: UInt32): LvStyle {
        check(unsafe { lvglcj_style_set_image_recolor(handle, c) }, "imageRecolor")
        return this
    }

    public func imageRecolorOpa(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_image_recolor_opa(handle, v) }, "imageRecolorOpa")
        return this
    }

    public func blendMode(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_blend_mode(handle, v) }, "blendMode")
        return this
    }

    public func layout(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_layout(handle, v) }, "layout")
        return this
    }

    public func baseDir(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_base_dir(handle, v) }, "baseDir")
        return this
    }

    public func clipCorner(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_clip_corner(handle, v) }, "clipCorner")
        return this
    }

    public func rotarySensitivity(v: Int32): LvStyle {
        check(unsafe { lvglcj_style_set_rotary_sensitivity(handle, v) }, "rotarySensitivity")
        return this
    }

    // ---------------------------------------------------------- 应用到对象
    /// 把本样式挂到对象上；selector 决定「哪一块、哪种状态」
    public func applyTo(obj: LvObject, selector!: Int32 = LV_PART_MAIN | LV_STATE_DEFAULT): Unit {
        check(unsafe { lvglcj_obj_add_style(obj.handle, handle, selector) }, "applyTo")
    }

    public func removeFrom(obj: LvObject, selector!: Int32 = LV_PART_MAIN | LV_STATE_DEFAULT): Unit {
        check(unsafe { lvglcj_obj_remove_style(obj.handle, handle, selector) }, "removeFrom")
    }

    /*
     * 释放样式。幂等：C 侧对非 ALIVE 句柄返回 InvalidHandle，这里视为「已关过」。
     * ★ 若该样式仍挂在某个对象上就 close，LVGL 侧会留下悬空引用 ——
     *   本层不阻止（与 C 侧一致），但使用顺序必须先 removeFrom 或先删对象。
     */
    public func close(): Unit {
        ensureLvglOwnership()
        let rc = unsafe { lvglcj_style_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return
        }
        check(rc, "style.close")
    }

    private func check(rc: Int32, what: String): Unit {
        throwIfError(rc, "样式操作 ${what}")
    }
}
