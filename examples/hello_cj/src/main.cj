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
/* tryParse 是 Parsable 接口上的扩展函数 —— 不显式导入该接口就不在作用域里，
 * 而报错是一句很难对应的「you must import at least one of its inherited interfaces」。 */
import std.convert.Parsable
import lvgl4cj.*

/*
 * 冒烟模式：`--smoke` 表示跑固定的若干轮后自动**干净退出**。
 *
 * 为什么示例需要它：默认行为是「一直跑到窗口关闭」，
 * 而退出路径恰恰是最容易出问题的地方（display 删除顺序、缓冲释放、句柄回收），
 * 却无法在无人干预的 CI 里触发。有了它就能自动跑一段再走完整退出流程。
 *
 * 帧预算刻意**不做**「--frames N」式解析：这里只需要一个固定值，字符串比较就够。
 * （`--size` 是另一回事 —— 它必须接受真实数值，所以那边用了 tryParse，
 *   并且解析失败就报错退出，见 parseSize 的说明。）
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

/*
 * libc 的 exit：参数写错时必须以**非 0** 退出。
 *
 * 用它而不是「打印用法后正常返回」，是因为那个返回码是 0 ——
 * 脚本会把「参数写错了」当成「跑成功了」，这正是最该避免的静默。
 */
foreign func exit(code: Int32): Unit

func fatal(msg: String): Nothing {
    eprintln(msg)
    unsafe { exit(2) }
    throw Exception("unreachable: exit 不返回")
}

/*
 * 解析 `--size WxH`（例如 `1024x600`）。未给出返回 None；给出但格式不对**直接报错退出**。
 *
 * ★ 为什么 hello_cj 需要它：后端建的是**非全屏**窗口（backend 里是 SDL_WINDOW_SHOWN），
 *   而在 KMSDRM 下窗口尺寸必须**精确匹配**一个内核已有的显示模式 ——
 *   SDL 不会替你挑一个合适的尺寸。所以嵌入式上屏的正确姿势是：
 *       先看内核给出的模式： head -1 /sys/class/drm/card0-HDMI-A-1/modes   # 例如 1024x600
 *       再原样传进来：       主程序 --size 1024x600
 *
 * ★ 解析刻意手写、且**绝不静默回落到默认值**：尺寸没生效会让后面所有现象都指向
 *   错误方向（比如"画面只占左上角一块"），排查代价远大于这里多出的二十行。
 */
func parseSize(text: String): Option<(Int32, Int32)> {
    var out: Option<(Int32, Int32)> = None
    match (text.indexOf("x")) {
        case Some(i) =>
            if (i > 0 && i < text.size - 1) {
                let a = Int32.tryParse(text[0..i])
                let b = Int32.tryParse(text[i + 1..text.size])
                if (a.isSome() && b.isSome()) {
                    let w = a.getOrThrow()
                    let h = b.getOrThrow()
                    /* 上限顺带拦掉「少写一个 0」这类输入 */
                    if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
                        out = Some((w, h))
                    }
                }
            }
        case None => ()
    }
    return out
}

func takeSize(args: Array<String>): Option<(Int32, Int32)> {
    for (i in 0..args.size) {
        if (args[i] == "--size") {
            if (i + 1 >= args.size) {
                fatal("[hello_cj] --size 后面缺少值；正确写法：--size 1024x600")
            }
            match (parseSize(args[i + 1])) {
                case Some(wh) => return Some(wh)
                case None => fatal("[hello_cj] --size 的值 \"${args[i + 1]}\" 不是 WxH 形式（例如 1024x600）")
            }
        }
    }
    return None
}

/*
 * `--shot <路径>`：跑完把当前画面写成文件（**P6 PPM**，不是 PNG —— 后端实现如此）。
 *
 * 为什么需要：嵌入式上屏时，「屏幕上到底有没有画面」这件事必须能被**带走**。
 * 有了它，目标机上跑一次就能把这一帧传回宿主机分析（scripts/check_ui.py 能直接读 PPM），
 * 而不必依赖「我在屏幕前看到了」这种无法复核的说法。
 */
func takeShot(args: Array<String>): Option<String> {
    for (i in 0..args.size) {
        if (args[i] == "--shot") {
            if (i + 1 >= args.size) {
                fatal("[hello_cj] --shot 后面缺少文件路径")
            }
            return Some(args[i + 1])
        }
    }
    return None
}

