/*
 * anim_test.cj —— 动画 L1 的测试（设计文档 §3.10 / §17 断言⑧）
 *
 * 覆盖：
 *   · ★ 断言⑧：repeat(1) 动画跑完后**动画上下文被释放**（animCtxCount 归零）
 *   · ★ 同一件事的跨语言侧面：仓颉侧的**闭包表也回到基线** ——
 *     这证明 C 侧的「注销协议」（arg = LVGLCJ_ARG_UNREGISTER）真的把 exec/start/deleted
 *     三个闭包都摘掉了。只看 ctx 归零是不够的：ctx 释放了但闭包留在仓颉表里，
 *     仍然是泄漏（只是换了个地方）。
 *   · exec 回调真的收到插值，且值从起点走向终点
 *   · 手动 close 同样释放上下文
 *   · 目标对象被删除时动画被停掉并释放上下文（ADR-015 代价补偿）
 *   · start 之后改配置被拒绝（「设了但没生效」的静默失效要变成明确报错）
 */
package lvgl4cj

import std.collection.ArrayList

/* null 后端每帧 sleep 5ms，据此把「毫秒」换算成帧数 */
private func pumpMs(rt: LvglRuntime, ms: Int32): Unit {
    /* 显式标注 Int32：否则字面量 1 会推断成 Int64，与 ms/5 的 Int32 冲突 */
    let frames: Int32 = if (ms / 5 < 1) {
        1
    } else {
        ms / 5
    }
    let _ = rt.pumpFrames(frames)
}

@Test
class AnimTests {
    @BeforeEach
    func bindThread(): Unit {
        bindCurrentThreadAsLvgl()
    }

    // ==================================================== 断言⑧

    @TestCase
    func testAssert08RepeatOneReleasesAnimContext(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)
        let scr = LvObject.screenActive()
        let box = LvObject.create(scr)
        box.setSize(50, 50)

        /* 基线：断言结束时闭包表必须回到这个数 */
        let baseClosures = closureCount()

        let execValues = ArrayList<Int32>()
        let startCalls = ArrayList<Int32>()
        let deletedCalls = ArrayList<Int32>()

        let anim = LvAnim
            .create()
            .target(box)
            .values(0, 100)
            .duration(100)
            .path(LvAnimPath.Linear)
            /* repeat(1) = 只跑一趟（0 才是无限循环） */
            .repeat(1)
            .onExec {v => execValues.add(v)}
            .onStart {_ => startCalls.add(1)}
            .onDeleted {=> deletedCalls.add(1)}
        anim.start()

        /* 运行中：ctx 存在，三个闭包都在表里 */
        @Assert(animCtxCount() > 0, true)
        @Assert(closureCount() > baseClosures, true)
        @Assert(anim.isAlive, true)

        /* 推进到自然结束（100ms 动画 + 余量） */
        pumpMs(rt, 400)

        /* ★ 断言⑧的核心 */
        @Assert(animCtxCount(), 0)
        @Assert(animCountRunning(), 0)

        /* ★ 跨语言侧面：闭包表也回到基线（注销协议生效） */
        @Assert(closureCount(), baseClosures)

        /* 回调语义 */
        @Assert(startCalls.size, 1)
        @Assert(deletedCalls.size, 1)
        @Assert(execValues.size > 0, true)
        @Assert(execValues[execValues.size - 1], 100)

        /* 句柄已失效；此时 close 应幂等而不是报错 */
        @Assert(anim.isAlive, false)
        anim.close()

