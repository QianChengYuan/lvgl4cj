/*
 * indev.cj —— 输入设备 L1 包装（设计文档 §5.4）
 *
 * ===================== 为什么读回调不接收参数 =====================
 * C 侧 T5 的约定是：trampoline 暂存「本次读取的 lv_indev_data_t」，
 * 仓颉回调通过 setter（lvglcj_indev_set_point 等）回填。
 * 这样仓颉侧**完全不需要知道** lv_indev_data_t 的内存布局 ——
 * 而那个结构体里还嵌了 lv_point_t，靠猜字段顺序去写会变成静默的输入错乱
 * （点了 A 触发 B），比崩溃更难查。
 *
 * 因此本层的读回调签名是 `() -> Unit`：它没有参数，因为输入数据不在参数里，
 * 而在「本次读取的上下文」里，通过 setIndevPoint / setIndevKey 写入。
 */
package lvgl4cj

import std.deriving.Derive

/*
 * 输入设备类型。数值与 C 侧 lvglcj_indev_type_t **以及 LVGL** 逐值对齐：
 *   NONE=0 POINTER=1 KEYPAD=2 BUTTON=3 ENCODER=4
 * （C 头文件早期曾把 BUTTON/ENCODER 写反，已在 t4 修正；
 *   这里保持一致，且两侧都有测试断言该对齐。）
 */
@Derive[Equatable] // cjlint-ignore !G.OPR.01 !G.ERR.01 派生生成的 ==/!= 与异常语义均非手写，属宏展开误报
public enum LvIndevType {
    | None // 0
    | Pointer // 1
    | Keypad // 2
    | Button // 3
    | Encoder // 4
}

func lvIndevTypeToInt(t: LvIndevType): Int32 {
    match (t) {
        case None => 0
        case Pointer => 1
        case Keypad => 2
        case Button => 3
        case Encoder => 4
    }
}

public class LvIndev {
    public let handle: Int64

    private init(h: Int64) {
        handle = h
    }

    /**
     * 创建一个输入设备（此时尚未绑定 display，用 bind() 绑定）。
     *
     * @throws[LvglException] 类型为 OutOfMemory —— C 侧无法分配 indev；
     *         WrongThread —— 在非 LVGL 线程上调用。
     */
    public static func create(t: LvIndevType): LvIndev {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_indev_create(lvIndevTypeToInt(t)) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "indev 创建失败")
        }
        return LvIndev(h)
    }

    /// 绑定显示设备（决定输入坐标映射到哪块屏幕）
    public func bind(display: LvDisplay): LvIndev {
        throwIfError(unsafe { lvglcj_indev_set_display(handle, display.handle) }, "indev.bind")
        return this
    }

    /*
     * 注册读取回调。回调内必须用 setIndevPoint / setIndevKey 之类回填本次输入，
     * 否则 LVGL 认为「无输入」（C 侧 trampoline 已把 data 复位为 RELEASED）。
     */
    public func onRead(cb: () -> Unit): LvIndev {
        let cid = registerClosure {_ => cb()}
        let rc = unsafe { lvglcj_indev_set_read_cb(handle, cid) }
        if (rc != 0) {
            /* 注册失败必须收回 cid，否则闭包表泄漏 */
            unregisterClosure(cid)
            throwIfError(rc, "indev.onRead")
        }
        return this
    }

    /**
     * 把本 indev 绑定到某个 Group。
     *
     * ★ 为什么需要它：**键盘与编码器型 indev 完全不看坐标** —— 它们的输入先送进 group，
     *   由 group 决定"当前焦点对象"，再发给那个对象。不绑定 group 的键盘按下去
     *   什么也不会发生（这正是设计文档 P1 里"Group 与键盘导航"缺的那一环）。
     *
     * @throws[LvglException] 句柄失效
     */
    public func bindGroup(g: LvGroup): LvIndev {
        throwIfError(unsafe { lvglcj_indev_set_group(handle, g.handle) }, "indev.bindGroup")
        return this
    }

    /// 幂等的关闭（C 侧严格返回 InvalidHandle，这里视为「已关过」）
    public func close(): Unit {
        ensureLvglOwnership()
        let rc = unsafe { lvglcj_indev_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return
        }
        throwIfError(rc, "indev.close")
    }
}

/*
 * ===================== Group（键盘 / 编码器导航）=====================
 *
 * ★ 为什么它比看上去重要：键盘与编码器型 indev **完全不看坐标** —— 它们的输入先送进
 *   group，由 group 决定"当前焦点对象"，再发给那个对象。没有 group，键盘按下去
 *   不会发生任何事。设计文档 §12 的 P1 阶段把"Group 与键盘导航"列为交付项，
 *   而此前 C 侧实现（native/src/indev.c）与 foreign 声明都已存在、**只缺这一层封装**
 *   和 bridge 头里的声明 —— 本批补齐，能力才真正可用。
 *
 * 句柄管理与 LvIndev 同构：group 不是 LVGL 对象，因此不参与对象树的级联失效，
 * 但 close() 幂等（重复关闭视为已关过）。
 */
