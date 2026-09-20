/*
 * hello_cj —— lvgl4cj 的第一个可交付物（设计文档 §17）
 *
 * 效果：SDL2 窗口 800×480，深色背景，中央一个圆角按钮；
 *       点击后按钮文字由「点我」变为「点了 N 次」；关闭窗口干净退出。
 *
 * ===================== 这段代码的顺序是**有讲究的**，不是风格问题 =====================
 * 方案 A（ADR-013 归档）下，LVGL 主循环跑在 C 侧独立 OS 线程上，
 * 因此「谁是 LVGL 线程」会在 startThreaded() 那一刻发生转移：
 *
 *   initLvgl()        ：当前（主）线程被登记为 LVGL 线程
 *   Sdl2Backend.start ：必须在**将要跑 SDL 事件循环的线程**上调用
 *                       —— 它创建窗口并把该线程登记为渲染线程
 *   建 UI（对象/样式/回调）：**必须在 startThreaded() 之前**做完，
 *                       因为此后主线程已不再是 LVGL 线程，
 *                       再调 lv_* 会被线程断言拒绝（WrongThread）
 *   startThreaded()   ：主循环线程接管 LVGL 线程身份
 *   事件循环          ：主线程只剩 SDL 事件轮询与「服务渲染请求」
 *
 * 顺序搞错的典型症状：界面能建出来但一点就抛 WrongThread，
 * 或者按钮位置/尺寸全是 0（布局发生在主循环线程上，而你读的是未结算的值）。
 */
package hello_cj

import std.collection.*
import std.time.*
import lvgl4cj.*

/*
 * 冒烟模式：`--smoke` 表示跑固定的若干轮后自动**干净退出**。
 *
 * 为什么示例需要它：默认行为是「一直跑到窗口关闭」，
 * 而退出路径恰恰是最容易出问题的地方（display 删除顺序、缓冲释放、句柄回收），
 * 却无法在无人干预的 CI 里触发。有了它就能自动跑一段再走完整退出流程。
 *
 * 刻意不做「--frames N」式的数字解析：那需要引入 std.convert 的 Parsable 扩展，
 * 而这里只需要一个固定预算 —— 用字符串比较就够了。
 * 少一层解析，就少一类「参数写错却静默跑成默认值」的可能。
 */
const SMOKE_ROUNDS: Int64 = 600

func wantSmoke(args: Array<String>): Bool {
    for (i in 0..args.size) {
        if (args[i] == "--smoke") {
            return true
        }
    }
    return false
}

main(args: Array<String>) {
    let smoke = wantSmoke(args)
    let frameBudget: Int64 = if (smoke) { SMOKE_ROUNDS } else { -1 }

    let rt = LvglRuntime()
    rt.initLvgl()

    /* 800×480 深色窗口（同时创建 display；缓冲由 C 侧持有，见 ADR-002） */
    let backend = Sdl2Backend.start(800, 480)
    let disp = backend.display()

    let scr = LvObject.screenActive()

    /* 深色背景 */
    LvStyle.create()
        .bgColor(rgb(0x12, 0x16, 0x1E))
        .bgOpa(255)
        .applyTo(scr)

    /*
     * ★ 中文字形：LVGL 内置的 Montserrat 只有拉丁字形，
     *   界面写中文必须显式用 CJK 字体，否则汉字会被静默跳过
     *   （日志报 "glyph dsc. not found for U+6211"，画面上按钮是空的）。
     */
    let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

    /* 中央圆角按钮 */
    let btn = LvButton.create(scr)
    btn.setSize(220, 72).align(LvAlign.Center)
    LvStyle.create()
        .bgColor(rgb(0x25, 0x63, 0xEB))
        .bgOpa(255)
        .radius(16)
        .textColor(rgb(0xFF, 0xFF, 0xFF))
        .textFont(cjk)
        .applyTo(btn)

    let label = btn.withLabel("点我")
    /* 标签再显式挂一次字体样式：不依赖「子对象从父对象继承 text_font」这一行为 */
    LvStyle.create().textFont(cjk).applyTo(label)

    /*
     * 点击计数。
     * ★ 用「let 绑定的可变容器」而不是 `var n = 0`：
     *   仓颉不允许捕获可变局部变量的 lambda 逃逸，而事件回调必然逃逸
     *   （它要存进闭包表、稍后由 LVGL 触发）。
     */
    let clicks = ArrayList<Int32>()
    clicks.add(0)
    btn.on(LvEventCode.Clicked) { _ =>
        clicks[0] = clicks[0] + 1
        label.setText("点了 ${clicks[0]} 次")
    }

    /* pointer indev：读回调把 SDL 采集到的鼠标状态喂给 LVGL */
    let indev = LvIndev.create(LvIndevType.Pointer).bind(disp)
    indev.onRead {
        backend.feedIndev()
    }

    /* 启动 C 侧主循环线程；此后主线程只负责 SDL 事件循环 */
    rt.startThreaded()

    println("[hello_cj] 窗口已启动（800x480，深色背景 + 居中按钮）；点击按钮查看计数")
    if (frameBudget > 0) {
        println("[hello_cj] 有界运行模式：${frameBudget} 轮后自动退出")
    }

    var rounds: Int64 = 0
    while (!backend.pollEvents()) {
        /* 2ms 轮询间隔：足够跟手，又不空转烧 CPU */
        sleep(Duration.millisecond * 2)
        rounds++
        if (frameBudget > 0 && rounds >= frameBudget) {
            break
        }
    }

    /*
     * ★ 退出顺序：先停主循环，再关后端，最后反初始化。
     *
     * rt.stop() 会把 LVGL 线程身份交回本线程（主循环线程已 join）——
     * 不做这一步，backend.close() 里的 display 删除会因「不是 LVGL 线程」
     * 而被拒绝，结果是界面看着退出正常，但 display 与绘制缓冲其实没释放。
     */
    /* 注意：命名参数必须写成 `名字: 值`，不能按位置传（仓颉规则，实测报
       "missing argument prefix 'shutdownMode:' for named parameter"） */
    rt.stop(shutdownMode: ShutdownMode.Discard)
    backend.close()
    rt.deinitLvgl()
    println("[hello_cj] 已干净退出；渲染帧数 ${backend.presentCount()}，累计点击 ${clicks[0]} 次")
}