main(args: Array<String>) {
    let smoke = wantSmoke(args)
    let frameBudget: Int64 = if (smoke) {
        SMOKE_ROUNDS
    } else {
        -1
    }

    /* 尺寸：默认 800×480（桌面）；KMSDRM 上必须传内核给出的模式，见 parseSize 的说明 */
    var width: Int32 = 800
    var height: Int32 = 480
    match (takeSize(args)) {
        case Some(wh) =>
            width = wh[0]
            height = wh[1]
        case None => ()
    }
    let shotPath = takeShot(args)

    let rt = LvglRuntime()
    rt.initLvgl()

    /* 深色窗口（同时创建 display；缓冲由 C 侧持有，见 ADR-002） */
    let backend = Sdl2Backend.start(width, height)
    let disp = backend.display()

    let scr = LvObject.screenActive()

    /* 深色背景 */
    LvStyle.create().bgColor(rgb(0x12, 0x16, 0x1E)).bgOpa(255).applyTo(scr)

    /*
     * ★ 中文字形：LVGL 内置的 Montserrat 只有拉丁字形，
     *   界面写中文必须显式用 CJK 字体，否则汉字会被静默跳过
     *   （日志报 "glyph dsc. not found for U+6211"，画面上按钮是空的）。
     */
    let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

    /* 中央圆角按钮 */
    let btn = LvButton.create(scr)
    btn.setSize(220, 72).align(LvAlign.Center)
    LvStyle
        .create()
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
    btn.on(LvEventCode.Clicked) {
        _ =>
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

    /*
     * ★ 打印**实际生效**的视频驱动名，而不是我们请求的那个。
     *
     *   环境变量 SDL_VIDEODRIVER 只是一个**请求**：驱动不可用时（没有已连接的显示器、
     *   DRM master 被桌面占着、库没编 KMS 支持）SDL **不报错**，而是静默回落到
     *   它还能用的驱动 —— 程序照跑、帧照 Present。不查这一项，就会把"回落成功"
     *   误读成"kmsdrm 成功"。嵌入式上屏时，这一行就是判定依据。
     */
    println("[hello_cj] 窗口已启动（${width}x${height}，视频驱动 ${backend.videoDriverName()}，深色背景 + 居中按钮）")
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
     * ★ 计数必须在**关闭之前**取走。
     *
     *   sdl2 后端的 deinit 会把 flush/present 等计数清零（"下次 init 从 0 开始"），
     *   而退出行原本写在 close() 之后 —— 于是它读到的**恒为 0**。
     *   这一行恰恰是"帧到底有没有上屏"的证据（嵌入式上屏时最该看的就是它），
     *   假信息比没有信息坏得多：它会让"一帧都没上屏"看起来像"正常退出了"。
     */
    let frames = backend.presentCount()
    let flushes = backend.flushCount()
    let waits = backend.waitTimeoutCount()
    let stale = backend.staleApplyCount()
    let clobbers = backend.slotClobberCount()

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

    /*
     * 截图放在 stop 之后、close 之前，顺序是刻意的：
     *   · stop 之后 LVGL 不再产生新帧 —— 纹理里是**稳定**的一帧，不会截到半张；
     *   · close 之前 display 与纹理都还在（display 的删除发生在 close 里）。
     * 截图读的是 SDL 纹理（不是 LVGL 的绘制缓冲），因此与 LVGL 是否在跑无关。
     */
    match (shotPath) {
        case Some(p) => try {
            LvDebug.screenshot(p)
            println("[hello_cj] 截图已写出：${p}（P6 PPM）")
        } catch (e: LvglException) {
            /* 截图失败不该让退出路径半途而废，但必须说出来 */
            eprintln("[hello_cj] 截图失败：${lvglErrorName(e.code)}（路径 ${p}）")
        }
        case None => ()
    }

    backend.close()
    rt.deinitLvgl()
    /*
     * ★ 这一行是嵌入式上屏的**判据行**，所以把关键计数都摆上来：
     *   · frames / flushes —— 帧有没有真的被 Present；
     *   · waits（等待超时）—— 正常应恒为 0，非 0 说明渲染跟不上；
     *   · stale（序号不匹配）—— 同样是 0 才算干净。它非 0 的含义很具体：
     *     某次 flush 等到的是**上一次**留下的额度，于是它覆盖的那条带
     *     内容被后续 flush 覆盖掉了。画面上表现为**成块**的区域颜色不对
     *     （全新纹理上是纯黑，旧纹理上是残留的旧内容/重影）。
     */
    println(
        "[hello_cj] 已干净退出；渲染帧数 ${frames}（flush ${flushes}，等待超时 ${waits}，"
            + "staleApply ${stale}，slotClobber ${clobbers}）；累计点击 ${clicks[0]} 次")
}
