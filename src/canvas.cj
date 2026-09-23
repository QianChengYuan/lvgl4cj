/*
 * canvas.cj —— Canvas 控件（设计文档 §3.11.2 / §6.5）
 *
 * ============================================================================
 * 【它不是"又补了一个控件"，而是自定义绘制的唯一出口】
 * ============================================================================
 * §3.11.1 把 lv_draw_* 明确列为**非目标**：本绑定不打算让用户接触绘制层。
 * §3.11.2 给出的替代路径就是 Canvas。所以 LvCanvas 的完成度直接决定
 * 「用户能不能画自己的东西」—— 它不是可选件。
 *
 * ============================================================================
 * 【像素缓冲：调用方只给尺寸，不给指针 —— 这是刻意的】
 * ============================================================================
 * setBuffer(w, h) **只接受尺寸**。缓冲由 C 侧分配，并随 canvas 对象一起释放
 * （缓存指针存在对象 user_data 里，DELETE 时 free）。
 *
 * 为什么不接收仓颉侧传进来的数组/指针（ADR-002）：
 *   仓颉的 GC 会**移动**对象。若把仓颉数组的地址交给 LVGL 长期持有，
 *   下一次 GC 移动它之后，LVGL 就在往一块已经不属于我们的内存里画。
 *   这类错误不会立即报错，而是在某次 GC 之后突然表现为画面撕裂或崩溃 ——
 *   所以这一层**根本不给**这样的调用形态。
 *
 * 由此带来的一条使用约束：改尺寸请调 setBuffer 重新设置（内部会先释放旧缓冲），
 * 而不是自己去管理缓冲。
 *
 * ============================================================================
 * 【★ 颜色参数的两种解释，务必看清】
 * ============================================================================
 *   · 绘制类（drawPoint/drawLine/drawRect/drawArc）与 fillBg：**0xRRGGBB**
 *     高 8 位不解释；透明度走 fillBg 的 `opa` 参数，其余绘制恒为不透明。
 *   · setPalette：**0xAARRGGBB** —— 调色板项本身就是带 alpha 的 32 位色。
 * 这是 ABI 层的既有不一致，映射到 L1 也没有抹平：抹平会悄悄改变
 * 已声明签名的语义，属于契约变更，不该顺手做。
 *
 * ============================================================================
 * 【两处已知缺口（如实写明，而不是让使用者自己踩）】
 * ============================================================================
 *   1. **线宽不可配**：drawLine / drawArc 的 ABI 里没有 width 参数，
 *      实现固定为 1 与 2。需要别的线宽要扩契约。
 *   2. **没有 drawPolygon**：设计文档 §3.11.2 的草稿里有它，
 *      但**冻结版桥接头只声明了 8 个 canvas 函数**，不含 polygon。
 *      以头文件为契约记录，故不实现；需要时应走契约变更。
 */
package lvgl4cj

/// 自定义绘制画布
public class LvCanvas <: LvObject {
    private init(h: Int64) {
        super(h)
    }

    /**
     * 创建一个 canvas。
     *
     * 注意：创建后**必须**先 `setBuffer` 才能绘制 —— 未设缓冲时的绘制操作
     * 会明确抛 InvalidArgument，而不是崩在 LVGL 内部。
     *
     * @throws[LvglException] OutOfMemory —— LVGL 内存池已满；
     *         WrongThread —— 非 LVGL 线程；InvalidHandle —— parent 句柄无效。
     */
    public static func create(parent: LvObject): LvCanvas {
        ensureLvglOwnership()
        let h = unsafe { lvglcj_canvas_create(parent.handle) }
        if (h <= 0) {
            throw LvglException(OutOfMemory, "canvas 创建失败（LVGL 内存池已满，或未启动显示导致没有默认屏幕）")
        }
        return LvCanvas(h)
    }

    /**
     * 设置像素缓冲（尺寸单位是像素，格式固定 ARGB8888）。
     *
     * 缓冲**由 C 侧分配**、随本对象释放；重复调用会先释放旧缓冲。
     *
     * @throws[LvglException] InvalidArgument —— 宽高非正，或尺寸大到超过 2GB；
     *         OutOfMemory —— 分配失败
     */
    public func setBuffer(w: Int32, h: Int32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_set_buffer(handle, w, h) }, "canvas.setBuffer")
        return this
    }

    /**
     * 设置调色板项。颜色是 **0xAARRGGBB**（含 alpha）——
     * 与绘制类函数的 0xRRGGBB 不同，原因见文件头。
     */
    public func setPalette(idx: Int32, color: UInt32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_set_palette(handle, idx, color) }, "canvas.setPalette")
        return this
    }

    /**
     * 用颜色填充整块画布，**不透明**（等价于 `fillBg(color, 255)`）。
     *
     * ★ 为什么是重载而不是默认参数：**仓颉不支持 `参数 = 默认值` 这种写法**
     *   （实测报 `expected ',' or ')', found '='`）。
     *   设计文档 §6.5 的示例只传颜色，所以要有一个单参形态。
     *   这与 src/widgets.cj 里 withCString 的取舍一致：
     *   宁可多一个短重载，也不在实现里塞一个"看不出默认值从哪来"的分支。
     */
    public func fillBg(color: UInt32): LvCanvas {
        return fillBg(color, 255)
    }

    /// 用颜色填充整块画布。颜色是 **0xRRGGBB**，透明度由 `opa`（0..255）单独给出
    public func fillBg(color: UInt32, opa: Int32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_fill_bg(handle, color, opa) }, "canvas.fillBg")
        return this
    }

    /// 画一个像素点。颜色 0xRRGGBB
    public func drawPoint(x: Int32, y: Int32, color: UInt32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_draw_point(handle, x, y, color) }, "canvas.drawPoint")
        return this
    }

    /// 画线段（线宽固定为 1，ABI 未暴露线宽）。颜色 0xRRGGBB
    public func drawLine(x1: Int32, y1: Int32, x2: Int32, y2: Int32, color: UInt32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_draw_line(handle, x1, y1, x2, y2, color) }, "canvas.drawLine")
        return this
    }

    /**
     * 画填充矩形（左上角坐标 + 宽高）。
     *
     * @throws[LvglException] InvalidArgument —— 宽或高非正
     */
    public func drawRect(x: Int32, y: Int32, w: Int32, h: Int32, color: UInt32): LvCanvas {
        throwIfError(unsafe { lvglcj_canvas_draw_rect(handle, x, y, w, h, color) }, "canvas.drawRect")
        return this
    }

    /**
     * 画弧（圆心 + 半径 + 起止角，角度单位为度，线宽固定为 2）。
     *
     * @throws[LvglException] InvalidArgument —— 半径非正或超过 65535
     *         （LVGL 的半径字段是 uint16_t，超出会静默截断）
     */
    // cjlint-ignore !G.FUN.01
    // 参数个数缩不了：画一段弧至少需要「圆心 x/y、半径、起止角、颜色」六项。
    // 任何打包成结构体的做法都会改变**设计文档 §6.5 已写明的调用形态**
    // （canvas.drawArc(cx, cy, radius, startAngle, endAngle, color)）。
    // 契约变更不该顺手做，所以显式抑制并写明原因。
    public func drawArc(cx: Int32, cy: Int32, r: Int32, startAngle: Int32, endAngle: Int32, color: UInt32): LvCanvas { // cjlint-ignore !G.FUN.01
        throwIfError(unsafe { lvglcj_canvas_draw_arc(handle, cx, cy, r, startAngle, endAngle, color) }, "canvas.drawArc")
        return this
    }
}
