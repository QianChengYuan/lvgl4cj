/*
 * closure.cj —— 闭包表与跨语言分发器（设计文档 §3.2.3，分支 B 的仓颉侧）
 *
 * ===================== 为什么必须这样做 =====================
 * 仓颉的 CFunc Lambda **不能捕获变量**（V2 探针的实测结论，见 docs/P0_RESULTS.md）。
 * 所以「把用户闭包直接当 C 函数指针交给 LVGL」不可能实现。
 *
 * 分支 B 的做法：
 *   1. 用户闭包（**可以**自由捕获）存进本文件的全局表，得到 closure_id
 *   2. 交给 C 侧的只是 closure_id（装箱进 user_data，显式经 intptr_t）
 *   3. C 侧 trampoline 调 g_dispatch(cid, arg)
 *   4. 本文件的分发器查表 → **释放锁后**调用闭包 → 异常在此被吞并
 *
 * C 侧只持有函数指针，**桥接库不引用任何仓颉符号** ——
 * 这同时解决了静态库的未定义符号与链接顺序问题。
 * ==========================================================
 *
 * 线程安全要点（cangjie-concurrency 技能）：
 *   · HashMap 非线程安全 → 全程用 Mutex 保护
 *   · **取出闭包后必须先释放锁再调用**，否则用户闭包里若再注册/注销闭包会自锁死
 *   · 分发器由 C 侧（可能是 LVGL OS 线程）调用，因此这些全局状态必须是并发安全的
 */
package lvgl4cj

import std.collection.{HashMap, HashSet}
import std.sync.Mutex

/* ---------------------------------------------------------------- 全局表 */
/*
 * cid 从 1 开始分配；0 保留给「无闭包」（与 C 侧 LVGLCJ_CID_NONE 一致）。
 * 因此 user_data 中永远不存在「NULL 与合法 cid」的二义值。
 */
let G_CLOSURE_MUTEX = Mutex()
let G_CLOSURE_MAP = HashMap<Int32, (Int64) -> Unit>()
var gNextCid: Int32 = 1

/*
 * 一次性闭包（任务队列用）的 cid 集合。
 *
 * 为什么需要它：任务队列把「一次投递」表示为一个闭包（task_id 即 cid，见
 * lvglcj_bridge.h §5.1），任务执行完必须把闭包表项回收，否则每投递一个任务
 * 就泄漏一条记录。而「执行完」这个时点只有 dispatch 知道。
 *
 * 两种实现方式的取舍：
 *   方案甲（不采用）：闭包内部注销自己 —— 需要闭包捕获「自己的 cid」，
 *     即先注册再把自己赋进被捕获的可变变量。这种自引用捕获依赖闭包对局部
 *     变量的捕获时机语义，脆弱且难读，出问题时表现为随机的 cid=0。
 *   方案乙（本实现）：注册时把 cid 标进本集合，dispatch 在**取出闭包的同时**
 *     就从表和集合里摘掉它。语义显式，且天然覆盖异常路径 ——
 *     「执行一次」与「回收」是同一个动作的两面，不依赖闭包自身做对什么。
 */
let G_ONE_SHOT_CIDS = HashSet<Int32>()

/* 可观测性：闭包表大小应随对象删除而归零（§11.3） */
var gLiveClosures: Int32 = 0
/* 分发时未找到闭包的次数：正常的注销竞态会产生少量，持续增长则是缺陷 */
var gMissingDispatch: Int32 = 0
/* 被吞并的异常次数（与 C 侧 CALLBACK_THREW 计数相互印证） */
var gSwallowedExceptions: Int32 = 0
var gDispatcherInstalled = false

/* ---------------------------------------------------------------- 分发器 */
/*
 * 顶层不捕获的 CFunc：只读写全局量，因此可以用作 C 侧函数指针。
 * 它**只做转发**，所有逻辑都在 dispatchClosure 里，便于单测。
 */
