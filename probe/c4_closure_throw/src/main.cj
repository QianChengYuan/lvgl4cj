/*
 * C4 缺陷最小复现 —— 「回调闭包抛异常后再分发导致 SIGSEGV」
 *
 * 复现要点：
 *   · 每一步都打印**带序号的标记**，崩溃时最后出现的标记就是崩溃点，
 *     这样即使拿不到栈回溯也能先定位到「第几次分发 / 异常路径的哪一段」
 *   · 场景可分别触发（一次只变一个变量），用于区分两个候选触发条件：
 *       (a) 同一 cid 在抛异常后再次被分发
 *       (b) 连续两次抛出异常（各走一遍异常处理路径）
 *   · 全部输出走 stdout 并 flush（println 自带行缓冲语义），
 *     避免「崩溃时最后的输出还没落地」导致误判
 *
 * 为什么必须独立于 cjpm test：
 *   测试框架使用 M:N 轻量级线程、会捕获 stdout/stderr、并有自己的生命周期钩子，
 *   三者都可能成为干扰变量。要判断「是不是仓颉/桥接层的真实缺陷」，
 *   必须先在一个普通可执行程序里排除掉它们。
 */
package c4_closure_throw

import lvgl4cj.*

/*
 * 本模块自己声明需要的 C 符号。
 * 原因：仓颉的 foreign 声明不接受访问修饰符，可见性上限是模块内
 * （见 src/ffi_bridge.cj 的说明），因此跨模块无法复用，必须各自声明。
 */
foreign func lvglcj_init(): Int32
foreign func lvglcj_deinit(): Int32
foreign func lvglcj_call_closure(cid: Int32, arg: Int64): Int32

/* ---------------------------------------------------------------- 工具 */
func mark(tag: String): Unit {
    println("  ${tag}")
}

func throwingClosure(tag: String): (Int64) -> Unit {
    return { arg => throw LvglException(LvglError.InvalidArgument, "${tag}(arg=${arg})") }
}

/* ------------------------------------------------------------ 场景：同一 cid 连续抛 */
func scenarioSameCid(): Unit {
    println("=== 场景 A/C：同一 cid 连续抛异常（原始复现）===")

    mark("[A1] register throwing closure")
    let cid = registerClosure(throwingClosure("A"))
    mark("[A2] cid=${cid} dispatcherInstalled=${isClosureDispatcherInstalled()}")

    mark("[A3] dispatch #1 → 期望被吞并并返回 -4")
    let rc1 = unsafe { lvglcj_call_closure(cid, 1) }
    mark("[A4] rc1=${rc1} swallowed=${closureSwallowedExceptionCount()}")

    mark("[A5] dispatch #2（同一 cid）← 崩溃候选点")
    let rc2 = unsafe { lvglcj_call_closure(cid, 2) }
    mark("[A6] rc2=${rc2} swallowed=${closureSwallowedExceptionCount()}")

    mark("[A7] dispatch #3（同一 cid）")
    let rc3 = unsafe { lvglcj_call_closure(cid, 3) }
    mark("[A8] rc3=${rc3} swallowed=${closureSwallowedExceptionCount()}")

    unregisterClosure(cid)
    mark("[A9] unregistered, live=${closureCount()}")
}

/* ------------------------------------------------------------ 场景：不同 cid 各抛一次 */
func scenarioDiffCids(): Unit {
    println("=== 场景 B：不同 cid 各抛一次（连续两次异常，但不重入同一 cid）===")

    mark("[B1] register two throwing closures")
    let cid1 = registerClosure(throwingClosure("B1"))
    let cid2 = registerClosure(throwingClosure("B2"))
    mark("[B2] cid1=${cid1} cid2=${cid2}")

    mark("[B3] dispatch #1 on cid1")
    let rc1 = unsafe { lvglcj_call_closure(cid1, 1) }
    mark("[B4] rc1=${rc1} swallowed=${closureSwallowedExceptionCount()}")

    mark("[B5] dispatch #2 on cid2（不同 cid）")
    let rc2 = unsafe { lvglcj_call_closure(cid2, 2) }
    mark("[B6] rc2=${rc2} swallowed=${closureSwallowedExceptionCount()}")

    unregisterClosure(cid1)
    unregisterClosure(cid2)
    mark("[B7] unregistered, live=${closureCount()}")
}

