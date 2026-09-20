/*
 * ffi_contract_test.cj —— FFI 契约测试（设计文档 §10.2）
 *
 * 要防的是什么：
 *   仓颉侧的 foreign 声明与 @C struct 是**手写的镜像**，C 侧头文件是权威。
 *   两边一旦漂移（改了结构体字段、改了错误码数值、换了 LVGL 配置却没重新生成常量），
 *   症状会是「莫名其妙的乱码数据」或「间歇性崩溃」，极难定位。
 *   本文件把这类漂移变成 CI 里一条明确的红色断言。
 *
 * 仓颉侧测试约定（cangjie-unittest 技能）：
 *   · 文件名 xxx_test.cj，与 xxx.cj 并列
 *   · 测试模式下 unittest / unittest.testmacro 自动导入，无需显式 import
 */
package lvgl4cj

import lvgl4cj.generated.{CODE_OK, CODE_OK_DEFERRED, CODE_ERR_INVALID_HANDLE, CODE_ERR_NOT_INITIALIZED, 
    CODE_ERR_INVALID_CONFIG, CODE_ERR_CALLBACK_THREW, CODE_ERR_OUT_OF_MEMORY, CODE_ERR_WRONG_THREAD, 
    CODE_ERR_DEADLOCK_RISK, CODE_ERR_QUEUE_FULL, CODE_ERR_BACKEND_FAILURE, CODE_ERR_INVALID_ARGUMENT, 
    CODE_ERR_CLOSURE_EXHAUSTED, CODE_ERR_PENDING_DELETE, CODE_ERR_NOT_SUPPORTED, CODE_ERR_VERSION_MISMATCH, 
    CODE_ERR_DEFERRED_LOOP, CODE_ERR_PIN_EXHAUSTED, CODE_ERR_ANIM_CTX_LOST, HSTATE_UNINIT, HSTATE_ALIVE, 
    HSTATE_INVALIDATED, HSTATE_RELEASED, CONF_HASH, CONF_COLOR_DEPTH, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, 
    LVGL_VERSION_PATCH, C_SIZEOF_AREA, C_SIZEOF_PERF, C_SIZEOF_TREE_DUMP}

/* --------------------------------------------------------- C 侧布局镜像 */
/*
 * 与 lvglcj_bridge.h / lvgl4cj 内部结构一一对应。
 * 字段顺序与类型必须与 C 侧严格一致，sizeOf 必须等于 C 侧实测值。
 */
@C
struct CArea {
    var x1: Int32 = 0
    var y1: Int32 = 0
    var x2: Int32 = 0
    var y2: Int32 = 0
}

/*
 * CPerf / CTreeDump 现在定义在 src/debug.cj（库里）—— 因为 LvDebug 需要用它们，
 * 而测试文件的类型在 `cjpm build` 时不可见。
 * 契约测试因此改为校验**库里那份定义**：测的就是真正被使用的布局，
 * 而不是一份只在测试中存在的副本（后者即使写错也不会影响到生产代码）。
 */
@Test
class FfiLayoutTests {
    @BeforeAll
    func setupOnce() {
        let rc = unsafe { lvglcj_init() }
        @Assert(rc, CODE_OK)
    }

    @BeforeEach
    func bindThread() {
        /* 同 ClosureTableTests：M:N 轻量级线程可能换 OS 线程，每个用例都要重新声明归属 */
        let rc = bindCurrentThreadAsLvgl()
        @Assert(rc, CODE_OK)
    }

    @AfterAll
    func teardownOnce() {
        unsafe { lvglcj_deinit() }
    }

    /* ------------------------------------------------ @C struct 布局 */
    @TestCase
    func testAreaStructLayout() {
        /* 期望 16 字节：4 × int32，无填充 */
        @Assert(Int64(sizeOf<CArea>()), Int64(C_SIZEOF_AREA))
    }

    @TestCase
    func testPerfStructLayout() {
        /* 期望 20 字节：5 × uint32 */
        @Assert(Int64(sizeOf<CPerf>()), Int64(C_SIZEOF_PERF))
    }

    @TestCase
    func testTreeDumpStructLayout() {
        /* 期望 32 字节：3 指针(24) + int32(4) + padding(4)，64 位平台 */
        @Assert(Int64(sizeOf<CTreeDump>()), Int64(C_SIZEOF_TREE_DUMP))
    }

    /* ------------------------------------- 构建期配置与运行库一致性 */
    /*
     * ★ 本文件最有价值的一条断言。
     *   它捕捉的是：C 库被重新构建（换了 lv_conf.h / LVGL 版本），
     *   但 src/generated/conf_const.cj 没有重新生成 ——
     *   此时仓颉侧会按旧假设解释新库的内存布局，是典型的花屏/崩溃来源（R8）。
     */
    @TestCase
    func testConfHashMatchesRuntime() {
        let runtimeHash = unsafe { lvglcj_conf_hash() }
        @Assert(runtimeHash, CONF_HASH)
    }

    @TestCase
    func testVersionMatchesRuntime() {
        @Assert(unsafe { lvglcj_conf_version_major() }, LVGL_VERSION_MAJOR)
        @Assert(unsafe { lvglcj_conf_version_minor() }, LVGL_VERSION_MINOR)
        @Assert(unsafe { lvglcj_conf_version_patch() }, LVGL_VERSION_PATCH)
    }