let G_DISPATCHER: CFunc<(Int32, Int64) -> Int32> = { // cjlint-ignore !G.VAR.02 它必须是顶层稳定对象：其地址要作为 C 函数指针交给 lvglcj_set_dispatch，放进类里只是换个写法而语义不变
    cid, arg => dispatchClosure(cid, arg)
}

/*
 * 幂等安装。首次注册闭包时自动调用 ——
 * 这样「忘记安装分发器」不会变成一个静默的回调丢失，而是不可能发生。
 */
func installClosureDispatcher(): Int32 {
    synchronized(G_CLOSURE_MUTEX) {
        if (gDispatcherInstalled) {
            return 0
        }
        let rc = unsafe { lvglcj_set_dispatch(G_DISPATCHER) }
        if (rc == 0) {
            gDispatcherInstalled = true
        }
        return rc
    }
}

/* ---------------------------------------------------------------- 注册/注销 */

/**
 * 注册一个捕获闭包，返回 closure_id。
 *
 * @throws[LvglException] 类型为 CallbackThrew —— 分发器安装失败（无法跨 C 边界分发）；
 *         ClosureExhausted —— closure_id 已耗尽（§3.2.3 配套约束，
 *         正常进程生命周期内不会发生；真发生说明有闭包没被注销）。
 */
public func registerClosure(cb: (Int64) -> Unit): Int32 {
    let irc = installClosureDispatcher()
    if (irc != 0) {
        throw LvglException(CallbackThrew, "安装分发器失败（rc=${irc}），无法注册闭包")
    }

    synchronized(G_CLOSURE_MUTEX) {
        if (gNextCid >= Int32.Max) {
            throw LvglException(ClosureExhausted, "closure_id 已耗尽")
        }
        let cid = gNextCid
        gNextCid = gNextCid + 1
        G_CLOSURE_MAP.add(cid, cb)
        gLiveClosures = gLiveClosures + 1
        return cid
    }
}

/**
 * 注册一个**一次性**闭包（任务队列用）：被分发一次后自动从闭包表摘除。
 *
 * 与 registerClosure 的唯一区别是「分发即回收」。除此之外完全相同 ——
 * 包括异常吞并行为，因此任务体抛异常不会污染后续任务。
 *
 * @throws[LvglException] 类型为 CallbackThrew —— 分发器安装失败；
 *         ClosureExhausted —— closure_id 已耗尽。
 */
public func registerOneShotClosure(cb: (Int64) -> Unit): Int32 {
    let irc = installClosureDispatcher()
    if (irc != 0) {
        throw LvglException(CallbackThrew, "安装分发器失败（rc=${irc}），无法注册任务")
    }

    synchronized(G_CLOSURE_MUTEX) {
        if (gNextCid >= Int32.Max) {
            throw LvglException(ClosureExhausted, "closure_id 已耗尽")
        }
        let cid = gNextCid
        gNextCid = gNextCid + 1
        G_CLOSURE_MAP.add(cid, cb)
        G_ONE_SHOT_CIDS.add(cid)
        gLiveClosures = gLiveClosures + 1
        return cid
    }
}

/// 该 cid 是否为一次性闭包（诊断/测试用）
public func isOneShotClosure(cid: Int32): Bool {
    synchronized(G_CLOSURE_MUTEX) {
        return G_ONE_SHOT_CIDS.contains(cid)
    }
}

/// 注销闭包。幂等：不存在的 cid 无操作。
public func unregisterClosure(cid: Int32): Unit {
    if (cid <= 0) {
        return
    }
    synchronized(G_CLOSURE_MUTEX) {
        /*
         * 一并清掉「一次性」标记：否则 cid 会永久留在 G_ONE_SHOT_CIDS 里，
         * 表现为集合随运行时间单调增长（而 gLiveClosures 是正常的）。
         */
        G_ONE_SHOT_CIDS.remove(cid)
        match (G_CLOSURE_MAP.remove(cid)) {
            case Some(_) => gLiveClosures = gLiveClosures - 1
            case None => ()
        }
    }
}

