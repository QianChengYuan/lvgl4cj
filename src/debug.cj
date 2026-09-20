/*
 * debug.cj —— 调试与可观测性（设计文档 §7.3 / §5.11）
 *
 * 提供四类能力，对应 C 侧 native/src/debug.c：
 *   · 对象计数  objCount()   —— 与 handleCount(ALIVE) 对比，是「句柄泄漏」的交叉判据
 *   · 内存监控  memMonitor() —— LVGL 内存池用量/碎片/峰值
 *   · 性能采样  perfSample() —— FPS / 忙碌占比 / 刷新耗时 / 对象数
 *   · 对象树    dumpTree()   —— 前序展开成 (句柄, 深度, 名字)
 *
 * ==================== 出参为什么用「长度 1 的 Array」承载 ====================
 * C 侧这些接口的出参是裸指针（`lvglcj_perf_t*`、`uint32_t*`）。
 * 把 Cangjie 的地址交给 C 去写，需要一块**不受 GC 移动**的内存。
 * 三种可行写法的取舍：
 *   甲、LibC.malloc 手工分配 —— 要自己管释放，异常路径上极易漏 free；
 *   乙、把 @C struct 取地址 —— 仓颉不允许对栈上值取裸地址交给 C 长期写；
 *   丙、Array<T> + acquireArrayRawData（本实现）—— 取数组数据区地址，
 *       作用域结束后 releaseArrayRawData 即可；**没有手工 free**，
 *       异常路径也不会漏（数组由 GC 管）。
 *   选丙。
 *
 * ★ @C struct 里不能有 GC 指针（Cangjie 会拒绝），因此这里的 Array<T>
 *   承载的是纯值类型内存，读取时不会触发 GC 参与，安全性来自这个约束本身。
 *
 * ==================== 为什么这些类型定义在库里而不是测试里 ====================
 * CPerf / CTreeDump 原本只定义在 ffi_contract_test.cj 中（仅测试时编译）。
 * 但 LvDebug 需要在**库**里使用它们，因此下沉到本文件；
 * 契约测试改为校验**库里的定义** —— 这样测的就是真正被用的那份布局，
 * 而不是一份只在测试中存在的副本。
 */
package lvgl4cj

import std.collection.ArrayList
import lvgl4cj.generated.CONF_MEM_SIZE

/* ------------------------------------------------------------ C 侧布局镜像 */
/*
 * 字段顺序与类型必须与 native/include/lvglcj_bridge.h 严格一致；
 * sizeof 由 ffi_contract_test 与 C 侧导出的 C_SIZEOF_* 对照校验。
 * 改动任何字段都必须同步改两边，否则布局测试会立刻失败。
 */

/* lvglcj_perf_t：5 个 uint32_t */
@C
struct CPerf {
    var fps: UInt32 = 0
    var cpuPercent: UInt32 = 0
    var refrTimeMs: UInt32 = 0
    var drawTimeMs: UInt32 = 0
    var objCount: UInt32 = 0
}

/*
 * lvglcj_tree_dump_t：3 个指针 + 1 个 int32（注意指针后的对齐填充）。
 * ★ 这里用**带类型**的指针（Int64 / Int32 / CString），而不是测试早期的
 *   `CPointer<Unit>`：两者都是 8 字节、布局相同（sizeof 仍为 32），
 *   但带类型才能在读取时不做手工指针运算 —— 手工运算正是 FFI 最容易错的地方。
 */
@C
struct CTreeDump {
    var handles: CPointer<Int64> = CPointer<Int64>()
    var depths: CPointer<Int32> = CPointer<Int32>()
    var names: CPointer<CString> = CPointer<CString>()
    var count: Int32 = 0
}

/* ------------------------------------------------------------ 对外类型 */

