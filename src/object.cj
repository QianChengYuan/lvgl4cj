/*
 * object.cj —— 对象基类（设计文档 §5.5 / §6.2）
 *
 * ===================== 生命周期语义（与 §3.1.3 的分层约定一致）=====================
 * close() **幂等**，且要吞掉两类「其实没问题」的返回：
 *   · InvalidHandle ：已经关过了，或该对象已被父对象级联失效
 *   · OkDeferred(+1)：在回调里调的 close()，已转入延迟删除队列（最迟同帧生效）
 * 这两种都不该让用户看到异常。
 *
 * 反过来说，**其它**错误码必须原样抛出：例如在非 LVGL 线程调用会得到
 * WrongThread，静默吞掉会让用户以为删成功了，而对象其实还在。
 *
 * ===================== 关于 isAlive 的时序 =====================
 * 回调内调用 close() 会转入延迟删除，此时对象**尚未**真正删除，
 * isAlive 仍为 true；要到本帧 drain 之后才变 false。
 * 断言 6（「回调中 close 自身不崩且同帧内确实被删除」）测的就是这个时序。
 */
package lvgl4cj

import std.deriving.Derive

/*
 * 对齐方式。数值与 LVGL v9 的 lv_align_t 逐值一致。
 * ★ 注意 LV_ALIGN_DEFAULT = 0 是枚举第一项，因此 TOP_LEFT 从 1 开始 ——
 *   少算这一项会让所有对齐整体偏一格（画面看得见，但很容易误判成布局 bug）。
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvAlign {
    | Default // 0
    | TopLeft // 1
    | TopMid // 2
    | TopRight // 3
    | BottomLeft // 4
    | BottomMid // 5
    | BottomRight // 6
    | LeftMid // 7
    | RightMid // 8
    | Center // 9
}

func lvAlignToInt(a: LvAlign): Int32 {
    match (a) {
        case Default => 0
        case TopLeft => 1
        case TopMid => 2
        case TopRight => 3
        case BottomLeft => 4
        case BottomMid => 5
        case BottomRight => 6
        case LeftMid => 7
        case RightMid => 8
        case Center => 9
    }
}

/// 对象基类。所有控件都继承它。
public open class LvObject {
    /// C 侧对象句柄（自增 ID，非指针 —— ADR-001）
    public let handle: Int64

    internal init(h: Int64) {
        handle = h
    }

    /// 句柄是否有效。父对象被删除后，子对象的该属性会变为 false
    public prop isAlive: Bool {
        get() {
            unsafe { lvglcj_handle_alive(handle) } != 0
        }
    }

    /*
     * 活动屏幕。
     * ★ G1：必须先有 display，否则这里返回的句柄为 0，后续任何创建对象的调用
     *   都会触发 LVGL 断言并**直接终止进程**（不是抛异常）。
     *   因此正确的用法是先 runtime.startHeadless()/Sdl2Backend.start()，再取屏幕。
     */
    public static func screenActive(): LvObject {
        LvObject(unsafe { lvglcj_screen_active() })
    }

    /**
     * 创建一个基础对象（容器/普通对象）。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— LVGL 内存池已满；
     *         WrongThread —— 在非 LVGL 线程上调用。
     */
    public static func create(parent: LvObject): LvObject {
        /* 泵模式下先确认线程归属：创建路径常常跨在 sleep 之后（见 ensureLvglOwnership） */
        ensureLvglOwnership()
        /* 父句柄也要重新确认归属后再读，故先取一次（ptr 查询本身不做线程检查，
           但随后所有 lv_* 调用都需要；这里统一在入口确认一次即可） */
        let h = unsafe { lvglcj_obj_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "对象创建失败（LVGL 内存池可能已满）")
        }
        return LvObject(h)
    }

    // ---------------------------------------------------------- 几何
    public func setPos(x: Int32, y: Int32): LvObject {
        throwIfError(unsafe { lvglcj_obj_set_pos(handle, x, y) }, "setPos")
        return this
    }

    public func setSize(w: Int32, h: Int32): LvObject {
        throwIfError(unsafe { lvglcj_obj_set_size(handle, w, h) }, "setSize")
        return this
    }

    /**
     * 相对当前滚动位置滚动（像素）。内容没超出时会被钳制，**不抛错**。
     *
     * 正值 `dy` = 内容向下移动（看到更靠上的内容），与 LVGL 的 `lv_obj_scroll_by` 一致。
     * 滚动过程**不带动画**，调用返回后位置立即稳定 —— 因此"滚一下再读"是确定性的。
     *
     * ★ 这个方法也是滚轮的落点：LVGL 的滚轮（encoder 型 indev）在导航模式下走的是
     *   `lv_group_focus_prev/next`（**移动焦点**），指针通路只认拖动 —— 库里没有
     *   "滚轮滚动视图"的原生通路，所以由后端收 `SDL_MOUSEWHEEL`、调用方显式滚动。
     *
     * @throws[LvglException] 句柄失效
     */
    public func scrollBy(dx: Int32, dy: Int32): LvObject {
        throwIfError(unsafe { lvglcj_obj_scroll_by(handle, dx, dy) }, "scrollBy")
        return this
    }

    public func align(a: LvAlign, xOfs!: Int32 = 0, yOfs!: Int32 = 0): LvObject {
        throwIfError(unsafe { lvglcj_obj_align(handle, lvAlignToInt(a), xOfs, yOfs) }, "align")
        return this
    }

    public func x(): Int32 {
        unsafe { lvglcj_obj_get_x(handle) }
    }

    public func y(): Int32 {
        unsafe { lvglcj_obj_get_y(handle) }
    }

    public func width(): Int32 {
        unsafe { lvglcj_obj_get_width(handle) }
    }

    public func height(): Int32 {
        unsafe { lvglcj_obj_get_height(handle) }
    }

    /**
     * 该对象的矩形是否包含给定点（本地判断，用于断言「点击坐标正确」）。
     *
     * ★ 几何值先取到局部变量再用：这两点既是静态检查的要求
     *   （G.EXP.03：短路表达式的右操作数不应含副作用 —— 而 x()/width()
     *   都是跨 FFI 调用），也是性能上的必要 —— 命中测试在每次输入事件都会跑，
     *   原写法会重复走 FFI。
     */
    public func containsPoint(px: Int32, py: Int32): Bool {
        let x0 = x()
        let y0 = y()
        let w = width()
        let h = height()
        return px >= x0 && px < x0 + w && py >= y0 && py < y0 + h
    }

    // ---------------------------------------------------------- 子对象
    public func childCount(): Int32 {
        unsafe { lvglcj_obj_get_child_count(handle) }
    }

    public func childAt(idx: Int32): Int64 {
        unsafe { lvglcj_obj_get_child(handle, idx) }
    }

    // ---------------------------------------------------------- 样式
    public func addStyle(s: LvStyle, selector!: Int32 = LV_PART_MAIN | LV_STATE_DEFAULT): LvObject {
        throwIfError(unsafe { lvglcj_obj_add_style(handle, s.handle, selector) }, "addStyle")
        return this
    }

    public func removeStyle(s: LvStyle, selector!: Int32 = LV_PART_MAIN | LV_STATE_DEFAULT): LvObject {
        throwIfError(unsafe { lvglcj_obj_remove_style(handle, s.handle, selector) }, "removeStyle")
        return this
    }

    /*
     * 读回样式属性（颜色为 0xRRGGBB，其余为数值）。P0 用于断言「样式确实生效」。
     * 参数名用 propId 而非 prop —— 后者是仓颉关键字。
     */
    public func styleProp(propId: Int32, selector!: Int32 = LV_PART_MAIN | LV_STATE_DEFAULT): Int64 {
        unsafe { lvglcj_obj_get_style_prop(handle, propId, selector) }
    }

    // ---------------------------------------------------------- 事件
    /**
     * 注册事件回调。
     *
     * ★ 同一事件码可以注册**多个**回调，且全部按注册顺序触发（§3.7.2）——
     *   这是 LVGL 原生语义，不会互相覆盖。
     * ★ 绘制类事件（LV_EVENT_DRAW_*）不受支持，会抛 NotSupported（§3.7.3）。
     * ★ 回调注册后由对象持有：对象删除时 C 侧会通知仓颉侧注销对应闭包，
     *   因此使用方不需要（也无法）手动注销闭包；`close()` 掉对象即完成清理。
     *
     * @throws[LvglException] 类型为 NotSupported —— 该事件码不受支持（如绘制类事件）；
     *         InvalidHandle —— 对象句柄已失效。
     */
    public func on(code: LvEventCode, cb: (LvEvent) -> Unit): Unit {
        let cid = registerClosure {arg => cb(LvEvent(arg))}
        let dsc = unsafe { lvglcj_obj_add_event(handle, lvEventCodeToInt(code), cid) }
        if (dsc <= 0) {
            /*
             * 注册失败必须立刻注销刚分配的 cid，否则闭包表泄漏
             * （这条路径正是「队列满/不支持的事件码」时才会走到，容易漏测）。
             */
            unregisterClosure(cid)
            let code2 = if (dsc < 0) {
                Int32(dsc)
            } else {
                lvglErrorToCode(InvalidArgument)
            }
            throw LvglException(lvglErrorFromCode(code2), "注册事件回调失败（该事件码可能不受支持）")
        }
    }

    /// 主动派发事件（用于测试与自定义通知）。载荷为句柄时用这个
    public func sendEvent(code: LvEventCode, param!: Int64 = 0): Unit {
        throwIfError(unsafe { lvglcj_obj_send_event(handle, lvEventCodeToInt(code), param) }, "sendEvent")
    }

    /// 主动派发事件，载荷为标量（与 sendEvent 是两条**对称**路径）
    public func sendEventScalar(code: LvEventCode, scalar: Int64): Unit {
        throwIfError(unsafe { lvglcj_obj_send_event_scalar(handle, lvEventCodeToInt(code), scalar) }, "sendEventScalar")
    }

    // ---------------------------------------------------------- 生命周期
    /*
     * 关闭并释放该对象（含其子树）。
     *
     * 幂等；在回调内调用会转延迟删除（返回 OkDeferred），最迟同帧生效。
     */
    public func close(): Unit {
        ensureLvglOwnership()
        let rc = unsafe { lvglcj_obj_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return /* 已关过，或已被父对象级联失效 */
        }
        if (rc == lvglErrorToCode(OkDeferred)) {
            return /* +1：回调内删除，已入延迟队列 */
        }
        throwIfError(rc, "close")
    }
}