        box.close()
        rt.deinitLvgl()
    }

    // ==================================================== 插值确实发生

    @TestCase
    func testExecReceivesInterpolatingValues(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()
        let box = LvObject.create(scr)

        let values = ArrayList<Int32>()
        LvAnim.create().target(box).values(10, 90).duration(150).repeat(1).onExec {v => values.add(v)}.start()

        pumpMs(rt, 400)

        @Assert(values.size > 1, true) /* 至少被调用了不止一次 */

        /* 单调不减，且首尾贴近区间端点 */
        var monotonic = true
        var i = 1
        while (i < values.size) {
            if (values[i] < values[i - 1]) {
                monotonic = false
            }
            i++
        }
        @Assert(monotonic, true)
        @Assert(values[0] <= 10 + 5, true) /* 起点接近 10 */
        @Assert(values[values.size - 1] >= 90 - 5, true) /* 终点接近 90 */

        box.close()
        rt.deinitLvgl()
    }

    // ==================================================== 手动 close

    @TestCase
    func testManualCloseReleasesContext(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()
        let box = LvObject.create(scr)

        let baseClosures = closureCount()
        let deletedCalls = ArrayList<Int32>()

        let anim = LvAnim
            .create()
            .target(box)
            .values(0, 100)
            .duration(5000) /* 足够长，避免被自然结束抢先 */
            .repeat(1)
            .onExec {_ => ()}
            .onDeleted {=> deletedCalls.add(1)}
        anim.start()

        pumpMs(rt, 40)
        @Assert(animCtxCount(), 1)

        anim.close()
        @Assert(animCtxCount(), 0) /* ★ 手动关闭同样释放 */
        @Assert(deletedCalls.size, 1) /* deleted 回调仍应被通知一次 */
        @Assert(closureCount(), baseClosures)
        @Assert(animCountRunning(), 0)

        box.close()
        rt.deinitLvgl()
    }

    // ==================================================== 对象删除补偿

    @TestCase
    func testObjectDeletionStopsAnimations(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()

        let baseClosures = closureCount()
        let victim = LvObject.create(scr)

        let execA = ArrayList<Int32>()
        let execB = ArrayList<Int32>()

        /* 同一对象上两个动画：var=ctx 让它们各自独立、互不串台 */
        let a1 = LvAnim.create().target(victim).values(0, 100).duration(5000).repeat(0) /* 无限循环：只有补偿才能停掉 */
            .onExec {v => execA.add(v)}
        a1.start()
        let a2 = LvAnim.create().target(victim).values(0, 50).duration(5000).repeat(0).onExec {v => execB.add(v)}
        a2.start()

        pumpMs(rt, 60)
        @Assert(animCtxCount(), 2)
        @Assert(execA.size > 0 && execB.size > 0, true)

        /* ★ 删除对象：DELETE 钩子必须补偿「var != obj 导致自动停动画失效」 */
        victim.close()
        @Assert(animCountRunning(), 0)
        @Assert(animCtxCount(), 0)
        @Assert(closureCount(), baseClosures)
        @Assert(a1.isAlive, false)
        @Assert(a2.isAlive, false)

        rt.deinitLvgl()
    }

    // ==================================================== 启动后再改配置

    @TestCase
    func testConfigAfterStartIsRejected(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(200, 150)
        let scr = LvObject.screenActive()
        let box = LvObject.create(scr)

        let anim = LvAnim.create().target(box).values(0, 10).duration(5000).repeat(1).onExec {_ => ()}
        anim.start()

        /* 重复 start 与启动后改值都应明确报错，而不是静默失效 */
        var threwStart = false
        try {
            anim.start()
        } catch (e: LvglException) {
            threwStart = true
        }
        @Assert(threwStart, true)

        var threwConfig = false
        try {
            anim.values(0, 999)
        } catch (e: LvglException) {
            threwConfig = true
        }
        @Assert(threwConfig, true)

        /* 未设 onExec 就 start 也必须被拒 */
        let bare = LvAnim.create().target(box).values(0, 10).duration(10)
        var threwNoExec = false
        try {
            bare.start()
        } catch (e: LvglException) {
            threwNoExec = true
        }
        @Assert(threwNoExec, true)
        /* 没启动的动画必须手动关掉，否则它注册的东西不会有人回收 */
        bare.close()

        anim.close()
        @Assert(animCtxCount(), 0)
        box.close()
        rt.deinitLvgl()
    }
}
