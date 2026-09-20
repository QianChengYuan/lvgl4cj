/*
 * font.cj —— 字体 L1 包装（设计文档 §5.10）
 *
 * ===================== 两个必须写在最前面的坑 =====================
 *
 * 【坑 1】LVGL 内置的 Montserrat 系列**只有拉丁字形**。
 * 界面里写中文时，LVGL 会对每个汉字报
 *   "draw_letter: glyph dsc. not found for U+6211"
 * 而画面上**什么都不显示** —— 症状是「按钮看上去是空的」，
 * 很容易被误判成布局或样式问题。
 *
 * 【坑 2】★ 内置的 CJK 字体（Cjk16）**不是「中文可用」的开关**。
 *
 * 官方对 LV_FONT_SIMSUN_16_CJK 的描述是
 *   "1000 of the most common **CJK radicals**"（lv_conf_template.h 等四处一致），
 * 但实测（native/probe/probe_font_coverage.c）表明它更像一份
 * **手工拼合的混合清单**：约 1118 个 CJK 统一表意文字 + 173 个日文假名，
 * 简体常用字、繁体字、日文异体混在一起 —— 连「部首表」这个字面读法也不成立
 * （部首样本实测缺 19/28）。
 *
 * 实测**缺失**的常用简体字：问 厅 灯 调 窗 帘 显 ……
 * 实测**在册**的繁体/日文异体：問 調 窓 ……
 *
 * 因此：
 *   · Cjk16 值得用 —— 但它值得用的理由是「能让**相当一部分**中文显示出来」
 *     （你好/世界/中文 等在册，见 P0 断言 A4 依赖的「我」也在册）；
 *   · **不能**据此认为任意中文文本都能正常显示。缺字时 LVGL 只在日志里打
 *     一行 glyph not found，画面上直接少一块，**不报错、不抛异常**；
 *   · 所以「这段话能不能显示」只能逐个字符核对，不存在一个开关式的答案。
 *     正式产品应加载覆盖完整的自定义字体（LvFont.load）。
 *
 * （CJK 字体需在 lv_conf 中开启；未开启时 LvFont.builtin 会明确抛错，
 *   而不是静默回退到无中文字形的默认字体。）
 */
package lvgl4cj

import std.collection.ArrayList
import std.deriving.Derive

/// 内置字体（数值与 C 侧 lvglcj_builtin_font_t 一致）
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvBuiltinFont {
    | Default // 0：LVGL 默认字体（Montserrat 14）——**无中文字形**
    // 1：SIMSUN 16 CJK。★ 官方称 "1000 most common CJK radicals"，实测是混合清单
    // （约 1118 个 CJK 字 + 173 个日文假名），**有缺字**：问/厅/灯/调/窗/帘/显 均缺失，
    // 而 問/調/窓 等在册。用它，但不要以为任意中文都安全，详见文件头「坑 2」。
    | Cjk16
}

func lvBuiltinFontToInt(f: LvBuiltinFont): Int32 {
    match (f) {
        case Default => 0
        case Cjk16 => 1
    }
}

public class LvFont {
    public let handle: Int64

    private init(h: Int64) {
        handle = h
    }

    /**
     * 取内置字体句柄。幂等：同一内置字体重复取返回**同一**句柄
     * （C 侧句柄表对「同一指针重复登记返回既有句柄」），因此不需要缓存。
     *
     * @throws[LvglException] 类型为 NotSupported —— 该内置字体未编译进本库
     *         （CJK 字体需在 lv_conf 中开启并重建原生库）。
     */
    public static func builtin(f: LvBuiltinFont): LvFont {
        let h = unsafe { lvglcj_font_builtin(lvBuiltinFontToInt(f)) }
        if (h <= 0) {
            throw LvglException(NotSupported,
                "内置字体不可用：CJK 字体需在 lv_conf 中开启（scripts/gen_lv_conf.sh）并重建原生库")
        }
        return LvFont(h)
    }

