/*
 * anim.cj —— 动画 L1 包装（设计文档 §3.10 / §6.4）
 *
 * ===================== 用户不需要管理 anim_ctx 的生命周期 =====================
 * 设计文档要求「不暴露 ctx 生命周期给用户」，这一点由 C 侧落实：
 * internal deleted_cb 是 anim_ctx 的**唯一释放点**，而它在三条路径上都会被触发
 * （自然结束 / 手动 close / 目标对象被删除）。因此本类**不提供** release/free，
 * 用户也**不能**手动释放 —— 能手动释放就会有「手动释放 + 回调释放」的双重释放。
 *
 * 用户侧只需知道一条规则：
 *   · 动画跑完（或被 close / 对象被删）后，这个 LvAnim 句柄就失效了，
 *     再调用它的方法会抛异常（或 close 被当作已关闭而幂等）。
 *
 * ===================== 回调里不要构造异常 =====================
 * onExec / onStart / onDeleted 的闭包都在「C → 仓颉」回调帧内执行。
 * 仓颉 1.1.0 在该帧内**构造**异常对象存在运行时缺陷（详见 docs/C4_DEFECT.md），
 * 因此回调内应通过修改捕获状态或返回码来表达失败，不要 `throw` 一个新异常。
 * （异常**传播**是安全的：分发器会把它们吞并在 C 边界之内。）
 */
package lvgl4cj

import std.deriving.Derive

/// 插值曲线（取值与 C 侧 lvglcj_anim_path_t 逐值一致）
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvAnimPath {
    | Linear // 0：匀速（默认）
    | Step // 1：阶跃（到结束时刻才跳到终值）
    | EaseIn // 2：慢起
    | EaseOut // 3：慢止
    | EaseInOut // 4：慢起慢止
    | Overshoot // 5：冲过头再回
    | Bounce // 6：回落弹跳
}

func lvAnimPathToInt(p: LvAnimPath): Int32 {
    match (p) {
        case Linear => 0
        case Step => 1
        case EaseIn => 2
        case EaseOut => 3
        case EaseInOut => 4
        case Overshoot => 5
        case Bounce => 6
    }
}

/*
 * 动画 DSL。
 *
 * 典型用法（每步都返回 this，便于链式书写）：
 *   LvAnim.create()
 *       .target(box).values(0, 100).duration(300)
 *       .path(LvAnimPath.EaseInOut)
 *       .onExec { v => ... }
 *       .start()
 */
public class LvAnim {
    /// C 侧动画上下文句柄
    public let handle: Int64

    /*
     * 已注册的闭包 id。
     * ★ 保留它们不是为了手动注销 —— 注销由 C 侧的注销协议完成。
     *   这里记录的目的是**防重复注册**：同一动画注册两个 exec 回调会静默覆盖，
     *   而那几乎总是调用方的笔误（例如把链式调用写了两遍）。
     */
    private var execCid: Int32 = 0
    private var deletedCid: Int32 = 0
    private var startCid: Int32 = 0
    private var started = false

    private init(h: Int64) {
        handle = h
    }

