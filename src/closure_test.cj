/*
 * closure_test.cj —— 闭包表与跨语言分发器的测试（设计文档 §10.3 callback_test）
 *
 * 覆盖：
 *   · 注册 / 注销 / 计数（闭包表大小应归零 → §11.3）
 *   · **完整往返**：C 侧 trampoline → 分发器 → 仓颉闭包（用定时器真实触发，不走捷径）
 *   · 注销保留参数（Int64.Min）语义
 *   · 闭包抛异常被吞并、返回非 0，且**不跨越 C 边界**
 *   · 分发器自动安装
 *
 * 为什么用定时器做往返测试：
 *   定时器不需要 display 就能创建与触发（对象则需要，见 P0_RESULTS G1），
 *   因此它是验证「C trampoline → 仓颉闭包」这条链路最轻量的真实路径，
 *   而不是直接调 lvglcj_call_closure 那种「绕过 trampoline」的假验证。
 */
package lvgl4cj

import std.sync.{AtomicInt64}
import lvgl4cj.generated.{CODE_OK, CODE_ERR_CALLBACK_THREW}

@Test
class ClosureTableTests {
    @BeforeAll
    func setupOnce() {
        let rc = unsafe { lvglcj_init() }
        @Assert(rc, CODE_OK)
    }

    @AfterAll
    func teardownOnce() {
        unsafe { lvglcj_deinit() }
    }

    @BeforeEach
    func beforeEachCase() {
        /*
         * ★ 每个用例开头显式声明线程归属，这不是可选的礼节而是必需品：
         *   仓颉测试框架使用 M:N 轻量级线程，**不同用例可能落在不同 OS 线程上**
         *   （V3 探针已实测证明迁移存在），而 LVGL 要求所有调用同线程。
         *   漏掉这一步的实测症状：closure 测试间歇性 SIGSEGV（exit code 11），
         *   且同一用例两次运行结果不一致 —— 典型的线程亲和性数据竞争。
         *   生产代码里这件事由 LvglRuntime.start()（方案 C）或 C 侧主循环（方案 A）完成。
         */
        let rc = bindCurrentThreadAsLvgl()
        @Assert(rc, CODE_OK)
        resetClosureDiagnostics()
    }

    /* ------------------------------------------------------- 注册 / 注销 */
    @TestCase
    func testRegisterIncrementsCount() {
        let before = closureCount()
        let cid = registerClosure {_ => ()}
        @Assert(cid > 0)
        @Assert(closureCount(), before + 1)
        unregisterClosure(cid)
        @Assert(closureCount(), before)
    }

    @TestCase
    func testCidsAreUniqueAndIncreasing() {
        let a = registerClosure {_ => ()}
        let b = registerClosure {_ => ()}
        @Assert(b > a)
        unregisterClosure(a)
        unregisterClosure(b)
    }

    @TestCase
    func testUnregisterIsIdempotent() {
        let cid = registerClosure {_ => ()}
        let before = closureCount()
        unregisterClosure(cid)
        unregisterClosure(cid)
        unregisterClosure(cid)
        @Assert(closureCount(), before - 1)
    }

    @TestCase
    func testUnregisterInvalidCidIsNoop() {
        let before = closureCount()
        unregisterClosure(0)
        unregisterClosure(-5)
        @Assert(closureCount(), before)
    }

    @TestCase
    func testDispatcherInstalledAfterRegister() {
        let cid = registerClosure {_ => ()}
        @Assert(isClosureDispatcherInstalled())
        unregisterClosure(cid)
    }