/* ------------------------------------------- 场景：抛一次后分发「正常」闭包 */
func scenarioNormalAfter(): Unit {
    println("=== 场景 D：同一 cid 抛一次 → 再分发另一个「正常」闭包 ===")

    let bad = registerClosure(throwingClosure("D-bad"))
    let good = registerClosure { arg => println("        (good 闭包被调用 arg=${arg})") }
    mark("[D1] bad=${bad} good=${good}")

    mark("[D2] dispatch on bad")
    let rc1 = unsafe { lvglcj_call_closure(bad, 1) }
    mark("[D3] rc1=${rc1} swallowed=${closureSwallowedExceptionCount()}")

    mark("[D4] dispatch on good")
    let rc2 = unsafe { lvglcj_call_closure(good, 2) }
    mark("[D5] rc2=${rc2}")

    mark("[D6] dispatch on good 第二次")
    let rc3 = unsafe { lvglcj_call_closure(good, 3) }
    mark("[D7] rc3=${rc3}")

    unregisterClosure(bad)
    unregisterClosure(good)
    mark("[D8] unregistered, live=${closureCount()}")
}

/*
 * 从参数中挑出场景名。
 *
 * 不用 args[1] 这种下标约定：仓颉的 main(args) 是否包含 argv[0]（程序路径）
 * 与调用方式有关，写下标会静默取错。改为**扫描全部参数匹配已知场景名**，
 * 这样无论是否包含 argv[0] 都能正确工作。
 * （上一版就是因为下标假设错误，导致三个场景全部跑了同一分支。）
 */
func pickScenario(args: Array<String>): String {
    let known = ["same_cid", "diff_cids", "normal_after", "outer_try", "prebuilt_throw", "deep_stack"]
    for (a in args) {
        for (k in known) {
            if (a == k) {
                return k
            }
        }
    }
    println("(参数中未找到已知场景名；实际参数：${args})")
    return "same_cid"
}

/* ------------------------------------- 场景 E：外层 try/catch 包裹（模拟测试框架） */
/*
 * 这是与 cjpm test 最可能相关的结构差异：
 *   测试框架会把每个用例体包在自己的 try/catch 里（用于上报失败），
 *   而前面的独立场景**没有**任何外层 try/catch。
 *
 * 因此本场景刻意复刻「外层 try/catch → C 边界 → 仓颉分发器内部 try/catch」
 * 这种**跨 C 边界的嵌套异常处理**结构，检验它是否才是真正的触发条件。
 */
func scenarioOuterTry(): Unit {
    println("=== 场景 E：外层 try/catch 包裹（复刻测试框架的用例包裹）===")

    let cid = registerClosure(throwingClosure("E"))
    mark("[E1] cid=${cid}")

    try {
        mark("[E2] dispatch #1（在 try 内）")
        let rc1 = unsafe { lvglcj_call_closure(cid, 1) }
        mark("[E3] rc1=${rc1}")

        mark("[E4] dispatch #2（在 try 内，同一 cid）")
        let rc2 = unsafe { lvglcj_call_closure(cid, 2) }
        mark("[E5] rc2=${rc2}")
    } catch (e: Exception) {
        println("        [外层 catch 捕获] ${e}")
    }

    mark("[E6] try 块之后")
    unregisterClosure(cid)
    mark("[E7] unregistered, live=${closureCount()}")
}

/* ------------------------- 场景 F：异常对象在回调帧之外预先构造，回调内只 throw */
/*
 * 目的：把「构造异常」与「抛出异常」两个动作拆开，验证究竟哪个才是故障操作。
 *
 * 由 gdb 回溯已知崩溃发生在 Exception::<init> → FillInStackTrace() → 运行时栈回溯器，
 * 即**构造异常对象时采集栈回溯**这一步。若本场景（回调帧之外先构造好异常，
 * 回调内只 throw 已存在的对象）不崩，则可确认：
 *   故障操作 = 「在 C→仓颉 回调帧内构造异常」（含栈回溯采集），
 *   而单纯的 throw/传播是安全的。
 * 这决定了我们该给出什么样的用户侧规避建议。
 */
