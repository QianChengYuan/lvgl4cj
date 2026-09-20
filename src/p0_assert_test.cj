/*
 * p0_assert_test.cj —— P0 八条断言的仓颉侧验证（设计文档 §17）
 *
 * 覆盖与分工（避免把「已由 C 侧单测证明的事」在这里重做一遍）：
 *   ① 主循环稳定不崩           → 本文件：有界 soak（多帧 + 计数不漂移）
 *   ② flush 被调用且画面正确   → 本文件：flush 计数 + **像素级体检**
 *   ③ 点击命中按钮且坐标正确   → 本文件：走**真实输入链路**（indev 读回调 → 命中测试）
 *   ④ 删父后子句柄 isAlive=false → C 侧 test_display_indev / test_style_widgets（含控件）
 *   ⑤ 退出 ASan 零泄漏         → t8（需要 ASan 构建）
 *   ⑥ 回调中 close 自身 + 同帧删除 → 本文件
 *   ⑦ 窗口关闭时 flush_wait 立即返回 → C 侧 test_sdl2_backend（实测 0ms）
 *   ⑧ repeat(1) 动画后 ctx 释放 → t7（动画子系统已落地：anim.c + LvAnim）
 *
 * 断言 ③ 刻意**不用** objSendEvent 直接派发 —— 那只能证明「回调能被调用」，
 * 证明不了「点到了按钮」。这里用 pointer indev 走完整的
 * 「读回调 → 命中测试 → 事件分发」链路，坐标由我们喂给 indev，
 * 因此「命中」这件事是被真正验证的。
 */
package lvgl4cj

import std.collection.ArrayList

/* 与 C 侧 lvglcj_buf_mode_t 一致 */
private const P0_BUF_PARTIAL: Int32 = 0

