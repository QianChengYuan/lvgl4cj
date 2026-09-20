/*
 * runtime_test.cj —— LvglRuntime 与任务队列的测试（设计文档 §10.3 / §3.9 / §6.1）
 *
 * 覆盖：
 *   · headless 启动 → 有 display（G1 硬前置）→ LvDisplay 信息（缓冲由 C 侧持有）
 *   · 配置哈希一致性（Conf_HASH 与实际二进制一致，§3.5.3）
 *   · ★ 任务队列的**完整往返**：post → pump → 闭包真的在 LVGL 线程被执行
 *   · ★ 一次性闭包回收：任务执行后闭包表回到基线（不泄漏，§11.3）
 *   · 任务执行顺序（FIFO）
 *   · 在 LVGL 线程上 postAndWait 被等待图检测拒绝（返回 DeadlockRisk，而不是挂死）
 *   · 生命周期幂等（重复 start / 重复 deinit）
 *
 * ===================== 本文件刻意不做的事 =====================
 * **不测试「任务体抛异常」**。原因见 docs/C4_DEFECT.md：
 *   在「C → 仓颉」回调帧内**构造**异常对象会触发仓颉 1.1.0 运行时缺陷（SIGSEGV），
 *   而任务正是在该帧内执行的。P0 的结论是「回调内不构造异常」，
 *   因此这里不去踩它 —— 需要覆盖的异常吞并路径已由 closure_test.cj 用
 *   「预构造异常」的方式验证过（那里避开了构造时机）。
 *
 * 每个用例自带 start/deinit，不共用 @BeforeAll 的 LVGL 状态：
 *   runtime 本身包含初始化决策，测试它就必须各自独立，否则测的是上一个用例的残留状态。
 */
package lvgl4cj

import std.collection.ArrayList
import lvgl4cj.generated.{CONF_HASH}

@Test
class RuntimeTests {
    @BeforeEach
    func bindThread(): Unit {
        /* 与 closure_test 同样的理由：M:N 迁移下每个用例都要重新声明线程归属 */
        bindCurrentThreadAsLvgl()
    }

    // ---------------------------------------------------------- 生命周期与 G1

    @TestCase
    func testHeadlessStartProvidesDisplay(): Unit {
        let rt = LvglRuntime()
        let disp = rt.startHeadless(320, 240)

        /*
         * ★ G1：没有 display 时 lv_screen_active() 返回 0，而 lv_obj_create(NULL)
         *   会触发 LVGL 断言并终止进程。所以「启动后拿得到活动屏幕」是能建对象的前提。
         */
        @Assert(rt.activeScreen() != 0, true)
        @Assert(rt.isRunning, true)
        @Assert(rt.currentMode, RunMode.Pumped)

        /* 缓冲由 C 侧分配（ADR-002）：仓颉侧只读到尺寸信息，拿不到也不需要它 */
        @Assert(disp.bufferBytes() > 0, true)
        @Assert(disp.bufferStride() >= 640, true) /* 320 像素 × RGB565 2 字节 */
        @Assert(disp.colorFormat(), LV_COLOR_FORMAT_RGB565)

        disp.close()
        rt.deinitLvgl()
        @Assert(rt.isRunning, false)
    }

    @TestCase
    func testLifecycleIdempotent(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(64, 64)
        /* 重复启动不应报错（内部幂等），也不应重复建 display */
        rt.startHeadless(64, 64)
        @Assert(rt.isRunning, true)

        rt.deinitLvgl()
        /* 重复反初始化同样幂等 */
        rt.deinitLvgl()
        @Assert(rt.isRunning, false)
    }

    @TestCase
    func testConfigHashMatchesBinary(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(32, 32)
        /*
         * §3.5.3：startHeadless 内部的 verifyConfigCompat 已经校验过；
         * 这里再显式断言一次，让「库与仓颉侧看到同一份 lv_conf」这件事
         * 在测试里有可见的证据，而不是只隐含在「没抛异常」里。
         */
        @Assert(confHash(), CONF_HASH)
        rt.deinitLvgl()
    }

