/*
 * error.cj —— 错误模型（设计文档 §3.12 + 附录 B）
 *
 * 设计要点：
 *   · LvglError 的数值**冻结**（附录 B），与 C 侧 lvglcj_status_t 逐项一致；
 *     一致性由 test/ffi_contract_test.cj 断言，避免两侧漂移
 *   · LvglException 继承 Exception（**不可继承 Error**，仓颉规则）
 *   · 回调边界抛出的异常由 dispatcher 捕获并转为非 0 返回码（§3.8.1），
 *     绝不跨越 C 边界
 *
 * 数值来源：src/generated/conf_const.cj（由 probe_conf 实测导出），
 * 因此不需要在仓颉侧硬编码，配置/错误码变更会自动同步。
 */
package lvgl4cj

import std.deriving.Derive
import lvgl4cj.generated.{CODE_OK, CODE_OK_DEFERRED, CODE_ERR_INVALID_HANDLE, CODE_ERR_NOT_INITIALIZED, 
    CODE_ERR_INVALID_CONFIG, CODE_ERR_CALLBACK_THREW, CODE_ERR_OUT_OF_MEMORY, CODE_ERR_WRONG_THREAD, 
    CODE_ERR_DEADLOCK_RISK, CODE_ERR_QUEUE_FULL, CODE_ERR_BACKEND_FAILURE, CODE_ERR_INVALID_ARGUMENT, 
    CODE_ERR_CLOSURE_EXHAUSTED, CODE_ERR_PENDING_DELETE, CODE_ERR_NOT_SUPPORTED, CODE_ERR_VERSION_MISMATCH, 
    CODE_ERR_DEFERRED_LOOP, CODE_ERR_PIN_EXHAUSTED, CODE_ERR_ANIM_CTX_LOST}

/* ------------------------------------------------------------------ 错误码 */
/*
 * 与 lvglcj_error.h 的 lvglcj_status_t 一一对应。
 * PinExhausted 因分支 A（闭包指针 + pin 表）已归档而**不会产生**，
 * 但保留枚举项以维持与 C 侧数值表的一一对应。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvglError {
    | Ok
    | OkDeferred
    | InvalidHandle
    | NotInitialized
    | InvalidConfig
    | CallbackThrew
    | OutOfMemory
    | WrongThread
    | DeadlockRisk
    | QueueFull
    | BackendFailure
    | InvalidArgument
    | ClosureExhausted
    | PendingDelete
    | NotSupported
    | VersionMismatch
    | DeferredLoop
    | PinExhausted
    | AnimCtxLost
    /// 未知/未来新增的错误码，保留原始数值以免信息丢失
    | UnknownCode(Int32)
}

/*
 * ★★ 这里必须用 if / else-if，**不能用 match** ★★
 *
 * 仓颉的 match 模式中，`case CODE_OK =>` 会被解释为**绑定模式**
 *   —— 即「声明一个新变量 CODE_OK，匹配任意值」，
 * 于是第一个 case 永远命中，且编译器不报错（新变量遮蔽了同名常量）。
 * 这个陷阱会让「错误码映射」静默地全部返回第一个分支，
 * 属于最难发现的一类 bug（本项目实测踩到：lvglErrorFromCode(-1) 返回了 Ok）。
 *
 * 只有**字面量**才是可靠的常量模式；需要引用具名常量时必须用 if/else-if。
 */
/*
 * 错误码 → 枚举的映射表。
 *
 * 为什么用表而不是一条长 if 链：
 *   · 短 —— 19 个分支的 if 链本身就是「函数太长」告警的来源；
 *   · 好维护 —— 新增错误码时只改这一处，且表就写在枚举附近，
 *     漏加一行一眼能看出来；if 链则容易插错位置或漏掉分支，而漏掉的表现是
 *     「未知错误码」这种最不该出现的兜底结果。
 */
private let CODE_TO_ERROR: Array<(Int32, LvglError)> = [ // cjlint-ignore !G.VAR.02 顶层常量只初始化一次；放进 lvglErrorFromCode 会让每次 FFI 返回都重建这张表
    (CODE_OK, Ok),
    (CODE_OK_DEFERRED, OkDeferred),
    (CODE_ERR_INVALID_HANDLE, InvalidHandle),
    (CODE_ERR_NOT_INITIALIZED, NotInitialized),
    (CODE_ERR_INVALID_CONFIG, InvalidConfig),
    (CODE_ERR_CALLBACK_THREW, CallbackThrew),
    (CODE_ERR_OUT_OF_MEMORY, OutOfMemory),
    (CODE_ERR_WRONG_THREAD, WrongThread),
    (CODE_ERR_DEADLOCK_RISK, DeadlockRisk),
    (CODE_ERR_QUEUE_FULL, QueueFull),
    (CODE_ERR_BACKEND_FAILURE, BackendFailure),
    (CODE_ERR_INVALID_ARGUMENT, InvalidArgument),
    (CODE_ERR_CLOSURE_EXHAUSTED, ClosureExhausted),
    (CODE_ERR_PENDING_DELETE, PendingDelete),
    (CODE_ERR_NOT_SUPPORTED, NotSupported),
    (CODE_ERR_VERSION_MISMATCH, VersionMismatch),
    (CODE_ERR_DEFERRED_LOOP, DeferredLoop),
    (CODE_ERR_PIN_EXHAUSTED, PinExhausted),
    (CODE_ERR_ANIM_CTX_LOST, AnimCtxLost)
]