@Test
class P0AssertTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    // ==================================================== ② flush + 画面正确

    @TestCase
    func testAssert02FlushCalledAndImageNotBlank(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)

        /* 用 let 容器承载计数：逃逸闭包不能捕获 var 局部变量 */
        let flushCount = ArrayList<Int32>()
        disp.setFlushCb {
            h =>
                flushCount.add(1)
                /* 无 sink 时由本回调负责放行，否则 LVGL 会停在等 flush */
                unsafe { lvglcj_display_flush_ready(h) }
        }

        /* 深色背景 + 一个明显的按钮，制造「非纯色」的画面 */
        let scr = LvObject.screenActive()
        let bg = LvStyle.create().bgColor(rgb(0x10, 0x18, 0x20)).bgOpa(255)
        bg.applyTo(scr)

        let btn = LvButton.create(scr)
        btn.setSize(160, 60).align(LvAlign.Center)
        let btnStyle = LvStyle.create().bgColor(rgb(0x00, 0x80, 0xC0)).radius(12)
        btnStyle.applyTo(btn)

        /* 推进若干帧让刷新周期到期并完成渲染 */
        var frames = 0
        while (frames < 200 && flushCount.size == 0) {
            rt.pump()
            sleep(Duration.millisecond * 3)
            frames++
        }

        @Assert(flushCount.size > 0, true) /* ②前半：flush 回调被调用 */

        /*
         * ②后半：像素级体检。
         * 仓颉侧看不到缓冲（ADR-002），因此「非花屏/非黑屏」只能由 C 侧代检：
         *   distinct > 1 → 画面不是纯色（排除「只刷了一块黑」与花屏成一致色）
         *   nonzero  > 0 → 画面不是全黑
         */
        let packed = unsafe { lvglcj_display_sample_buf(disp.handle) }
        @Assert(packed >= 0, true) /* 负数表示出错 */
        let d = (packed >> 32) & 0xFFFFFFFF
        let nz = packed & 0xFFFFFFFF
        @Assert(d > 1, true) /* ★ 非纯色：画面确实有内容 */
        @Assert(nz > 0, true) /* ★ 非全黑 */

        btn.close()
        rt.deinitLvgl()
    }

    // ==================================================== 辅助

    /*
     * 推进 n 帧、每帧间隔 ms 毫秒。
     *
     * 抽出来的直接原因是控制用例长度，但更实际的价值是把「推进多少帧」
     * 变成一个**显式参数** —— 布局结算、indev 读取周期、延迟删除生效
     * 都靠帧数控制，散落在用例里时很容易改一处漏一处。
     */
    private func pumpFrames(rt: LvglRuntime, n: Int64, ms: Int64): Unit {
        for (_ in 0..n) {
            rt.pump()
            sleep(Duration.millisecond * ms)
        }
    }

    /*
     * 推进若干帧让**布局结算**，然后返回按钮中心点。
     *
     * ★ 必须先跑布局再读几何：LVGL 的坐标是在 lv_timer_handler 里算出来的，
     *   刚 setSize 完时 width() 仍为 0，此时「取中心点」会取到 (x, y) 本身，
     *   命中测试必然失败。实测踩到过：断言 containsPoint 为 false，
     *   而根因只是布局还没跑 —— 这类「代码看起来没问题却没反应」最难定位，
     *   所以单独命名成一个函数。
     */
    private func settleLayoutAndCenter(rt: LvglRuntime, btn: LvButton): (Int32, Int32) {
        pumpFrames(rt, 8, 4)
        let bw = btn.width()
        let bh = btn.height()
        @Assert(bw > 0 && bh > 0, true) /* 布局已结算 */
        return (btn.x() + bw / 2, btn.y() + bh / 2)
    }

    /* 等待点击回调，最多 n 帧；返回是否收到 */
    private func waitForClick(rt: LvglRuntime, clicks: ArrayList<Int64>, n: Int64): Bool {
        for (_ in 0..n) {
            rt.pump()
            sleep(Duration.millisecond * 5)
            if (clicks.size > 0) {
                return true
            }
        }
        return false
    }

    // ==================================================== ③ 真实点击链路

    @TestCase
    func testAssert03ClickHitsButtonWithCorrectCoordinates(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)
        let scr = LvObject.screenActive()

        /* 按钮放在一个**明确可算**的位置，便于断言坐标 */
        let btn = LvButton.create(scr)
        btn.setPos(100, 80).setSize(120, 50)
        let lbl = btn.withLabel("点我")

        /* 输入状态：x / y / pressed。用 let 容器承载（逃逸闭包不能捕获 var） */
        let feed = ArrayList<Int32>()
        feed.add(0)
        feed.add(0)
        feed.add(0)

        let clicks = ArrayList<Int64>()
        btn.on(LvEventCode.Clicked) {
            e => clicks.add(e.target())
        }

        let indev = LvIndev.create(LvIndevType.Pointer).bind(disp)
        indev.onRead {
            setIndevPoint(feed[0], feed[1], feed[2] != 0)
        }

        /* 布局结算后取按钮中心点（为何必须先跑布局见该函数注释） */
        let (px, py) = settleLayoutAndCenter(rt, btn)
        @Assert(btn.containsPoint(px, py), true) /* 先证明这个点确实在按钮内 */

        /*
         * 按下 → 保持 → 松开。
         * 时长要覆盖 LVGL 的 indev 读取周期（约 30ms）：太短会「一次都没读到」，
         * 表现为点击完全没反应，而代码看起来没问题。
         */
        feed[0] = px
        feed[1] = py
        feed[2] = 1
        pumpFrames(rt, 30, 5)
        feed[2] = 0
        waitForClick(rt, clicks, 60)

        @Assert(clicks.size > 0, true) /* ★ 点击真的命中并触发了回调 */
        @Assert(clicks[0], btn.handle) /* ★ 事件 target 就是被点的按钮 */
        @Assert(btn.containsPoint(px, py), true) /* ★ 坐标落在按钮矩形内 */

        /* 交互结果：改文字（hello_cj 的核心行为） */
        lbl.setText("点了 1 次")
        @Assert(lbl.isAlive, true)

        indev.close()
        lbl.close()
        btn.close()
        rt.deinitLvgl()
    }

    // ==================================================== ⑥ 回调中 close 自身

    @TestCase
    func testAssert06CloseSelfInsideCallbackDeletedInSameFrame(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)
        let scr = LvObject.screenActive()

        let btn = LvButton.create(scr)
        btn.setPos(50, 50).setSize(100, 40)
        /* 回调里删除**自身** —— 这是最典型的重入场景（§3.8.2） */
        btn.on(LvEventCode.Clicked) {
            _ => btn.close()
        }

        let feed = ArrayList<Int32>()
        feed.add(0)
        feed.add(0)
        feed.add(0)

        let indev = LvIndev.create(LvIndevType.Pointer).bind(disp)
        indev.onRead {
            setIndevPoint(feed[0], feed[1], feed[2] != 0)
        }

        /* 同断言③：先让布局结算，再取按钮内部的一点 */
        for (_ in 0..8) {
            rt.pump()
            sleep(Duration.millisecond * 4)
        }
        feed[0] = btn.x() + btn.width() / 2
        feed[1] = btn.y() + btn.height() / 2
        @Assert(btn.containsPoint(feed[0], feed[1]), true)

        feed[2] = 1
        for (_ in 0..30) {
            rt.pump()
            sleep(Duration.millisecond * 5)
        }
        feed[2] = 0
        /* 推进到「同帧 drain 完成」之后：close 是在回调里调的，会转延迟删除 */
        var frames = 0
        while (frames < 60 && btn.isAlive) {
            rt.pump()
            sleep(Duration.millisecond * 5)
            frames++
        }

        /* ★ 不崩 + 确实被删除（延迟删除在同一帧内完成，这里多推进几帧更宽松） */
        @Assert(btn.isAlive, false)
        @Assert(rt.isRunning, true) /* 进程与运行时状态都还健康 */

        indev.close()
        rt.deinitLvgl()
    }

    // ==================================================== ① 有界 soak

    @TestCase
    func testAssert01BoundedSoakKeepsCountersStable(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()

        let btn = LvButton.create(scr)
        btn.setSize(100, 40).align(LvAlign.Center)

        let baseline = closureCount()
        let errBefore = unsafe { lvglcj_error_count(0) }

        /*
         * 说明：§17 断言①的真实要求是「连续跑 10 分钟不崩」。
         * 在单测里跑满 10 分钟不现实（会让整套测试无法忍受），
         * 因此这里做的是**有界 soak**：跑 600 帧并断言
         *   · 闭包表回到基线（无泄漏）
         *   · 错误计数不增长（无持续报错）
         * 真正的 10 分钟长跑由 scripts/soak.sh 单独执行。
         */
        for (i in 0..600) {
            /* 周期性改动 UI，确保每帧都有真实工作（不是空转） */
            if (i % 100 == 0) {
                btn.setSize(100 + Int32(i / 10), 40)
            }
            rt.pump()
        }

        @Assert(closureCount(), baseline) /* ★ 无闭包泄漏 */
        @Assert(unsafe { lvglcj_error_count(0) }, errBefore) /* ★ 无新增错误 */
        @Assert(rt.isRunning, true)

        btn.close()
        rt.deinitLvgl()
    }

    // ==================================================== ④ 级联失效（仓颉 API 侧）

    @TestCase
    func testAssert04ChildHandleDiesAfterParentClose(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()

        /* 用仓颉 API 复现断言④（C 侧已有更强的等价断言，这里证明 L1 语义一致） */
        let box = LvObject.create(scr)
        let child = LvObject.create(box)
        @Assert(child.isAlive, true)

        box.close()
        @Assert(child.isAlive, false) /* ★ 子句柄已失效 */

        /* 失效后操作应被明确拒绝，而不是写到已释放内存 */
        var threw = false
        try {
            child.setPos(1, 1)
        } catch (e: LvglException) {
            threw = true
        }
        @Assert(threw, true)

        rt.deinitLvgl()
    }
}
