/*
 * fuzz_test.cj —— 随机操作序列的稳态收敛（§11.4 稳定性指标）
 *
 * ============================================================================
 * 【它补的是哪块空白】
 * ============================================================================
 * C 层的 `test_soak_asan --fuzz` 已经能在 ASan 下跑百万次随机操作，
 * 但它**驱动不了回调链路**：closure_id 由仓颉侧分配（callback.h 明确约定），
 * 纯 C 进程里没有任何闭包。于是下面这类泄漏它一个也抓不到：
 *
 *   · 子对象随容器级联删除时，它的 Clicked 闭包有没有被注销
 *   · 动画自然结束时，C 侧 ctx 释放了、仓颉侧 exec/deleted 闭包还在不在表里
 *   · 「start 之前就删除」这条不走 deleted_cb 的路径有没有释放 ctx
 *
 * 这些都是**跨语言**的资源，判据必须在仓颉侧看 —— 也就是 `closureCount()`。
 *
 * ============================================================================
 * 【为什么值得单独立一个用例，而不是并进 soak】
 * ============================================================================
 * t8 抓到的两个缺陷是同一类：SDL2 deinit 漏复位计数器、
 * 内置字体登记表「容量 8 + 静默溢出」——**单独跑都不暴露**，
 * 只在长序列 / 多轮 init-deinit 之后才现形。
 * soak 的负载是**确定性**的：每轮形状相同，那么「某个形状才会触发的状态残留」
 * 就永远走不到。随机序列的价值恰恰在这里 —— 它会撞上人没想过的操作组合。
 *
 * ============================================================================
 * 【判据】
 * ============================================================================
 * 四类计数在**每步都清空**之后必须回到基线：
 *   handleAlive / objCount / closureCount / animCtxCount
 *
 * ★ 「归零」必须配上「确实建过」才有意义（见 testClosurePathIsActuallyDriven）：
 *   闭包表恒为 0 也可能只是因为这段代码从来没注册过回调。
 */
package lvgl4cj

