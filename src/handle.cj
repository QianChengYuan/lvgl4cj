/*
 * handle.cj —— 四态句柄的仓颉侧表示（设计文档 §3.1）
 *
 * 分工：
 *   · C 侧 lvglcj_handle_* 是权威实现（自增 ID + 双哈希表 + 墓碑回收）
 *   · 本文件是它的**薄封装**：把 Int64 句柄变成有类型、有语义的值对象，
 *     让用户代码无法把句柄和普通整数混用
 *
 * 四态与幂等契约（§3.1.3，数值来自 conf_const.cj，与 C 侧冻结一致）：
 *   Uninit       未初始化     —— 无表项
 *   Alive        有效         —— 表项存在，指针有效
 *   Invalidated  原生对象已删 —— **表项仍存在**（保留删除栈便于诊断）
 *   Released     表项已回收   —— ptr_of 返回 NULL
 *
 *   close()   在 Alive → Invalidated；其他态无操作（幂等）
 *   release() 任何态 → Released；重复调用无操作（幂等）
 */
package lvgl4cj

import std.deriving.Derive
import lvgl4cj.generated.{HSTATE_UNINIT, HSTATE_ALIVE, HSTATE_INVALIDATED, HSTATE_RELEASED}

/* ------------------------------------------------------------------ 四态 */
/*
 * @Derive[Equatable]：四态是需要直接比较的值语义，
 * 派生 Equatable 后可在 @Assert / @Expect 中直接比较，
 * 也让用户代码可以写 `if (h.state() == HandleState.Alive)`。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum HandleState {
    | Uninit
    | Alive
    | Invalidated
    | Released
}

/*
 * 用 if / else-if 而非 match：原因见 error.cj 中 lvglErrorFromCode 的说明 ——
 * `case HSTATE_ALIVE =>` 会被当作**绑定模式**（匹配任意值），第一个 case 永远命中。
 */
public func handleStateFromCode(code: Int32): HandleState {
    if (code == HSTATE_UNINIT) {
        return Uninit
    }
    if (code == HSTATE_ALIVE) {
        return Alive
    }
    if (code == HSTATE_INVALIDATED) {
        return Invalidated
    }
    if (code == HSTATE_RELEASED) {
        return Released
    }
    return Uninit
}

public func handleStateName(st: HandleState): String {
    match (st) {
        case Uninit => "Uninit"
        case Alive => "Alive"
        case Invalidated => "Invalidated"
        case Released => "Released"
    }
}

/* ------------------------------------------------------------------ 句柄 */
/*
 * 为什么是 struct 而不是 class：
 *   句柄是「值」——复制一个句柄不应该产生两个不同的生命周期对象。
 *   用值类型还能避免额外的堆分配（句柄在热路径上被频繁传递）。
 *
 * 为什么实现 Resource：
 *   设计文档 ADR-006 要求「显式 close()，禁用终结器」。
 *   Resource 让 `try (h = ...) { }` 语法可用，把释放动作写进控制流，
 *   而不是交给不确定时机的 GC。
 */
public struct LvHandle <: Resource & ToString {
    /// 原生句柄 ID；0 表示空句柄
    public let raw: Int64

    public init(raw: Int64) {
        this.raw = raw
    }

    /// 空句柄（永不指向任何对象）
    public static func empty(): LvHandle {
        return LvHandle(0)
    }

    public func isNull(): Bool {
        return raw <= 0
    }

    /// 当前四态
    public func state(): HandleState {
        return handleStateFromCode(unsafe { lvglcj_handle_state(raw) })
    }

    public func isAlive(): Bool {
        return unsafe { lvglcj_handle_alive(raw) } != 0
    }

    /// 是否处于「延迟删除」待处理状态（回调中 close() 后、同帧删除前）
    public func isPendingDelete(): Bool {
        return unsafe { lvglcj_handle_pending_delete(raw) } != 0
    }

    /* -------------------------------------------------- Resource 语义 */
    /// 已关闭 = 不再是 Alive（Invalidated / Released / Uninit 都算）
    public func isClosed(): Bool {
        return !isAlive()
    }

    /*
     * close()：Alive → Invalidated。幂等。
     *
     * ★ 注意语义：close() 只把句柄置为失效，**不释放表项内存**——
     *   这是刻意的（§3.1）：保留表项与删除栈，让「谁删了我」可被诊断。
     *   表项内存由 release() 回收。
     *
     * 对**对象**而言，真正的删除动作（调 lv_obj_delete）由 LvObject.close()
     * 封装（含 DELETE 钩子四步顺序），本方法是更底层的原语。
     */
    public func close(): Unit {
        unsafe { lvglcj_handle_invalidate(raw) }
    }

    /// release()：任何态 → Released（回收表项内存）。幂等。
    public func release(): Unit {
        unsafe { lvglcj_handle_release(raw) }
    }

    /// close() + release()：一步走完，用于确定不再需要诊断信息的场景
    public func closeAndRelease(): Unit {
        close()
        release()
    }

    public override func toString(): String {
        return "LvHandle(${raw}, ${handleStateName(state())})"
    }
}

/* ------------------------------------------------------------ 统计 / 诊断 */
/// 指定状态的句柄数量；allStates 为 true 时统计全部
public func handleCount(state: HandleState, allStates!: Bool = false): Int32 {
    /* 状态码就地映射，不引入中间变量（G.VAR.02：作用域最小化） */
    return unsafe {
        lvglcj_handle_count(if (allStates) {
            -1
        } else {
            match (state) {
                case Uninit => HSTATE_UNINIT
                case Alive => HSTATE_ALIVE
                case Invalidated => HSTATE_INVALIDATED
                case Released => HSTATE_RELEASED
            }
        })
    }
}

/*
 * 泄漏自检（§7.3 / §11.1 门禁）：
 *   handleCount(Alive) 与 LVGL 内部对象数应长期一致或差值恒定；
 *   本函数给出仓颉侧可见的 ALIVE 句柄数，供长跑对账。
 */
public func aliveHandleCount(): Int32 {
    return unsafe { lvglcj_handle_count(HSTATE_ALIVE) }
}