/// 一次性能采样。各字段的口径见 native/src/debug.c 文件头（与官方 perf monitor 不保证逐位相同）
public struct LvPerf {
    /// 每秒完成的刷新次数（滑动 1s 窗口；首个窗口结算前为 0）
    public let fps: UInt32
    /// 最近 1s 内**在 lv_timer_handler 里消耗的墙钟时间**占比 —— 是「主循环忙碌比」而非进程 CPU 占用率
    public let cpuPercent: UInt32
    /// 最近一次 lv_timer_handler 的耗时（ms，含渲染与 flush）
    public let refrTimeMs: UInt32
    /// 最近一帧中 flush 回调累计耗时（ms，像素搬运部分）
    public let drawTimeMs: UInt32
    /// 当前对象总数（见 objCount 的口径说明）
    public let objCount: UInt32

    public init(fps: UInt32, cpuPercent: UInt32, refrTimeMs: UInt32, drawTimeMs: UInt32, objCount: UInt32) {
        this.fps = fps
        this.cpuPercent = cpuPercent
        this.refrTimeMs = refrTimeMs
        this.drawTimeMs = drawTimeMs
        this.objCount = objCount
    }
}

/// LVGL 内存池状态（§11.3：峰值 ≤ 配置值的 80%）
public struct LvMemStat {
    /// 已用百分比
    public let usedPercent: UInt32
    /// 碎片百分比
    public let fragPercent: UInt32
    /// 历史峰值字节数 —— 与总池大小比才是「峰值占比」
    public let maxUsedBytes: UInt32
    /// 当前空闲字节数
    public let freeBytes: UInt32

    public init(usedPercent: UInt32, fragPercent: UInt32, maxUsedBytes: UInt32, freeBytes: UInt32) {
        this.usedPercent = usedPercent
        this.fragPercent = fragPercent
        this.maxUsedBytes = maxUsedBytes
        this.freeBytes = freeBytes
    }
}

/// 对象树中的一个节点（前序展开）
public struct LvTreeNode {
    /// 对象句柄；不在句柄表内（例如 LVGL 自建的 layer）时为 0
    public let handle: Int64
    /// 相对 dump 根的深度，根为 0
    public let depth: Int32
    /// 句柄表登记的名字；未登记为 "?"
    public let name: String

    public init(handle: Int64, depth: Int32, name: String) {
        this.handle = handle
        this.depth = depth
        this.name = name
    }
}

/// 调试与可观测性入口（§7.3）
public class LvDebug {
    /*
     * 当前对象总数。
     *
     * ★ 口径（见 native/src/debug.c）：遍历每个 display 的 active screen 子树
     *   加上默认 display 的三个 layer 子树并**去重**。
     *   非默认 display 的 layer 拿不到公开 API，多 display 场景会少算。
     *
     * ★ 它的价值在于**与句柄表独立**：handleCount(ALIVE) 来自我们的表，
     *   本函数来自 LVGL 的对象树。两者差值恒定才是有效判据 ——
     *   若用「句柄表里 type==obj 的条数」充当它，就变成自己和自己比，判据失效。
     */
    public static func objCount(): Int32 {
        unsafe { lvglcj_obj_count() }
    }

    /// LVGL 内存池状态
    public static func memMonitor(): LvMemStat {
        /*
         * 4 个出参共用一块 4 元素的 UInt32 缓冲：第 i 个出参就是 &buf[i]。
         * 这样只需一次 acquire/release，而不是四次。
         */
        let buf = Array<UInt32>(4, repeat: 0)
        let h = unsafe { acquireArrayRawData(buf) }
        let rc = unsafe {
            lvglcj_mem_monitor(h.pointer, h.pointer + 1, h.pointer + 2, h.pointer + 3)
        }
        let st = LvMemStat(buf[0], buf[1], buf[2], buf[3])
        unsafe { releaseArrayRawData(h) }
        throwIfError(rc, "LvDebug.memMonitor")
        return st
    }

    /// 性能采样
    public static func perfSample(): LvPerf {
        let buf = Array<CPerf>(1, repeat: CPerf())
        let h = unsafe { acquireArrayRawData(buf) }
        let rc = unsafe { lvglcj_perf_sample(h.pointer) }
        let p = buf[0]
        unsafe { releaseArrayRawData(h) }
        throwIfError(rc, "LvDebug.perfSample")
        return LvPerf(p.fps, p.cpuPercent, p.refrTimeMs, p.drawTimeMs, p.objCount)
    }

