/*
 * display.cj —— 显示子系统的 L1 包装（设计文档 §5.3 / §6.4）
 *
 * ===================== 本文件刻意**不提供**的能力 =====================
 * 不提供「传入 Array<UInt8> 作为绘制缓冲」的接口，也不提供读取缓冲内容的接口。
 *
 * 理由（ADR-002）：绘制缓冲必须由 C 侧分配并持有。
 *   仓颉的 GC 可能移动或回收数组，而 LVGL 会在后续**多个帧**里持续写这块内存。
 *   一旦把仓颉数组的底层指针交出去，症状是间歇性花屏甚至段错误 ——
 *   它在小测试里往往复现不出来，是典型的上线后才暴露的一类缺陷。
 *
 *   最可靠的对策不是「文档里警告别这么用」，而是**根本不提供这个入口**：
 *   没有入口，就不存在误用。
 *
 * 需要自绘时走 Canvas 控件（§3.11.2），它同样在 C 侧管理自己的缓冲。
 * ★ 但 Canvas **尚未落地**（设计文档排在 P2，ABI 里的 8 个 canvas 符号是唯一
 *   尚未实现的一组）。在那之前自绘没有任何入口 —— 这一点请明确告诉使用者，
 *   不要让人去找一个不存在的 API。
 */
package lvgl4cj

import std.deriving.Derive

/*
 * 颜色格式（数值与 LVGL 的 lv_color_format_t 逐值一致，勿自行改号）。
 *
 * 为什么用顶层常量而不是「命名空间对象」：仓颉没有 object 声明
 * （那是 Kotlin/Scala 的写法），能承载常量的顶层结构只有 const/enum/class。
 * enum 表达不了这种非连续的魔数，class 装常量又要多一层限定名，
 * 因此直接沿用 LVGL 的宏名做顶层常量 —— 与生成的 conf_const.cj 也是同一风格。
 */
public const LV_COLOR_FORMAT_L8: Int32 = 0x06
public const LV_COLOR_FORMAT_RGB888: Int32 = 0x0F
public const LV_COLOR_FORMAT_ARGB8888: Int32 = 0x10
public const LV_COLOR_FORMAT_XRGB8888: Int32 = 0x11
public const LV_COLOR_FORMAT_RGB565: Int32 = 0x12
public const LV_COLOR_FORMAT_RGB565A8: Int32 = 0x14

/// 绘制缓冲模式（数值与 lvglcj_buf_mode_t 及 LVGL render mode 逐值一致）
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvBufMode {
    | Partial // 0：分块渲染，缓冲可远小于屏幕。MVP 默认，内存占用最小
    | Direct // 1：整屏缓冲，只重绘脏区
    | Full // 2：整屏缓冲，每次全屏重绘
}

func bufModeToCode(m: LvBufMode): Int32 {
    match (m) {
        case Partial => 0
        case Direct => 1
        case Full => 2
    }
}

/// 显示设备。持有 display 句柄；缓冲由 C 侧持有（见文件头说明）。
public class LvDisplay {
    /// C 侧 display 句柄
    public let handle: Int64

    /*
     * flush / flush_wait 回调的 cid。
     * 保存它**不是为了调用**，而是为了在 close 时明确回收 ——
     * 虽然 C 侧 delete 时也会注销，但显式记录让「注册了什么」在仓颉侧可见，
     * 便于泄漏排查时判断一个 cid 是否属于 display。
     */
    private var flushCid: Int32 = 0
    private var flushWaitCid: Int32 = 0

    private init(h: Int64) {
        handle = h
    }

    /*
     * 包装一个**已存在**的 display 句柄。
     *
     * 用于「display 由别处创建」的场景，最典型的是 headless 后端：
     * display 由 lvglcj_null_init 在 C 侧创建，这里只是取回它的包装。
     * 不提供这个入口的话，调用方就只能拿到裸句柄，失去 close() 的幂等保护。
     */
    static func wrap(h: Int64): LvDisplay {
        LvDisplay(h)
    }