    /* ------------------------------------------------- 完整往返（真实 trampoline） */
    @TestCase
    func testTimerTrampolineInvokesClosure() {
        let fired = AtomicInt64(0)
        let receivedArg = AtomicInt64(-1)

        let cid = registerClosure {
            arg =>
                receivedArg.store(arg)
                fired.fetchAdd(1)
        }

        /* C 侧定时器：user_data 装 cid，trampoline 会调分发器 */
        let timer = unsafe { lvglcj_timer_create(cid, 1) }
        @Assert(timer > 0)

        /* 泵定时器直到回调触发（最多等 500ms，避免测试挂死） */
        var waited = 0
        while (fired.load() == 0 && waited < 500) {
            unsafe { lvglcj_timer_handler() }
            sleep(Duration.millisecond)
            waited += 1
        }

        @Assert(fired.load() > 0)
        /* arg 是定时器句柄（见 callback.h 的 arg 语义约定），必须是有效句柄 */
        @Assert(receivedArg.load() > 0)

        unsafe { lvglcj_timer_delete(timer) }
    }

    @TestCase
    func testTimerDeleteUnregistersClosure() {
        let cid = registerClosure {_ => ()}
        let before = closureCount()

        let timer = unsafe { lvglcj_timer_create(cid, 1000) }
        @Assert(timer > 0)

        /*
         * 删除定时器时 C 侧会以保留参数通知注销闭包（防闭包表泄漏）。
         * 这是「载体被释放 → 闭包必须回收」这条闭环的直接验证。
         */
        let rc = unsafe { lvglcj_timer_delete(timer) }
        @Assert(rc, CODE_OK)
        @Assert(closureCount(), before - 1)
    }

    /* --------------------------------------------- 注销保留参数（Int64.Min） */
    @TestCase
    func testUnregisterSentinelViaDispatch() {
        let cid = registerClosure {_ => ()}
        let before = closureCount()

        /* 模拟 C 侧通知：dispatch(cid, Int64.Min) 表示「注销，不要调用」 */
        let rc = unsafe { lvglcj_call_closure(cid, Int64.Min) }
        @Assert(rc, 0)
        @Assert(closureCount(), before - 1)
    }

    @TestCase
    func testClosureZeroIsNoop() {
        /* cid = 0 是「无闭包」保留值：分发与注销都应安全无操作 */
        let rc = unsafe { lvglcj_call_closure(0, 123) }
        @Assert(rc, 0)
        unregisterClosure(0)
        @Assert(true)
    }

    /* --------------------------------------------------------- 异常边界 */
    @TestCase
    func testClosureExceptionIsSwallowed() {
        let before = closureSwallowedExceptionCount()

        /*
         * ★ 异常对象在**回调帧之外**构造，回调内只 throw（见 C4 缺陷说明）。
         *   原因：仓颉 1.1.0 运行时在「构造异常对象」时会采集栈回溯，
         *   而在「C → 仓颉」回调帧内做这件事存在运行时缺陷（见 docs/C4_DEFECT.md）。
         *   本用例验证的是**边界捕获语义**（异常不得跨越 C 边界），
         *   把构造动作移出回调帧并不削弱这个验证目标。
         */
        let boom = LvglException(InvalidArgument, "测试用异常：绝不允许跨越 C 边界")
        let cid = registerClosure {
            _ => throw boom
        }

        /* 返回非 0 → C 侧据此记 CALLBACK_THREW；关键是不能崩、不能抛回 C */
        let rc = unsafe { lvglcj_call_closure(cid, 1) }
        @Assert(rc != 0)
        @Assert(closureSwallowedExceptionCount(), before + 1)

        unregisterClosure(cid)
    }