    /**
     * 从文件加载 binfont。
     * 需要 LV_USE_FS_POSIX 已开启且 lvglcj_fs_init_posix(root) 已初始化，
     * 否则会因「打不开」而失败 —— C 侧会给出明确错误码。
     *
     * @throws[LvglException] 类型为 BackendFailure —— 文件打不开、格式非法或内存不足。
     */
    public static func load(path: String): LvFont {
        /* 用 Int64 版本：句柄本身就是 Int64，不该经过 Int32 中转 */
        let h = withCString64(path) {cs => unsafe { lvglcj_font_load(cs) }}
        if (h <= 0) {
            throw LvglException(BackendFailure, "字体加载失败：${path}（检查 lv_fs 是否已初始化）")
        }
        return LvFont(h)
    }

    /**
     * 该字体是否含有某个字符的字形。
     *
     * ★ 「没有字形」**不是**异常，它返回 `false` —— 缺字是一个合法答案。
     *   只有在**查询本身**失败时才抛异常（句柄无效 / 非 LVGL 线程 / 未初始化）。
     *   这个区分很重要：把「缺字」抛成异常会让调用方被迫用 try/catch 处理
     *   一个正常情况，反而更容易写出忽略它的代码。
     *
     * @throws[LvglException] 查询失败（**不含**「缺字」）
     */
    public func hasGlyph(cp: Rune): Bool {
        /* Rune 的码位范围是 0..0x10FFFF，一定能放进 Int32 */
        let r = unsafe { lvglcj_font_has_glyph(handle, Int32(UInt32(cp))) }
        if (r < 0) {
            throwIfError(r, "font.hasGlyph")
        }
        return r == 1
    }

    /**
     * 逐字报出这段文本里**渲染不出来**的字符（去重，按首次出现顺序）。
     *
     * ★ 这是本层对「内置 CJK 字库有缺字」这个事实的正式应对。
     *
     * 缺字在 LVGL 里是**静默**的：它只打一行 `glyph dsc. not found for U+XXXX`，
     * 画面上直接少一块，不报错、不抛异常、也不影响同一段里的其他字符。
     * 因此「文案里少了一个字」这件事在链路上无迹可寻 ——
     * 本函数把它变成一次可断言、可进 CI 的查询。
     *
     * ```
     * let f = LvFont.builtin(LvBuiltinFont.Cjk16)
     * if (f.missingGlyphs("文件已保存").size > 0) { ... }
     * ```
     *
     * @throws[LvglException] 查询本身失败（缺字不算，它体现在返回的列表里）
     */
    public func missingGlyphs(text: String): ArrayList<Rune> {
        let missing = ArrayList<Rune>()
        /*
         * 按 Rune 迭代（`for (r in text)` 迭代的是 UTF-8 **字节**，
         * 那是仓颉与多数语言不同的一处，会把一个汉字拆成 3 个 byte 逐个查询）。
         */
        for (r in text.runes()) {
            /*
             * 去重用手写线性扫描，**不用 `ArrayList.contains`**：
             * 以 Rune 字面量作实参时该重载解析失败（Rune 的相等性来自运算符，
             * 未实现 Equatable<Rune>，而 contains 带该约束）。
             * 传变量能过、传字面量不过 —— 这种「换个写法就变」的依赖不该进公共实现。
             * 复杂度无关紧要：missing 的元素数量是「缺字的种数」，通常个位数。
             */
            var seen = false
            for (x in missing) {
                if (x == r) {
                    seen = true
                    break
                }
            }
            if (!seen && !hasGlyph(r)) {
                missing.add(r)
            }
        }
        return missing
    }

    /*
     * 释放。
     * ★ 内置字体不可释放 —— C 侧会拒绝并返回 InvalidArgument；
     *   这里把它**原样抛出**而不是当成功吞掉，因为那通常意味着调用方
     *   把自己加载的字体和内置字体搞混了，是需要修代码的信号。
     */
    public func close(): Unit {
        let rc = unsafe { lvglcj_font_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return /* 已关过 */
        }
        throwIfError(rc, "font.close")
    }
}