    /**
     * 创建一个动画（尚未启动；配置完后调 start()）。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— C 侧无法分配动画上下文时
     *         （通常是 LVGL 动画池已满）。
     */
    public static func create(): LvAnim {
        let h = unsafe { lvglcj_anim_create() }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "动画创建失败（动画池可能已满）")
        }
        return LvAnim(h)
    }

    /*
     * 目标对象。
     *
     * ★ 它的作用**只是**「该对象被删除时，这个动画要一起停掉」——
     *   它不决定动画改什么（改什么由 onExec 里的逻辑决定）。
     *   之所以要单独说明：LVGL 原本会自动停掉「同一 var 上的动画」，
     *   而我们的 var 是 ctx（ADR-015），所以那条自动路径失效，
     *   改由 C 侧 DELETE 钩子按本字段补偿。
     */
    public func target(obj: LvObject): LvAnim {
        check(unsafe { lvglcj_anim_set_target(handle, obj.handle) }, "anim.target")
        return this
    }

    /// 插值区间：动画期间 onExec 会收到从 fromValue 平滑过渡到 toValue 的值
    public func values(fromValue: Int32, toValue: Int32): LvAnim {
        check(unsafe { lvglcj_anim_set_values(handle, fromValue, toValue) }, "anim.values")
        return this
    }

    /// 单程时长（毫秒）
    public func duration(ms: Int32): LvAnim {
        check(unsafe { lvglcj_anim_set_time(handle, ms) }, "anim.duration")
        return this
    }

    /// 启动延迟（毫秒）
    public func delay(ms: Int32): LvAnim {
        check(unsafe { lvglcj_anim_set_delay(handle, ms) }, "anim.delay")
        return this
    }

    public func path(p: LvAnimPath): LvAnim {
        check(unsafe { lvglcj_anim_set_path(handle, lvAnimPathToInt(p)) }, "anim.path")
        return this
    }

    /*
     * 重复次数。★ 取值语义容易记反，这里明确写出：
     *   0 = **无限循环**，1 = 只跑一趟（不重复），N = 共跑 N 趟。
     * 「0 表示无限」是 LVGL 的原生语义，本层逐值透传不做换算 ——
     * 换算会让这条特殊语义变得模糊。
     */
    public func repeat(cnt: Int32): LvAnim {
        check(unsafe { lvglcj_anim_set_repeat(handle, cnt) }, "anim.repeat")
        return this
    }

    /// 到达终点后反向播回的时长（毫秒，0 = 不反向）
    public func playback(ms: Int32): LvAnim {
        check(unsafe { lvglcj_anim_set_playback(handle, ms) }, "anim.playback")
        return this
    }

    // ---------------------------------------------------------- 回调

    /**
     * 每动画帧调用一次，参数是本次插值。
     *
     * ★ 这是**必需**的：没有它 start() 会被拒绝（一个没有执行回调的动画没有意义，
     *   而且 lv_anim_delete 的删除键也依赖它）。
     * ★ 回调内不要构造异常（C4 平台限制）。
     *
     * @throws[LvglException] 类型为 InvalidArgument —— 已经设置过执行回调
     *         （重复设置会静默覆盖，通常是链式调用写了两遍）。
     */
    public func onExec(cb: (Int32) -> Unit): LvAnim {
        if (execCid != 0) {
            throw LvglException(InvalidArgument, "onExec 已经设置过了（重复设置会静默覆盖，疑似笔误）")
        }
        /* C 侧传回的是 int32 插值经 int64 扩宽后的值，这里收回 Int32 */
        let cid = registerClosure {arg => cb(Int32(arg))}
        let rc = unsafe { lvglcj_anim_set_exec_cb(handle, cid) }
        if (rc != 0) {
            /* 注册失败必须立刻收回 cid，否则闭包表泄漏 */
            unregisterClosure(cid)
            throwIfError(rc, "anim.onExec")
        }
        execCid = cid
        return this
    }

    /**
     * 动画真正开始时调用一次（考虑 delay 之后）。参数是目标对象句柄（0 = 未指定）。
     *
     * @throws[LvglException] 类型为 InvalidArgument —— 已经设置过该回调。
     */
    public func onStart(cb: (Int64) -> Unit): LvAnim {
        if (startCid != 0) {
            throw LvglException(InvalidArgument, "onStart 已经设置过了")
        }
        let cid = registerClosure {arg => cb(arg)}
        let rc = unsafe { lvglcj_anim_set_start_cb(handle, cid) }
        if (rc != 0) {
            unregisterClosure(cid)
            throwIfError(rc, "anim.onStart")
        }
        startCid = cid
        return this
    }

    /**
     * 动画结束时调用一次（自然结束 / 被删除 / 目标对象被删除，三条路径都会到）。
     * 这是「动画真的没了」的唯一可靠时机。
     *
     * @throws[LvglException] 类型为 InvalidArgument —— 已经设置过该回调。
     */
    public func onDeleted(cb: () -> Unit): LvAnim {
        if (deletedCid != 0) {
            throw LvglException(InvalidArgument, "onDeleted 已经设置过了")
        }
        let cid = registerClosure {_ => cb()}
        let rc = unsafe { lvglcj_anim_set_deleted_cb(handle, cid) }
        if (rc != 0) {
            unregisterClosure(cid)
            throwIfError(rc, "anim.onDeleted")
        }
        deletedCid = cid
        return this
    }

    // ---------------------------------------------------------- 启动与结束

    /**
     * 启动动画。
     *
     * ★ 启动之后**不能再改配置**：C 侧会拒绝（start 会把配置拷贝进 LVGL，
     *   之后改本地模板是「设了但没生效」的静默失效）。本类同样在此拦一道，
     *   让错误在仓颉侧就带着清晰信息抛出。
     *
     * @throws[LvglException] 类型为 InvalidArgument —— 重复启动，或调用方
     *         未先设置 onExec；OutOfMemory —— C 侧启动失败。
     */
    public func start(): Unit {
        if (started) {
            throw LvglException(InvalidArgument, "该动画已经启动过了")
        }
        check(unsafe { lvglcj_anim_start(handle) }, "anim.start")
        started = true
    }

    /*
     * 主动结束动画。幂等 —— 动画已经结束（或被目标对象带走）时调用它不会报错，
     * 因为「让它结束」这个意图已经达成。
     */
    public func close(): Unit {
        let rc = unsafe { lvglcj_anim_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return
        }
        check(rc, "anim.close")
    }

    /// 是否仍在运行（句柄有效即代表 C 侧的 ctx 还在）
    public prop isAlive: Bool {
        get() {
            unsafe { lvglcj_handle_alive(handle) } != 0
        }
    }

    private func check(rc: Int32, what: String): Unit {
        throwIfError(rc, "动画操作 ${what}")
    }
}

// ------------------------------------------------------------------ 观测与辅助

/// 当前正在运行的动画数（不含尚未到期的 delay 阶段）
public func animCountRunning(): Int32 {
    unsafe { lvglcj_anim_count_running() }
}

/*
 * 活跃 anim_ctx 数 —— §11.3 的泄漏对账指标。
 *
 * 与 animCountRunning() 的关系（对账断言的正确写法）：
 *   · **稳态（没有动画在跑）时两者都必须是 0** —— 这就是「零泄漏」；
 *   · 动画进行中本值可能 ≥ running（delay 未到期的动画不计入 running，但 ctx 已存在）。
 *   因此不要断言「两者恒等」，要断言「稳态双零」。
 */
public func animCtxCount(): Int32 {
    unsafe { lvglcj_anim_ctx_count() }
}

/*
 * 停止某对象上的全部动画。
 *
 * 框架在对象被删除时会**自动**做这件事（DELETE 钩子步骤 (c)）；
 * 这个入口是给「想提前停掉某对象的动画但保留对象」的场景用的。
 */
public func clearAnimations(obj: LvObject): Unit {
    throwIfError(unsafe { lvglcj_obj_delete_anim(obj.handle) }, "clearAnimations")
}