public class LvGroup {
    public let handle: Int64

    private init(h: Int64) {
        handle = h
    }

    /**
     * 创建一个 group。
     *
     * @throws[LvglException] OutOfMemory —— C 侧没有空槽；WrongThread —— 非 LVGL 线程
     */
    public static func create(): LvGroup {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_group_create() }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "group 创建失败（C 侧槽位已满）")
        }
        return LvGroup(h)
    }

    /**
     * 把对象加入本组。
     *
     * LVGL 的约束是**一个对象只能属于一个组**，因此重复加入或加入已属别组的对象
     * 会由底层调整，而不是报错。
     *
     * @throws[LvglException] 句柄失效
     */
    public func add(obj: LvObject): LvGroup {
        throwIfError(unsafe { lvglcj_group_add_obj(handle, obj.handle) }, "group.add")
        return this
    }

    /**
     * 把对象移出它所在的组。
     *
     * 这个入口**不针对某个组**（底层只收对象）—— 因为"一个对象只属于一个组"，
     * 所以只需说"把它移出来"，无需指名道姓。
     *
     * @throws[LvglException] 句柄失效
     */
    public func remove(obj: LvObject): LvGroup {
        throwIfError(unsafe { lvglcj_group_remove_obj(obj.handle) }, "group.remove")
        return this
    }

    /**
     * 让某个对象取得焦点（对象须已在本组内）。
     *
     * @throws[LvglException] 句柄失效
     */
    public func focus(obj: LvObject): LvGroup {
        throwIfError(unsafe { lvglcj_group_focus_obj(obj.handle) }, "group.focus")
        return this
    }

    /**
     * 当前焦点对象；组内为空时为 `None`。
     *
     * @throws[LvglException] 句柄失效
     */
    public func focused(): Option<LvObject> {
        let h = unsafe { lvglcj_group_get_focused(handle) }
        if (h < 0) {
            throw LvglException(InvalidArgument, "group.focused 失败")
        }
        if (h == 0) {
            return None
        }
        /* 包回对象：LvObject 的构造函数是 internal（包内可见），正是为此准备的。
         * 对象若已被删除，拿到的是一个会正常报错的失效句柄 —— 与 keyboard.textarea() 同理。 */
        return Some(LvObject(h))
    }

    /**
     * 焦点移到下一个对象（到末尾回绕）。
     *
     * @throws[LvglException] 句柄失效
     */
    public func focusNext(): LvGroup {
        throwIfError(unsafe { lvglcj_group_focus_next(handle) }, "group.focusNext")
        return this
    }

    /**
     * 焦点移到上一个对象（到开头回绕）。
     *
     * @throws[LvglException] 句柄失效
     */
    public func focusPrev(): LvGroup {
        throwIfError(unsafe { lvglcj_group_focus_prev(handle) }, "group.focusPrev")
        return this
    }

    /**
     * 设为**默认组**：没有显式绑定组的 indev 会使用它。
     *
     * @throws[LvglException] 句柄失效
     */
    public func setDefault(): LvGroup {
        throwIfError(unsafe { lvglcj_group_set_default(handle) }, "group.setDefault")
        return this
    }

    /// 幂等的关闭。组内对象不受影响（它们仍可正常使用）。
    public func close(): Unit {
        ensureLvglOwnership()
        let rc = unsafe { lvglcj_group_delete(handle) }
        if (rc == lvglErrorToCode(InvalidHandle)) {
            return
        }
        throwIfError(rc, "group.close")
    }
}

/*
 * 回填本次输入（只能在 indev 读回调内调用；回调外调用返回 NotSupported）。
 * 这两个是**自由函数**而不是 LvIndev 的方法：它们作用于「当前正在读取的上下文」，
 * 与具体是哪个 indev 无关 —— C 侧用线程局部暂存，天然如此。
 */
public func setIndevPoint(x: Int32, y: Int32, pressed: Bool): Unit {
    let rc = unsafe { lvglcj_indev_set_point(x, y, if (pressed) {
        1
    } else {
        0
    }) }
    if (rc == lvglErrorToCode(NotSupported)) {
        /* 静默忽略是刻意的：读回调之外调用它没有意义，报错只会污染日志 */
        return
    }
    throwIfError(rc, "setIndevPoint")
}

public func setIndevKey(key: UInt32, pressed: Bool): Unit {
    let rc = unsafe { lvglcj_indev_set_key(key, if (pressed) {
        1
    } else {
        0
    }) }
    if (rc == lvglErrorToCode(NotSupported)) {
        return
    }
    throwIfError(rc, "setIndevKey")
}
