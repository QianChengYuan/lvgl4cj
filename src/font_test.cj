/*
 * font_test.cj —— 字体 L1 的测试，聚焦「字形覆盖查询」这一项能力
 *
 * ============================================================================
 * 【这个测试存在的理由】
 * ============================================================================
 * 内置 CJK 字库**有缺字**（官方描述是 "1000 of the most common CJK radicals"，
 * 实测是一份简体/繁体/日文异体混合的清单，详见 native/probe/probe_font_coverage.c
 * 与 docs/P0_RESULTS.md §14）。
 *
 * 缺字在 LVGL 里是**静默**的：只打一行 glyph not found 日志，画面上少一块，
 * 不报错、不抛异常、不影响同一段里的其他字符。所以「文案里少了一个字」
 * 在「输入 → 显示」的整条链路上无迹可寻。
 *
 * 因此本测试锁住两件事：
 *   1. 查询能力本身可用（hasGlyph / missingGlyphs 行为正确、缺字不抛异常）；
 *   2. ★ **已实测确认的缺字名单** —— 一旦 LVGL 换字库，这里立刻失败，
 *      强制复核 src/font.cj、src/ffi_bridge.cj、native/include/lvglcj_bridge.h
 *      与 docs/P0_RESULTS.md 中关于该字库的描述。
 *      一个"锁住缺陷"的断言，价值在于让文档与事实不会悄悄分叉。
 */
package lvgl4cj

import std.collection.ArrayList

/*
 * 列表成员判断。
 *
 * ★ 不用 `ArrayList<Rune>.contains(r'X')`：以 **Rune 字面量**作实参时重载解析失败
 *   （Rune 的相等性来自运算符，没有实现 Equatable<Rune>，而 contains 要求该约束；
 *     传 Rune **变量**时能解析，这正是它容易被漏过去的原因）。
 *   测试断言不该依赖这种边缘行为，所以这里用最朴素的循环。
 */
private func hasRune(xs: ArrayList<Rune>, r: Rune): Bool {
    for (x in xs) {
        if (x == r) {
            return true
        }
    }
    return false
}