func scenarioPrebuiltThrow(): Unit {
    println("=== 场景 F：异常在回调帧外预先构造，回调内只 throw ===")

    /* ★ 关键：构造动作发生在这里 —— 此时栈上还没有 sdl/桥接层的 C 帧 */
    let prebuilt = LvglException(LvglError.InvalidArgument, "F-prebuilt")
    mark("[F1] 异常对象已预先构造完成")

    let cid = registerClosure { _ => throw prebuilt }
    mark("[F2] cid=${cid}")

    mark("[F3] dispatch #1（回调内只 throw 预先构造的对象）")
    let rc1 = unsafe { lvglcj_call_closure(cid, 1) }
    mark("[F4] rc1=${rc1} swallowed=${closureSwallowedExceptionCount()}")

    mark("[F5] dispatch #2（同一 cid，再次 throw 同一对象）")
    let rc2 = unsafe { lvglcj_call_closure(cid, 1) }
    mark("[F6] rc2=${rc2} swallowed=${closureSwallowedExceptionCount()}")

    unregisterClosure(cid)
    mark("[F7] unregistered, live=${closureCount()}")
}

/* ------------------------------- 场景 G：深层仓颉调用链后再 dispatch（检验是否栈深相关） */
/*
 * 这一场景回答一个决定严重程度的问题：
 *   缺陷是**只在测试框架的深栈下**出现，还是**普通应用的调用链也能触发**？
 *
 * 做法：在调用桥接层之前先递归若干层，人工制造与测试框架相近的仓颉栈深度。
 *   · 若崩溃 → 缺陷与栈深相关，普通应用（层级稍深时）同样有风险，必须严格禁止该用法
 *   · 若不崩 → 支持「测试框架栈形态特有」的判断，但仍按已知限制处理
 */
/* 递归深度：H6 实验用过 40 / 200 / 800，三档均未触发 */
let DEPTH: Int64 = 40

func deepCall(depth: Int64, cid: Int32): Int32 {
    if (depth <= 0) {
        return unsafe { lvglcj_call_closure(cid, 1) }
    }
    return deepCall(depth - 1, cid)
}

func scenarioDeepStack(): Unit {
    println("=== 场景 G：深调用链（40 层）后再 dispatch ===")

    let cid = registerClosure(throwingClosure("G"))
    mark("[G1] cid=${cid}")

    mark("[G2] dispatch（栈深 ${DEPTH}）第 1 次")
    let rc1 = deepCall(DEPTH, cid)
    mark("[G3] rc1=${rc1} swallowed=${closureSwallowedExceptionCount()}")

    mark("[G4] dispatch（栈深 ${DEPTH}）第 2 次")
    let rc2 = deepCall(DEPTH, cid)
    mark("[G5] rc2=${rc2} swallowed=${closureSwallowedExceptionCount()}")

    unregisterClosure(cid)
    mark("[G6] unregistered, live=${closureCount()}")
}

/* ------------------------------------------------------------------ main */
main(args: Array<String>) {
    let scenario = pickScenario(args)

    println("########## C4 复现：scenario=${scenario} ##########")
    println("[0] 以下带序号的标记用于定位崩溃点：最后出现的标记即为崩溃点之前的位置")

    mark("[0.1] lvglcj_init")
    let rinit = unsafe { lvglcj_init() }
    mark("[0.2] init rc=${rinit}")

    mark("[0.3] bindCurrentThreadAsLvgl")
    let rbind = bindCurrentThreadAsLvgl()
    mark("[0.4] bind rc=${rbind}")

    match (scenario) {
        case "same_cid" => scenarioSameCid()
        case "diff_cids" => scenarioDiffCids()
        case "normal_after" => scenarioNormalAfter()
        case "outer_try" => scenarioOuterTry()
        case "prebuilt_throw" => scenarioPrebuiltThrow()
        case "deep_stack" => scenarioDeepStack()
        case _ => println("未知场景：${scenario}（可选 same_cid / diff_cids / normal_after / outer_try）")
    }

    mark("[E1] lvglcj_deinit")
    let rdeinit = unsafe { lvglcj_deinit() }
    mark("[E2] deinit rc=${rdeinit}")

    println("########## C4 REPRO SURVIVED（本场景未崩溃）##########")
}