    /*
     * 对象树 dump（前序）：调试布局、分析「谁没被删掉」。
     *
     * ★ C 侧会 malloc 数组，读完必须调 free_tree_dump ——
     *   这里用 try-finally 风格保证异常路径也释放（C 侧无 GC，漏了就是真泄漏）。
     *   注意 free 必须在 releaseArrayRawData **之前**：free 需要那块结构体本身。
     */
    public static func dumpTree(root!: LvObject, maxDepth!: Int32 = 8): ArrayList<LvTreeNode> {
        let out = Array<CTreeDump>(1, repeat: CTreeDump())
        let h = unsafe { acquireArrayRawData(out) }
        let n = unsafe { lvglcj_debug_dump_tree(root.handle, maxDepth, h.pointer) }

        let result = ArrayList<LvTreeNode>()
        if (n > 0) {
            let d = out[0]
            for (i in 0..Int64(n)) {
                /* 三个数组由 C 侧 malloc；CPointer 的 read(i) 取下第 i 个元素 */
                let hd = unsafe { d.handles.read(i) }
                let dp = unsafe { d.depths.read(i) }
                let nm = unsafe { d.names.read(i) }
                result.add(LvTreeNode(hd, dp, nm.toString()))
            }
        }
        /* 无论 n 是否为负都释放：dump_tree 在出错路径上也可能已分配过一部分 */
        unsafe { lvglcj_debug_free_tree_dump(h.pointer) }
        unsafe { releaseArrayRawData(h) }

        /*
         * ★ 返回值是**节点数**（与 handleCount 同一类，成功时为正），不是错误码，
         *   因此**不能**交给 throwIfError ——
         *   它会把任何非 0/非 1 的值当成错误码，于是「展开出 6 个节点」
         *   会被误判为 UnknownCode(6) 并抛异常。
         *   （实测踩到：soak 一启动就因此退出。）
         *   C 侧在出错路径上返回负值，因此这里只判负。
         */
        if (n < 0) {
            throwIfError(n, "LvDebug.dumpTree")
        }
        return result
    }

    /// 初始化 LVGL 内置 POSIX 文件系统驱动（字体/图片加载前置）
    public static func fsInitPosix(root!: String = "."): Unit {
        /*
         * root 缺省为 "."（即 LV_FS_POSIX_PATH 编译期值），语义是「用编译期根路径」。
         * 传其它值会得到 NOT_SUPPORTED —— 运行时换根需要自定义 lv_fs 驱动（P2），
         * C 侧对此明确报错而不是静默忽略，否则用户会面对「文件读不到但也不报错」的谜题。
         */
        let cs = unsafe { LibC.mallocCString(root) }
        let rc = unsafe { lvglcj_fs_init_posix(cs) }
        unsafe { LibC.free(cs) }
        throwIfError(rc, "LvDebug.fsInitPosix")
    }
}

/*
 * 内存池峰值占比（百分比）—— §11.3 的判据「峰值 ≤ 配置值的 80%」直接读它。
 *
 * 单独提供是因为 maxUsedBytes 本身没有意义：必须与池总量比。
 * 池总量从 freeBytes + 已用反推不可靠（有碎片），
 * 因此用「峰值 / (峰值 + 当前空闲 + 已用)」… 也不对 ——
 * 正确做法是用 memMonitor 的 used/max 与 conf 里的 LV_MEM_SIZE 比，
 * 而 LV_MEM_SIZE 已由 conf_const.cj 导出为 CONF_MEM_SIZE。
 */
public func memPeakPercent(): UInt32 {
    let st = LvDebug.memMonitor()
    if (CONF_MEM_SIZE <= 0) {
        return 0
    }
    return UInt32(UInt64(st.maxUsedBytes) * 100 / UInt64(CONF_MEM_SIZE))
}

/// 当前内存池空闲占比（百分比）—— 与 memPeakPercent 互补，用于观察稳态
public func memFreePercent(): UInt32 {
    let st = LvDebug.memMonitor()
    if (CONF_MEM_SIZE <= 0) {
        return 0
    }
    return UInt32(UInt64(st.freeBytes) * 100 / UInt64(CONF_MEM_SIZE))
}