public func lvglErrorFromCode(code: Int32): LvglError {
    for ((c, e) in CODE_TO_ERROR) {
        if (c == code) {
            return e
        }
    }
    return UnknownCode(code)
}

public func lvglErrorToCode(err: LvglError): Int32 {
    match (err) {
        case Ok => CODE_OK
        case OkDeferred => CODE_OK_DEFERRED
        case InvalidHandle => CODE_ERR_INVALID_HANDLE
        case NotInitialized => CODE_ERR_NOT_INITIALIZED
        case InvalidConfig => CODE_ERR_INVALID_CONFIG
        case CallbackThrew => CODE_ERR_CALLBACK_THREW
        case OutOfMemory => CODE_ERR_OUT_OF_MEMORY
        case WrongThread => CODE_ERR_WRONG_THREAD
        case DeadlockRisk => CODE_ERR_DEADLOCK_RISK
        case QueueFull => CODE_ERR_QUEUE_FULL
        case BackendFailure => CODE_ERR_BACKEND_FAILURE
        case InvalidArgument => CODE_ERR_INVALID_ARGUMENT
        case ClosureExhausted => CODE_ERR_CLOSURE_EXHAUSTED
        case PendingDelete => CODE_ERR_PENDING_DELETE
        case NotSupported => CODE_ERR_NOT_SUPPORTED
        case VersionMismatch => CODE_ERR_VERSION_MISMATCH
        case DeferredLoop => CODE_ERR_DEFERRED_LOOP
        case PinExhausted => CODE_ERR_PIN_EXHAUSTED
        case AnimCtxLost => CODE_ERR_ANIM_CTX_LOST
        case UnknownCode(v) => v
    }
}

public func lvglErrorName(err: LvglError): String {
    match (err) {
        case Ok => "Ok"
        case OkDeferred => "OkDeferred"
        case InvalidHandle => "InvalidHandle"
        case NotInitialized => "NotInitialized"
        case InvalidConfig => "InvalidConfig"
        case CallbackThrew => "CallbackThrew"
        case OutOfMemory => "OutOfMemory"
        case WrongThread => "WrongThread"
        case DeadlockRisk => "DeadlockRisk"
        case QueueFull => "QueueFull"
        case BackendFailure => "BackendFailure"
        case InvalidArgument => "InvalidArgument"
        case ClosureExhausted => "ClosureExhausted"
        case PendingDelete => "PendingDelete"
        case NotSupported => "NotSupported"
        case VersionMismatch => "VersionMismatch"
        case DeferredLoop => "DeferredLoop"
        case PinExhausted => "PinExhausted"
        case AnimCtxLost => "AnimCtxLost"
        case UnknownCode(v) => "UnknownCode(${v})"
    }
}

/// 该错误码是否属于「成功类」（0 与正数）
public func lvglErrorIsSuccessCode(code: Int32): Bool {
    return code >= 0
}

/* ------------------------------------------------------------------ 异常 */
public class LvglException <: Exception {
    public let code: LvglError

    public init(code: LvglError, message: String) {
        super(message)
        this.code = code
    }

    public override func getClassName(): String {
        return "LvglException"
    }

    public override func toString(): String {
        return "LvglException(${lvglErrorName(code)}): ${message}"
    }
}

/**
 * 把 C 侧返回码转成异常。
 * OK 与 OK_DEFERRED 都算成功（§3.8.2：延迟删除返回 +1，不是错误）。
 * 返回 OK_DEFERRED 时调用方应得知「已入队、同帧内生效」。
 *
 * @throws[LvglException] 异常类型与错误码一一对应（见 lvglErrorFromCode）；
 *         本函数只做转换，不吞并任何失败。
 */
public func throwIfError(code: Int32, context: String): Unit {
    match (lvglErrorFromCode(code)) {
        case Ok | OkDeferred => return
        case err => throw LvglException(err, "${context} 失败：${lvglErrorName(err)}（code=${code}）")
    }
}

/* ------------------------------------------------- 全局错误回调（§3.12.3） */
/*
 * 全局错误回调的具体形态在 LvglRuntime.onError 处提供
 * （需要 @C struct 映射 lvglcj_error_t，其布局偏移由 conf_const.cj 描述）。
 * 本文件只负责错误码模型与异常类型，保持职责单一。
 */