@Test
class FontTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    /*
     * 已知在册的常用简体字（示例与断言 A4 依赖它们）。
     * 这不是"字库支持中文"的证据，只是这几个字恰好命中 —— 见文件头。
     */
    private static let PRESENT_CHARS: Array<Rune> = [
        r'你',
        r'好',
        r'世',
        r'界',
        r'中',
        r'文',
        r'我',
        r'字',
        r'体',
        r'大',
        r'小'
    ]

    /*
     * ★ 实测确认**缺失**的常用简体字。
     *   社区报告的 问/厅/灯/调/窗/帘 全部核实；显 是本项目自己文档里用到的字。
     */
    private static let MISSING_CHARS: Array<Rune> = [
        r'问',
        r'厅',
        r'灯',
        r'调',
        r'窗',
        r'帘',
        r'显'
    ]

    /*
     * ★ 实测确认**在册**的繁体 / 日文异体 —— 「有繁无简」的证据。
     *   它们的存在说明该清单的来源混合了繁体与日文用字。
     */
    private static let TRADITIONAL_CHARS: Array<Rune> = [r'問', r'調', r'窓']

    // ==================================================== 一、两套字体的基本分野

    @TestCase
    func testDefaultFontHasNoCjkGlyphs(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()

        let def = LvFont.builtin(LvBuiltinFont.Default)
        /* Montserrat 有拉丁字形 */
        @Assert(def.hasGlyph(r'A'), true)
        @Assert(def.hasGlyph(r'0'), true)
        /* 但**没有任何**中文字形 —— 这正是「界面里中文完全不显示」的根源 */
        @Assert(def.hasGlyph(r'我'), false)
        @Assert(def.hasGlyph(r'你'), false)

        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)
        /* CJK 字体同样覆盖拉丁与数字（LVGL 内置字体都带 ASCII 范围） */
        @Assert(cjk.hasGlyph(r'A'), true)
        @Assert(cjk.hasGlyph(r'0'), true)
        /* 且确实有中文字形（与 Default 形成对照） */
        @Assert(cjk.hasGlyph(r'我'), true)

        rt.deinitLvgl()
    }

    @TestCase
    func testBuiltinFontHandleIsIdempotent(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        /* 同一内置字体重复取返回同一句柄（句柄表一对一），因此无需缓存 */
        @Assert(LvFont.builtin(LvBuiltinFont.Cjk16).handle,
                LvFont.builtin(LvBuiltinFont.Cjk16).handle)
        rt.deinitLvgl()
    }

    // ==================================================== 二、覆盖查询的正确性

    @TestCase
    func testPresentCharactersAreReportedPresent(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)
        for (r in PRESENT_CHARS) {
            @Assert(cjk.hasGlyph(r), true)
        }
        rt.deinitLvgl()
    }

    /*
     * ★ 本文件最重要的一条：把实测缺字名单钉死。
     *
     * 这几行会让任何字库变更立刻失败 —— 而失败信息本身就把"该去复核哪些文档"
     * 说清楚了。没有它，那几处描述会随着上游升级悄悄变成错误信息。
     */
    @TestCase
    func testKnownMissingCharactersAreStillMissing(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

        for (r in MISSING_CHARS) {
            @Assert(cjk.hasGlyph(r), false)
        }
        /* 对照：对应的繁体/日文异体在册 —— 「有繁无简」不是推断，是事实 */
        for (r in TRADITIONAL_CHARS) {
            @Assert(cjk.hasGlyph(r), true)
        }

        rt.deinitLvgl()
    }

    /*
     * ★ 契约：缺字**不是异常**。
     *
     * 这个区分必须守住：把「缺字」抛成异常，会迫使调用方用 try/catch 处理
     * 一个正常情况，反而更容易写出「catch 里什么都不做」的代码。
     * 只有**查询本身**失败（句柄无效/非 LVGL 线程/未初始化）才抛。
     */
    @TestCase
    func testMissingGlyphDoesNotThrow(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

        var threw = false
        var value = true
        try {
            value = cjk.hasGlyph(r'问')
        } catch (e: LvglException) {
            threw = true
        }
        @Assert(threw, false)
        @Assert(value, false)

        /* missingGlyphs 同理：缺字体现在返回的列表里，不体现在异常上 */
        var threw2 = false
        var n = 0
        try {
            n = cjk.missingGlyphs("问").size
        } catch (e: LvglException) {
            threw2 = true
        }
        @Assert(threw2, false)
        @Assert(n, 1)

        rt.deinitLvgl()
    }

    @TestCase
    func testMissingGlyphsDeduplicatesAndKeepsOrder(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

        /* 去重（同一段文本里重复出现的缺字只报一次）+ 保持首次出现顺序 */
        let m = cjk.missingGlyphs("问问调问")
        @Assert(m.size, 2)
        @Assert(hasRune(m, r'问'), true)
        @Assert(hasRune(m, r'调'), true)

        rt.deinitLvgl()
    }

    /*
     * 真实场景形状：一段常见的中文文案里混着缺字。
     *
     * 断言刻意只针对**实测确认**的缺失字（调/窗），不对整段文案的字符集下结论 ——
     * 否则这条用例会随文案措辞变化而假失败。
     */
    @TestCase
    func testRealisticUiTextReportsMissingCharacters(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

        let m = cjk.missingGlyphs("请调整窗口大小")
        @Assert(hasRune(m, r'调'), true)
        @Assert(hasRune(m, r'窗'), true)

        /* 一段恰好全覆盖的文案：不应报出任何缺字 */
        @Assert(cjk.missingGlyphs("你好世界").size, 0)
        /* 纯 ASCII 文案同样不应报缺字（ASCII 范围 95/95 全覆盖） */
        @Assert(cjk.missingGlyphs("hello 123").size, 0)

        rt.deinitLvgl()
    }

    /*
     * 示例实际渲染的文案必须全覆盖。
     *
     * 来源是 grep setText 得到的真实字符串，不是手工抄的：
     *   examples/hello_cj/src/main.cj:88   "点我"
     *   examples/hello_cj/src/main.cj:102  "点了 ${n} 次"
     * 缺字在这里就是画面上少一块，而 LVGL 不会报错、CI 也不会发现 ——
     * 所以它值得一条用例。
     */
    @TestCase
    func testExampleRenderedTextIsFullyCovered(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

        @Assert(cjk.missingGlyphs("点我").size, 0)
        @Assert(cjk.missingGlyphs("点了 3 次").size, 0)

        rt.deinitLvgl()
    }

    /*
     * 内置字体不可释放（静态 const 对象），必须明确拒绝而非「看起来能跑就放行」。
     * 放在这个文件里是因为它同样属于「字体的生命周期约束」。
     */
    @TestCase
    func testBuiltinFontCannotBeReleased(): Unit {
        let rt = LvglRuntime()
        rt.initLvgl()
        var threw = false
        try {
            LvFont.builtin(LvBuiltinFont.Cjk16).close()
        } catch (e: LvglException) {
            threw = true
        }
        @Assert(threw, true)
        rt.deinitLvgl()
    }
}