/*
 * 批量注销：C 侧的 DELETE 钩子逐个 cid 通知（§3.1.4 步骤 a），
 * 因此仓颉侧不需要「按对象批量注销」的接口 —— 对象→cid 的映射在 C 侧注册表里。
 * 本函数供 L1 层在异常路径下兜底使用。
 */
public func unregisterClosures(cids: Array<Int32>): Unit {
    for (cid in cids) {
        unregisterClosure(cid)
    }
}

/* ---------------------------------------------------------------- 分发 */
/*
 * 由 C 侧调用（可能是 LVGL OS 线程）。
 * 返回值：0 成功；非 0 表示闭包抛了异常（已被吞并，C 侧会记 CALLBACK_THREW）。
 *
 * ★ 绝不能把异常抛回 C 侧 —— 那会跨越 C 边界，是未定义行为（§3.8.1）。
 */
func dispatchClosure(cid: Int32, arg: Int64): Int32 {
    if (cid <= 0) {
        return 0
    }

    /*
     * 保留参数：C 侧通知「请注销这个 cid，不要调用」（见 callback.h）。
     * 用于对象删除/动画结束/定时器删除时回收闭包，防止闭包表泄漏。
     */
    if (arg == Int64.Min) {
        unregisterClosure(cid)
        return 0
    }

    /* 在锁内取出闭包引用，锁外调用 —— 防止用户闭包内再操作闭包表造成自锁死 */
    var picked: Option<(Int64) -> Unit> = None
    synchronized(G_CLOSURE_MUTEX) {
        picked = G_CLOSURE_MAP.get(cid)
        if (picked.isSome() && G_ONE_SHOT_CIDS.contains(cid)) {
            /*
             * ★ 一次性闭包（任务）：**先摘除再调用**，顺序是刻意的。
             *   无论闭包体正常返回还是抛异常，任务都只执行一次、表项必被回收，
             *   不依赖闭包自身做对任何事。
             *   若改成「调用后再摘除」，闭包抛异常就会跳过回收 → 表项泄漏。
             */
            G_CLOSURE_MAP.remove(cid)
            G_ONE_SHOT_CIDS.remove(cid)
            gLiveClosures = gLiveClosures - 1
        }
        if (picked.isNone()) {
            /* 已注销但仍被分发：多为注销与分发并发的正常竞态，计数观察即可 */
            gMissingDispatch = gMissingDispatch + 1
        }
    }

    match (picked) {
        case Some(cb) => try {
            cb(arg)
            return 0
        } catch (e: Exception) {
            /* ★ 异常在此终止，不跨越 C 边界 */
            synchronized(G_CLOSURE_MUTEX) {
                gSwallowedExceptions = gSwallowedExceptions + 1
            }
            eprintln("[lvgl4cj] 回调闭包抛出异常（已吞并，cid=${cid}）：${e}")
            return lvglErrorToCode(CallbackThrew)
        }
        case None =>
            /* 闭包不存在不视为错误：注销与分发的竞态是预期内的 */
            return 0
    }
}

/* ---------------------------------------------------------- 可观测性 */
/// 当前活跃闭包数；对象全部删除后应归零（§11.3 闭包表大小）
public func closureCount(): Int32 {
    synchronized(G_CLOSURE_MUTEX) {
        return gLiveClosures
    }
}

/// 分发了但未找到闭包的次数（正常应稳定在很小的值）
public func closureMissingDispatchCount(): Int32 {
    synchronized(G_CLOSURE_MUTEX) {
        return gMissingDispatch
    }
}

/// 被吞并的回调异常次数
public func closureSwallowedExceptionCount(): Int32 {
    synchronized(G_CLOSURE_MUTEX) {
        return gSwallowedExceptions
    }
}

/// 分发器是否已安装到 C 侧
public func isClosureDispatcherInstalled(): Bool {
    synchronized(G_CLOSURE_MUTEX) {
        return gDispatcherInstalled
    }
}

/// 重置诊断计数（仅测试使用）
public func resetClosureDiagnostics(): Unit {
    synchronized(G_CLOSURE_MUTEX) {
        gMissingDispatch = 0
        gSwallowedExceptions = 0
    }
}