    // ---------------------------------------------------------- 任务队列往返

    @TestCase
    func testPostRunsOnPump(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(64, 64)

        /*
         * ★ 注意这里用「let 持有的可变容器」，而不是 `var executed = false`。
         *   原因是仓颉的一条语言约束：**捕获可变局部变量的 lambda 不能逃逸**
         *   （编译报错 "lambda capturing mutable variables needs to be called directly"）。
         *   rt.post { } 的闭包是逃逸的（它要被存进队列、稍后在别的帧执行），
         *   因此不能直接写 `var f = false; rt.post { f = true }`。
         *   这是**用户也会遇到**的约束，已写入 LvglRuntime.post 的文档注释。
         */
        let executed = ArrayList<Int32>()
        rt.post {
            executed.add(1)
        }

        /* 投递只是入队，尚未执行 —— 这正是「跨线程不直接调用」的核心语义 */
        @Assert(executed.size, 0)
        @Assert(taskQueueSize() >= 1, true)

        /* 推进一帧即执行 */
        @Assert(rt.pump(), 0)
        @Assert(executed.size, 1)
        @Assert(taskQueueSize(), 0)

        rt.deinitLvgl()
    }

    @TestCase
    func testTaskClosureIsRecycled(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(64, 64)

        /*
         * ★ 这条是「一次性闭包」机制的核心断言：
         *   任务执行完必须把闭包表项摘掉，否则每投递一个任务就泄漏一条记录。
         *   §11.3 要求闭包表大小可归零，因此这里比的是**基线前后一致**。
         */
        let before = closureCount()

        for (_ in 0..8) {
            rt.post {}
        }
        @Assert(closureCount() > before, true) /* 排队期间确实占用了 cid */

        rt.pump()
        @Assert(closureCount(), before) /* 执行后全部回收 */

        rt.deinitLvgl()
    }

    @TestCase
    func testTaskOrderIsFifo(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(64, 64)

        /* 同样用 let 容器而非 var 计数（见 testPostRunsOnPump 的说明） */
        let order = ArrayList<Int64>()
        for (i in 0..5) {
            rt.post {
                order.add(i)
            }
        }
        rt.pump()

        @Assert(order.size, 5)
        var ok = true
        for (i in 0..5) {
            if (order[i] != i) {
                ok = false
            }
        }
        @Assert(ok, true)

        rt.deinitLvgl()
    }

    @TestCase
    func testPostAndWaitRejectedOnLvglThread(): Unit {
        let rt = LvglRuntime()
        /* headless 是「调用方驱动」，因此当前线程**就是** LVGL 线程 */
        rt.startHeadless(64, 64)
        @Assert(rt.isLvglThread(), true)

        /*
         * ★ §3.8.4 等待图检测：在 LVGL 线程上同步等待自己要执行的任务必然死锁，
         *   正确行为是**立刻返回 DeadlockRisk**，而不是挂住整个线程。
         *   用 tryPostAndWait 拿错误码，避免在非回调帧外构造异常。
         */
        let rc = rt.tryPostAndWait {}
        @Assert(rc, lvglErrorToCode(DeadlockRisk))
        @Assert(rt.isRunning, true) /* 关键：没有挂死，进程仍在正常状态 */

        rt.deinitLvgl()
    }

    @TestCase
    func testQueuePolicyAndCapacityAreWired(): Unit {
        let rt = LvglRuntime()
        rt.startHeadless(64, 64)

        /* 这两个入口直连 C 侧队列（§3.9），验证接线正确且策略可设 */
        setTaskQueueCapacity(8)
        setTaskQueueFullPolicy(QueueFullPolicy.FailFast)

        /* 用 let 容器承载计数，理由同 testPostRunsOnPump（逃逸闭包不能捕获 var） */
        let n = ArrayList<Int32>()
        for (_ in 0..3) {
            rt.post {
                n.add(1)
            }
        }
        rt.pump()
        @Assert(n.size, 3)

        /* 容量恢复默认，避免影响同一进程内的后续用例 */
        setTaskQueueCapacity(1024)
        rt.deinitLvgl()
    }
}