    /*
     * 创建 display。必须在该 runtime 的 LVGL 线程上调用。
     *   bufLines = 0 时由桥接层按 PARTIAL 的 1/10 屏下限自动选取；
     *   DIRECT / FULL 模式下 bufLines 被忽略（缓冲固定整屏）。
     */
    /**
     * 创建一个 display（刷新缓冲由 C 侧分配，见 ADR-002）。
     *
     * 注：colorFormat / bufMode / bufLines 用**命名参数**（`!` 标记）声明默认值。
     * 仓颉不支持 `param: T = v` 这种写法，必须写成 `param!: T = v`；
     * 调用时可以按名传（create(320, 240, bufMode: LvBufMode.Direct)）。
     *
     * @throws[LvglException] 类型为 BackendFailure —— C 侧创建失败
     *         （尺寸/格式非法，或 lv_conf 未启用软件渲染）。
     */
    public static func create(width: Int32, height: Int32, colorFormat!: Int32 = LV_COLOR_FORMAT_RGB565,
        bufMode!: LvBufMode = LvBufMode.Partial, bufLines!: Int32 = 0): LvDisplay {
        let h = unsafe {
            lvglcj_display_create(width, height, colorFormat, bufModeToCode(bufMode), bufLines)
        }
        if (h <= 0) {
            throw LvglException(BackendFailure,
                "display 创建失败（宽=${width} 高=${height} 格式=${colorFormat}）；请检查 lv_conf 是否启用了软件渲染")
        }
        return LvDisplay(h)
    }

    /// 注册刷新回调。回调收到 display 句柄，必须在合适时机调用 flushReady()。
    public func setFlushCb(cb: (Int64) -> Unit): Unit {
        let cid = registerClosure(cb)
        let rc = unsafe { lvglcj_display_set_flush_cb(handle, cid) }
        if (rc != 0) {
            /* 注册失败必须把新 cid 收回，否则闭包表泄漏 */
            unregisterClosure(cid)
            throwIfError(rc, "lvglcj_display_set_flush_cb")
        }
        /* 换回调时 C 侧已注销旧 cid，这里只需更新记录 */
        flushCid = cid
    }

    /// 注册 flush 等待回调：在「需要等待上一次刷新完成」时被调用
    public func setFlushWaitCb(cb: (Int64) -> Unit): Unit {
        let cid = registerClosure(cb)
        let rc = unsafe { lvglcj_display_set_flush_wait_cb(handle, cid) }
        if (rc != 0) {
            unregisterClosure(cid)
            throwIfError(rc, "lvglcj_display_set_flush_wait_cb")
        }
        flushWaitCid = cid
    }

    /*
     * 通知 LVGL「本帧刷新已完成」。
     * 异步后端（如 SDL2 投递渲染请求后）必须在上传真正完成后调用它，
     * 否则 LVGL 会停在等待刷新的状态，表现为画面只出第一帧。
     */
    public func flushReady(): Unit {
        unsafe { lvglcj_display_flush_ready(handle) }
    }

    public func flushIsLast(): Bool {
        unsafe { lvglcj_display_flush_is_last(handle) } != 0
    }

    /// 绘制缓冲字节数（由 C 侧持有；此处仅供断言与排查，-1 表示不存在）
    public func bufferBytes(): Int64 {
        unsafe { lvglcj_display_get_buf_bytes(handle) }
    }

    /// 绘制缓冲每行字节数（来自 LVGL 的 stride 算子）
    public func bufferStride(): Int32 {
        unsafe { lvglcj_display_get_buf_stride(handle) }
    }

    public func colorFormat(): Int32 {
        unsafe { lvglcj_display_get_color_format(handle) }
    }

    /*
     * 关闭 display。
     *
     * ★ 幂等性由本层承担（§3.1.3 的分层约定）：
     *    C 侧对非 ALIVE 句柄返回 InvalidHandle，这里把它视为「已经关过了」。
     *    这样 close() 可以安全地重复调用，也可以在父对象/父 display 已被
     *    别处清理后继续调用。
     */
    public func close(): Unit {
        ensureLvglOwnership()
        let rc = unsafe { lvglcj_display_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return
        }
        /*
         * 其余错误如实抛出：delete 失败（例如非 LVGL 线程）绝不能静默 ——
         * 那意味着 display 没被真正删除，缓冲与句柄都还在。
         */
        throwIfError(rc, "lvglcj_display_delete")
        flushCid = 0
        flushWaitCid = 0
    }
}
