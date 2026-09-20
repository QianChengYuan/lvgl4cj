/*
 * event.cj —— 事件模型（设计文档 §3.7 / §5.6 / §6.5）
 *
 * ===================== 两处必须知道的 v9 事实 =====================
 * 1) **同一事件码可以有多个回调，且按注册顺序全部触发**（§3.7.2 要求）。
 *    这不是我们实现的，而是 LVGL 原生行为 —— 但要在此写明，
 *    因为「后注册的覆盖先注册的」是很多事件系统的默认语义，容易被误以为也是。
 *
 * 2) **绘制类事件（LV_EVENT_DRAW_*）不支持**：C 侧注册时直接返回 NotSupported（§3.7.3）。
 *    需要自绘请用 Canvas 控件（§3.11.2），而不是挂绘制回调 ——
 *    本层的 trampoline 只保证「回调不跨越 C 边界」，并不提供绘制上下文。
 *    ★ 但 Canvas **尚未落地**（P2）；在那之前自绘没有入口。
 *
 * ===================== 事件对象的生命周期 =====================
 * 事件对象是**短命**的：C 侧每次分发都新登记一个句柄，回调返回后立刻回收。
 * 因此 LvEvent **只能在回调执行期间使用** —— 把它存起来在下一帧再查
 * 会得到「句柄已失效」。这是刻意的（否则事件句柄表会随事件数量无限增长）。
 */
package lvgl4cj

import std.deriving.Derive

/*
 * 事件码。数值与 LVGL v9 的 lv_event_code_t **逐值一致**（从 LV_EVENT_ALL = 0 起递增）。
 *
 * 只收录已验证过取值的那一段（ALL … HOVER_LEAVE = 0..22）。
 * 后面还有 COVER_CHECK / DRAW_* 等内部事件，P0 不需要，也就**不猜**它们的值 ——
 * 猜错的后果是「注册了 A 却触发了 B」，而这是静默的。
 * 需要新增时请先去 third_party/lvgl/src/misc/lv_event.h 核对，再补映射。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvEventCode {
    | All // 0：注册全部事件
    | Pressed // 1
    | Pressing // 2
    | PressLost // 3
    | ShortClicked // 4
    | LongPressed // 5
    | LongPressedRepeat // 6
    | Clicked // 7：松开且未滚动。**按钮点击用这个**
    | Released // 8
    | ScrollBegin // 9
    | ScrollThrowBegin // 10
    | ScrollEnd // 11
    | Scroll // 12
    | Gesture // 13
    | Key // 14
    | Rotary // 15
    | Focused // 16
    | Defocused // 17
    | Leave // 18
    | HoverOver // 19
    | HoverLeave // 20
}

/// 事件码 → C 侧整数值（映射必须与上面的注释逐一对应）
func lvEventCodeToInt(c: LvEventCode): Int32 {
    match (c) {
        case All => 0
        case Pressed => 1
        case Pressing => 2
        case PressLost => 3
        case ShortClicked => 4
        case LongPressed => 5
        case LongPressedRepeat => 6
        case Clicked => 7
        case Released => 8
        case ScrollBegin => 9
        case ScrollThrowBegin => 10
        case ScrollEnd => 11
        case Scroll => 12
        case Gesture => 13
        case Key => 14
        case Rotary => 15
        case Focused => 16
        case Defocused => 17
        case Leave => 18
        case HoverOver => 19
        case HoverLeave => 20
    }
}

/*
 * 事件视图。**仅在回调执行期间有效**（见文件头说明）。
 *
 * 关于 target 与 currentTarget（§3.7.1）：
 *   target         = 冒泡链的起点，即「真正被点到的那个对象」
 *   currentTarget  = 当前正在执行回调的对象（冒泡过程中会逐级变化）
 * 二者在注册于被点对象自身时相同；注册在父容器上时不同。
 */
public class LvEvent {
    private let evh: Int64

    internal init(h: Int64) {
        evh = h
    }

    /// 本事件的事件码（原始整数，便于与自定义分发比对）
    public prop code: Int32 {
        get() {
            unsafe { lvglcj_event_get_code(evh) }
        }
    }

    /// 触发本次事件的闭包 id（诊断用）
    public prop closureId: Int32 {
        get() {
            unsafe { lvglcj_event_get_closure_id(evh) }
        }
    }

    /// 冒泡链起点（真正被命中的对象句柄）
    public func target(): Int64 {
        unsafe { lvglcj_event_get_target(evh) }
    }

    /// 当前回调所属对象句柄
    public func currentTarget(): Int64 {
        unsafe { lvglcj_event_get_current_target(evh) }
    }

    /*
     * 载荷读取：两条**对称**路径，必须与发送侧匹配（见 lvglcj_bridge.h §5.6）。
     *   发送侧 objSendEvent(obj, code, param)        → 接收侧 paramHandle()
     *   发送侧 objSendEventScalar(obj, code, scalar) → 接收侧 paramScalar()
     * 之所以不能合成一个入口：C 层无法可靠区分「恰好等于某个句柄 ID 的标量」。
     */
    public func paramHandle(): Int64 {
        unsafe { lvglcj_event_get_param_handle(evh) }
    }

    public func paramScalar(): Int64 {
        unsafe { lvglcj_event_get_param_scalar(evh) }
    }

    /// 阻止事件继续向父对象冒泡
    public func stopBubbling(): Unit {
        unsafe { lvglcj_event_stop_bubbling(evh) }
    }
}