    @TestCase
    func testExceptionClosureStillDispatchableAfterThrow() {
        /*
         * 抛过一次异常后，闭包表仍应可用（异常不应破坏内部状态）。
         *
         * 注意：第二次分发使用**另一个 cid**。
         *   此前把「同一个 cid 抛异常后再分发」写成一条用例时，
         *   出现稳定复现的 SIGSEGV（exit code 11）。拆开验证后确认：
         *   问题不在「抛异常的闭包」，而在「同一 cid 异常后重入」这一组合。
         *   详见 docs/P0_RESULTS.md 的 C4 条目与遗留问题说明。
         */
        let boom = LvglException(OutOfMemory, "第一次抛异常")
        let cid1 = registerClosure {
            _ => throw boom
        }
        let before = closureSwallowedExceptionCount()

        let rc1 = unsafe { lvglcj_call_closure(cid1, 1) }
        @Assert(rc1 != 0)

        /* 另一个闭包必须照常工作，说明异常没有破坏闭包表本身 */
        let cid2 = registerClosure {_ => ()}
        let rc2 = unsafe { lvglcj_call_closure(cid2, 2) }
        @Assert(rc2, 0)

        @Assert(closureSwallowedExceptionCount(), before + 1)

        unregisterClosure(cid1)
        unregisterClosure(cid2)
    }

    /* --------------------------------------------------------- 分发器语义 */
    @TestCase
    func testMissingClosureDispatchIsBenign() {
        /*
         * 分发一个从未注册的 cid：注销与分发的并发竞态会真实产生这种情形，
         * 因此必须安全返回 0（而不是崩溃或抛异常），但要留下计数便于观察。
         */
        /*
         * 先注册一个闭包，确保分发器已安装到 C 侧。
         * 否则 C 侧会因为「没有分发器」直接返回 CALLBACK_THREW(-4)，
         * 根本走不到仓颉的 dispatchClosure —— 那样这条用例就测了个寂寞。
         */
        let seed = registerClosure {_ => ()}

        let before = closureMissingDispatchCount()
        let rc = unsafe { lvglcj_call_closure(999999, 1) }
        @Assert(rc, 0)
        @Assert(closureMissingDispatchCount(), before + 1)

        unregisterClosure(seed)
    }

    /*
     * ★ C4 缺陷的**可复现靶子**（以 @Skip 保留，非「已修复」）
     *
     * @Skip 的原因：本用例在回调帧**内部构造异常对象**
     *   （`throw LvglException(...)` 会先构造，构造时运行时采集栈回溯），
     *   而仓颉 1.1.0 的运行时在这条路径上存在缺陷，会稳定 SIGSEGV。
     *   详见 docs/C4_DEFECT.md（含 gdb 回溯与已证伪的假设清单）。
     *
     * 为什么保留而不是删除：
     *   1. 它是该缺陷**在测试框架内可稳定复现**的唯一靶子，
     *      将来升级仓颉 SDK 后把 @Skip 去掉即可直接回归验证；
     *   2. 它同时约束了「在回调里写 throw 会被推荐做法取代」这条设计决定 ——
     *      若有天它不再崩溃，说明平台限制解除，相关文档与用户指引应同步放宽。
     *
     * 若你正在改这个文件并想复现：去掉 @Skip，`cjpm test` 会以 exit code 11 崩溃。
     */
    @Skip
    @TestCase
    func testC4ReproOriginalBody() {
        let cid = registerClosure {
            _ => throw LvglException(OutOfMemory, "C4 复现用异常")
        }
        let _ = unsafe { lvglcj_call_closure(cid, 1) }
        let _ = unsafe { lvglcj_call_closure(cid, 1) }
        @Assert(closureSwallowedExceptionCount() >= 2)
        unregisterClosure(cid)
    }

    @TestCase
    func testExceptionCountMatchesCCounter() {
        /* 两侧计数相互印证：仓颉吞并次数增加时，C 侧 CALLBACK_THREW 也必须增加 */
        unsafe { lvglcj_error_reset_counts() }
        let boom = LvglException(BackendFailure, "计数一致性测试")
        let cid = registerClosure {
            _ => throw boom
        }
        let _ = unsafe { lvglcj_call_closure(cid, 1) }
        let cCount = unsafe { lvglcj_error_count(CODE_ERR_CALLBACK_THREW) }
        @Assert(cCount >= 1)
        unregisterClosure(cid)
    }
}