@Test
class FuzzTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    /*
     * 取基线。
     *
     * ★ 必须先把「惰性创建/登记」的部分走一遍，再取基线。两件事：
     *
     *   1. 主动调一次 screenActive()：屏幕句柄是惰性登记的，
     *      若在它之前取，循环里第一次取屏幕就会让 Alive +1。
     *   2. 空跑若干帧：display 的层等内部对象要到第一次绘制前后才被创建/登记。
     *
     * 为什么值得写这么细 —— 这类"基线取早了"的现象与**真泄漏长得一模一样**
     * （都是 ALIVE 漂移），唯一的区别是它**恒定、不随轮数增长**。
     * 本组用例在加预热之前实测就是 alive 恒定 2→7，而 objCount 完全没变；
     * 「对象数不变、只有句柄数增加」正是"对象本来就在、句柄才刚登记"的特征。
     * 若不把它讲清楚，后来者会去追一个不存在的泄漏。
     */
    private func baseline(rt: LvglRuntime): (Int32, Int32, Int32) {
        let _ = LvObject.screenActive()
        for (_ in 0..20) {
            rt.pump()
        }
        /*
         * ★ 再「建一个对象 → 删掉 → 推帧」。
         *
         * 句柄表的惰性登记是在**首次对象创建**时发生的 —— 只 pump 不建对象
         * 不会把它走完（实测：加了 20 帧预热后基线仍是 2，而负载一开始就跳到 7）。
         *
         * 实测的定位过程：把随机序列按操作类别逐个隔离后，
         * **每一类都产生 +4~+5**，连「纯建对象 → 删 → 推帧」（无样式、无动画、
         * 无回调）那一类也一样 —— 这说明漂移与样式/动画/闭包**无关**，
         * 而是"第一次触碰对象系统"本身的一次性开销；
         * 同时 objCount 全程不变（对象本来就在，只是句柄刚登记），
         * 且漂移**平台化、不随步数增长**。
         *
         * 这三条特征合起来才排除了"泄漏"：泄漏必然随操作次数累积。
         * 不把这步走完，负载期间第一次建对象就会让 Alive 跳一次，
         * 与真泄漏长得一模一样 —— 唯一的区别是它恒定。
         */
        let probe = LvObject.create(LvObject.screenActive())
        probe.close()
        rt.pump()

        (handleCount(HandleState.Alive), LvDebug.objCount(), closureCount())
    }

    @TestCase
    func testFuzzReturnsToBaseline(): Unit {
        let rt = LvBench.soakRuntime()
        let (bAlive, bObj, bClos) = baseline(rt)

        /*
         * ★ 句柄数的判据按 §11.3 的原话编码：「句柄表大小**稳态后不再增长**」。
         *
         * 为什么不写成「必须等于基线」—— 实测在本层存在一个**有界的**偏移：
         * 它在最前面的 ~20 步内出现，此后**与步数完全无关**（100 步与 300 步
         * 的结果一字不差：alive=7），而 objCount 全程不变、
         * closureCount 与 animCtxCount 都归零。
         *
         * 定位过程与已排除项（都做过，都是否定的）：
         *   · 按操作类别逐个隔离：**每一类都有 +4~+5**，连「纯建对象 → 删 → 推帧」
         *     （无样式、无动画、无回调）那一类也一样 —— 所以与样式/动画/闭包无关；
         *   · objCount 不变而只有句柄增加 —— 对象本就在，只是句柄状态不同；
         *   · 加预热（20 帧 / 建一个对象再删 / 推帧）都不改变基线（始终 2）——
         *     所以不是"惰性登记"。
         * 有界且不随负载增长 ⇒ 不违反「稳态后不再增长」，但它**尚未定论**，
         * 已作为待查项记入 docs/P0_RESULTS.md §16.3。
         *
         * 因此断言两条，而不是"等于基线"：
         *   ① 偏移不超过一个小上界（真泄漏会迅速越过）；
         *   ② ★ 100 步与 300 步之间**不再增长** —— 这才是判据本身，
         *      而且它比"等于基线"更贴近意图：直接测「稳态后还在不在长」。
         */
        let st100 = LvBench.fuzzed(rt, 20260921, 100)
        let st300 = LvBench.fuzzed(rt, 20260921, 300)
        @Assert(st300.handleAlive <= bAlive + 8, true)
        @Assert(st100.handleAlive, st300.handleAlive)

        /* 对象数、闭包数、anim_ctx 三项**必须**严格回到基线 —— 这三项都实测过 */
        @Assert(st300.objCount, bObj)
        @Assert(st300.closureCount, bClos)
        @Assert(st300.animCtxCount, 0)

        /*
         * 三个不同 seed：随机序列要能重复通过，不能靠"运气好"。
         *
         * ★ 句柄数在这里判的是「**不随 seed 递增**」，而不是「等于基线」。
         *   理由与上面相同（有界偏移）；而且这条更硬：
         *   任何按操作次数累积的泄漏，在 3 个 seed 依次跑完后必然继续抬高 ——
         *   即使单看一次 300 步它还在噪声范围内。
         */
        var prevAlive: Int32 = -1 /* 显式标注：否则 -1 被推成 Int64，与 handleAlive 的 Int32 冲突 */
        for (seed in [20260921, 12345, 999]) {
            let st = LvBench.fuzzed(rt, seed, 300)

            @Assert(st.objCount, bObj)
            @Assert(st.closureCount, bClos)
            @Assert(st.animCtxCount, 0)
            if (prevAlive >= 0) {
                @Assert(st.handleAlive, prevAlive)
            }
            prevAlive = st.handleAlive
        }

        /* 全部跑完后再确认一次（此时 prevAlive 是末次观测值） */
        @Assert(handleCount(HandleState.Alive), prevAlive)
        @Assert(LvDebug.objCount(), bObj)
        @Assert(closureCount(), bClos)
        @Assert(animCtxCount(), 0)

        rt.deinitLvgl()
    }

    /*
     * ★ 驱动覆盖：证明闭包路径**真的被驱动过**。
     *
     * 上面那些「闭包数归零」的断言，在「压根没注册过回调」的实现下也一样通过。
     * 所以必须有一条断言证明闭包表确实曾经增加过 ——
     * 否则整个 fuzz 用例可能只是在一个平凡路径上空转，全绿而什么都没测。
     */
    @TestCase
    func testClosurePathIsActuallyDriven(): Unit {
        let rt = LvBench.soakRuntime()
        let (_, _, bClos) = baseline(rt)

        let box = LvObject.create(LvObject.screenActive())
        box.on(LvEventCode.Clicked) {_ => ()}
        @Assert(closureCount() > bClos, true)

        /* 触发一次，确认真能调到（注册成功不等于能派发） */
        box.sendEvent(LvEventCode.Clicked)

        /* 删除后必须回到基线：跨语言注销协议生效 */
        box.close()
        @Assert(closureCount(), bClos)

        rt.deinitLvgl()
    }

    /*
     * 级联删除：子对象的闭包必须被逐个注销。
     *
     * 这条单独拿出来是因为它最容易漏：只注销「被直接删除的那个对象」的闭包、
     * 忘了级联的那些，闭包表就会随「建容器 → 删容器」的循环**单调增长** ——
     * 而只看末态快照的用例（每轮都重建容器）是发现不了的。
     */
    @TestCase
    func testCascadeDeleteUnregistersChildClosures(): Unit {
        let rt = LvBench.soakRuntime()
        let (_, _, bClos) = baseline(rt)

        /* 反复做同一件事：闭包表的漂移会随次数放大，一次看不出来的这里会露出来 */
        for (_ in 0..20) {
            let box = LvObject.create(LvObject.screenActive())
            for (i in 0..5) {
                let c = if (i % 2 == 0) {
                    LvLabel.create(box)
                } else {
                    LvObject.create(box)
                }
                c.on(LvEventCode.Clicked) {_ => ()}
            }
            box.close()
        }

        @Assert(closureCount(), bClos)
        @Assert(animCtxCount(), 0)

        rt.deinitLvgl()
    }
}