    @TestCase
    func testColorDepthMatchesRuntime() {
        @Assert(unsafe { lvglcj_conf_color_depth() }, CONF_COLOR_DEPTH)
    }

    /* ------------------------------------------- 冻结数值回归保护 */
    /*
     * 附录 B 的错误码与 §3.1.1 的四态数值是**冻结**的：
     * 测试、文档、用户代码都依赖它们。任何改动都必须先改设计文档。
     */
    @TestCase
    func testFrozenStatusCodes() {
        @Assert(CODE_OK, 0)
        /* 正数是特殊成功标记（@Assert 不支持消息参数，说明写在注释里） */
        @Assert(CODE_OK_DEFERRED, 1)
        @Assert(CODE_ERR_INVALID_HANDLE, -1)
        @Assert(CODE_ERR_NOT_INITIALIZED, -2)
        @Assert(CODE_ERR_INVALID_CONFIG, -3)
        @Assert(CODE_ERR_CALLBACK_THREW, -4)
        @Assert(CODE_ERR_OUT_OF_MEMORY, -5)
        @Assert(CODE_ERR_WRONG_THREAD, -6)
        @Assert(CODE_ERR_DEADLOCK_RISK, -7)
        @Assert(CODE_ERR_QUEUE_FULL, -8)
        @Assert(CODE_ERR_BACKEND_FAILURE, -9)
        @Assert(CODE_ERR_INVALID_ARGUMENT, -10)
        @Assert(CODE_ERR_CLOSURE_EXHAUSTED, -11)
        @Assert(CODE_ERR_PENDING_DELETE, -12)
        @Assert(CODE_ERR_NOT_SUPPORTED, -13)
        @Assert(CODE_ERR_VERSION_MISMATCH, -14)
        @Assert(CODE_ERR_DEFERRED_LOOP, -15)
        /* -16 已归档：分支 A 未采用，但数值保留以维持与 C 侧一一对应 */
        @Assert(CODE_ERR_PIN_EXHAUSTED, -16)
        @Assert(CODE_ERR_ANIM_CTX_LOST, -17)
    }

    @TestCase
    func testFrozenHandleStates() {
        @Assert(HSTATE_UNINIT, 0)
        @Assert(HSTATE_ALIVE, 1)
        @Assert(HSTATE_INVALIDATED, 2)
        @Assert(HSTATE_RELEASED, 3)
    }

    /* -------------------------------------------- 错误码映射双向一致 */
    @TestCase
    func testErrorCodeRoundTrip() {
        @Assert(lvglErrorToCode(lvglErrorFromCode(CODE_ERR_INVALID_HANDLE)), CODE_ERR_INVALID_HANDLE)
        @Assert(lvglErrorToCode(lvglErrorFromCode(CODE_OK_DEFERRED)), CODE_OK_DEFERRED)
        @Assert(lvglErrorToCode(lvglErrorFromCode(CODE_ERR_ANIM_CTX_LOST)), CODE_ERR_ANIM_CTX_LOST)
        /* 未知码不应丢失原始数值 */
        @Assert(lvglErrorToCode(lvglErrorFromCode(-9999)), -9999)
    }

    @TestCase
    func testErrorNameStable() {
        @Assert(lvglErrorName(InvalidHandle), "InvalidHandle")
        @Assert(lvglErrorName(OkDeferred), "OkDeferred")
    }

    @TestCase
    func testSuccessCodeClassification() {
        @Assert(lvglErrorIsSuccessCode(CODE_OK))
        /* 延迟删除返回 +1，不是错误 */
        @Assert(lvglErrorIsSuccessCode(CODE_OK_DEFERRED))
        @Assert(!lvglErrorIsSuccessCode(CODE_ERR_INVALID_HANDLE))
    }

    /* ------------------------------------------------ 句柄值类型语义 */
    @TestCase
    func testNullHandleSemantics() {
        let h = LvHandle.empty()
        @Assert(h.isNull())
        @Assert(!h.isAlive())
        /* 空句柄视为已关闭 */
        @Assert(h.isClosed())
        @Assert(h.state(), HandleState.Uninit)
    }

    @TestCase
    func testHandleStateMapping() {
        @Assert(handleStateFromCode(HSTATE_ALIVE), HandleState.Alive)
        @Assert(handleStateFromCode(HSTATE_INVALIDATED), HandleState.Invalidated)
        @Assert(handleStateFromCode(HSTATE_RELEASED), HandleState.Released)
        /* 未知数值安全降级为 Uninit */
        @Assert(handleStateFromCode(12345), HandleState.Uninit)
    }

    @TestCase
    func testThrowIfErrorSemantics() {
        /* OK 与 OK_DEFERRED 都不应抛异常 */
        throwIfError(CODE_OK, "test")
        throwIfError(CODE_OK_DEFERRED, "test")
        @Assert(true)
    }

    @TestCase
    func testThrowIfErrorThrows() {
        var caught = false
        try {
            throwIfError(CODE_ERR_INVALID_HANDLE, "test")
        } catch (e: LvglException) {
            caught = true
            @Assert(e.code, InvalidHandle)
        }
        /* 非零错误码必须抛出 LvglException */
        @Assert(caught)
    }
}
