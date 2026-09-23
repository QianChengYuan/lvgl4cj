/*
 * gallery_cj —— 全控件示例（设计文档 §7.1 的 P1 清单，19 个控件）
 *
 * 它有两个用途，这两个用途决定了代码结构：
 *
 *   ① **看**：把 19 个控件一次性摆在窗口里，肉眼确认渲染是否正常；
 *   ② **量**：屏幕上有一个每秒刷新的统计行（FPS / 刷新耗时 / 对象数 / 动画数 / 任务队列），
 *      退出时还会把最后一次采样打到 stdout —— 于是「看着不对」能变成一串可比对的数字。
 *
 * ★ 针对"控件抖动"这类现象，特意留了两个对照开关：
 *     --static : 关掉全部动画与周期数据更新。若静止时不抖、动起来才抖，
 *                问题就落在刷新/动画这一侧，而不是布局或绘制本身；
 *     --smoke  : 有界运行（固定轮数）后干净退出，用于无人值守地验证"能建、能跑、能退"。
 *
 * ★ 初始化顺序与 hello_cj 一致，且**不是风格问题**（ADR-013）：
 *   initLvgl() 把当前线程登记为 LVGL 线程；startThreaded() 之后主线程不再是 LVGL 线程，
 *   再调 lv_* 会被线程断言拒绝。所以：**所有建 UI 的代码必须在 startThreaded() 之前**，
 *   之后要用 LVGL 只能通过 rt.post { }（在 LVGL 线程上执行）。
 *
 * ★ 链式调用的一条硬规则（写这个示例时踩到）：setPos/setSize/align 返回的是 **LvObject**
 *   （基类），所以"先 setPos 再 setColor"会丢类型、编译不过。
 *   本文件统一写成 **控件自有方法在前、对象通用方法在后**：
 *       x.setRange(0, 100).setValue(30).setPos(PAD, 44).setSize(220, 18)
 */
package gallery_cj

import std.collection.*
import std.time.*
import lvgl4cj.*

const SMOKE_ROUNDS: Int64 = 900

/* 卡片排版：屏幕 800 宽，两侧各留 16 */
const CARD_W: Int32 = 768
const PAD: Int32 = 16

func hasFlag(args: Array<String>, name: String): Bool {
    for (i in 0..args.size) {
        if (args[i] == name) {
            return true
        }
    }
    return false
}

/* 造一张卡片（圆角容器 + 左上角标题），返回卡片本身供放控件 */
func makeCard(parent: LvObject, y: Int32, h: Int32, title: String, cjk: LvFont): LvObject {
    let c = LvObject.create(parent)
    c.setPos(PAD, y).setSize(CARD_W, h)
    LvStyle
        .create()
        .bgColor(rgb(0x1E, 0x24, 0x30))
        .bgOpa(255)
        .radius(10)
        .borderWidth(1)
        .borderColor(rgb(0x30, 0x3A, 0x4A))
        .padAll(0)
        .applyTo(c)

    let t = LvLabel.create(c)
    t.setText(title).setPos(PAD, 6)
    LvStyle.create().textColor(rgb(0x7A, 0xA2, 0xF7)).textFont(cjk).applyTo(t)
    return c
}

/* 小工具：给对象上正文样式。
 * ★ 形参用 LvObject 而不是 LvLabel：勾选框这类控件本身也要上同样的正文样式，
 *   而它们不是 LvLabel —— 写成 LvLabel 会让那些调用点编译不过。 */
func bodyStyle(l: LvObject, cjk: LvFont, twoLine!: Bool = false): Unit {
    let s = LvStyle.create().textColor(rgb(0xC8, 0xD0, 0xDC)).textFont(cjk)
    if (twoLine) {
        s.textLineSpace(4)
    }
    s.applyTo(l)
}

main(args: Array<String>) {
    let smoke = hasFlag(args, "--smoke")
    let staticMode = hasFlag(args, "--static")
    let frameBudget: Int64 = if (smoke) {
        SMOKE_ROUNDS
    } else {
        -1
    }

    let rt = LvglRuntime()
    rt.initLvgl()

    let backend = Sdl2Backend.start(800, 480)
    let disp = backend.display()
    let scr = LvObject.screenActive()
    let cjk = LvFont.builtin(LvBuiltinFont.Cjk16)

    LvStyle.create().bgColor(rgb(0x12, 0x16, 0x1E)).bgOpa(255).applyTo(scr)

    /* 根容器比屏幕高 → 内容超出后可用鼠标按住拖动滚动（LVGL 基础对象的默认行为） */
    let root = LvObject.create(scr)
    root.setPos(0, 0).setSize(800, 1420)
    LvStyle.create().bgOpa(0).padAll(0).applyTo(root)

    var y: Int32 = 8

    /* ================= ⓪ 统计（放最上面，方便与下面的画面直接对照） ================= */
    let statCard = makeCard(root, y, 96, "运行统计（每秒刷新；--static 可关掉动画做对照）", cjk)
    let stat = LvLabel.create(statCard)
    stat.setText("采样中…").setPos(PAD, 30).setSize(CARD_W - 2 * PAD, 60)
    bodyStyle(stat, cjk, twoLine: true)
    y += 104

    /* ================= ① LvLabel ================= */
    let c1 = makeCard(root, y, 96, "① LvLabel —— 文本（长文本自动换行）", cjk)
    let lab = LvLabel.create(c1)
    lab.setText("普通标签；这一段刻意写得长一些，用来观察换行与行距是否稳定、有没有逐帧跳动。")
    lab.setPos(PAD, 32).setSize(CARD_W - 2 * PAD, 54)
    bodyStyle(lab, cjk, twoLine: true)
    y += 104

    /* ================= ② LvButton ================= */
    let c2 = makeCard(root, y, 88, "② LvButton —— 点击事件（回调在 LVGL 线程上执行）", cjk)
    let btn = LvButton.create(c2)
    btn.setPos(PAD, 34).setSize(220, 44)
    LvStyle
        .create()
        .bgColor(rgb(0x25, 0x63, 0xEB))
        .bgOpa(255)
        .radius(10)
        .textColor(rgb(0xFF, 0xFF, 0xFF))
        .textFont(cjk)
        .applyTo(btn)
    let btnLabel = btn.withLabel("点我")
    LvStyle.create().textFont(cjk).applyTo(btnLabel)
    let clicks = ArrayList<Int32>()
    clicks.add(0)
    btn.on(LvEventCode.Clicked) {
        _ =>
            clicks[0] = clicks[0] + 1
            btnLabel.setText("点了 ${clicks[0]} 次")
    }
    y += 96

    /* ================= ③ LvSwitch / LvCheckbox ================= */
    let c3 = makeCard(root, y, 88, "③ LvSwitch / LvCheckbox —— 开关与勾选", cjk)
    let sw = LvSwitch.create(c3)
    sw.setChecked(true).setPos(PAD, 40)
    let swLab = LvLabel.create(c3)
    swLab.setText("开关").setPos(PAD + 80, 42)
    bodyStyle(swLab, cjk)

    let cb = LvCheckbox.create(c3)
    cb.setText("勾选")
    cb.setChecked(true).setPos(PAD + 220, 40)
    bodyStyle(cb, cjk)
    y += 96

    /* ================= ④ LvLed / LvSpinner ================= */
    let c4 = makeCard(root, y, 88, "④ LvLed / LvSpinner —— 指示与忙碌", cjk)
    let led = LvLed.create(c4)
    led.setColor(0x00E676)
    led.setBrightness(255)
    led.setOn(true)
    led.setPos(PAD, 40)

    let spin = LvSpinner.create(c4)
    spin.setPos(PAD + 80, 34).setSize(48, 48)

    let ledLab = LvLabel.create(c4)
    ledLab.setText("LED 亮度由动画驱动（--static 下为固定值）").setPos(PAD + 150, 42)
    bodyStyle(ledLab, cjk)
    y += 96

    /* ================= ⑤ LvBar / LvSlider / LvArc（三个都由动画驱动，抖动最易现形） ================= */
    let c5 = makeCard(root, y, 116, "⑤ LvBar / LvSlider / LvArc —— 数值显示（三个都由 LvAnim 驱动）", cjk)
    let bar = LvBar.create(c5)
    bar.setRange(0, 100)
    bar.setValue(30)
    bar.setPos(PAD, 44).setSize(220, 18)

    let slider = LvSlider.create(c5)
    slider.setRange(0, 100)
    slider.setValue(50)
    slider.setPos(PAD, 76).setSize(220, 18)

    let arc = LvArc.create(c5)
    arc.setRange(0, 100)
    arc.setValue(20)
    arc.setPos(PAD + 280, 38).setSize(68, 68)

    let numLab = LvLabel.create(c5)
    numLab.setText("数值随动画往返变化").setPos(PAD + 380, 58)
    bodyStyle(numLab, cjk)
    y += 124

    /* ================= ⑥ LvDropdown / LvRoller ================= */
    let c6 = makeCard(root, y, 116, "⑥ LvDropdown / LvRoller —— 选择（选项串由 LVGL 拷贝，两种模式都拷贝）", cjk)
    let dd = LvDropdown.create(c6)
    dd.setOptions("选项甲\n选项乙\n选项丙")
    dd.setSelected(0)
    dd.setPos(PAD, 40).setSize(200, 40)

    let roller = LvRoller.create(c6)
    roller.setOptions("一\n二\n三\n四")
    roller.setSelected(1)
    roller.setVisibleRowCount(3)
    roller.setPos(PAD + 260, 38).setSize(150, 70)

    let selLab = LvLabel.create(c6)
    selLab.setText("滚轮：点击后上下滚动").setPos(PAD + 440, 58)
    bodyStyle(selLab, cjk)
    y += 124

    /* ================= ⑦ LvTextarea + LvKeyboard ================= */
    let c7 = makeCard(root, y, 252, "⑦ LvTextarea + LvKeyboard —— 输入（点击按键会写入下面的输入框）", cjk)
    let ta = LvTextarea.create(c7)
    ta.setOneLine(true)
    ta.setPlaceholderText("在这里输入…")
    ta.setText("已输入文本")
    ta.setPos(PAD, 34).setSize(360, 46)

    let kb = LvKeyboard.create(c7)
    kb.setMode(LVGLCJ_KEYBOARD_MODE_NUMBER)
    kb.setPopovers(true)
    kb.setTextarea(ta)
    kb.setPos(PAD, 90)

    let kbLab = LvLabel.create(c7)
    kbLab.setText("数字键盘（全键盘占高太大，这里用数字模式）\n键盘与输入框的绑定：删除输入框前必须先 clearTextarea")
    kbLab.setPos(PAD + 330, 50).setSize(CARD_W - (PAD + 330) - PAD, 90)
    bodyStyle(kbLab, cjk, twoLine: true)
    y += 260

    /* ================= ⑧ LvCanvas / LvLine ================= */
    let c8 = makeCard(root, y, 146, "⑧ LvCanvas / LvLine —— 自绘（画布缓冲由 C 侧持有）", cjk)
    let cv = LvCanvas.create(c8)
    cv.setBuffer(140, 96)
    cv.fillBg(rgb(0x20, 0x28, 0x36))
    cv.drawRect(8, 8, 56, 32, rgb(0x00, 0xE6, 0x76))
    cv.drawLine(0, 92, 139, 10, rgb(0xEF, 0x53, 0x50))
    cv.drawArc(100, 46, 26, 0, 280, rgb(0x42, 0xA5, 0xF5))
    cv.setPos(PAD, 34)

    /* 折线：逐项赋值，不依赖数组字面量语法 */
    let pts = Array<Int32>(12, repeat: 0)
    pts[0] = 0
    pts[1] = 60
    pts[2] = 24
    pts[3] = 20
    pts[4] = 48
    pts[5] = 70
    pts[6] = 72
    pts[7] = 30
    pts[8] = 96
    pts[9] = 55
    pts[10] = 120
    pts[11] = 12
    let line = LvLine.create(c8)
    line.setPoints(pts)
    line.setPos(PAD + 170, 34).setSize(140, 96)

    let drawLab = LvLabel.create(c8)
    drawLab.setText("左：画布（矩形/直线/弧）\n右：折线").setPos(PAD + 330, 60)
    bodyStyle(drawLab, cjk, twoLine: true)
    y += 154

    /* ================= ⑨ LvChart ================= */
    let c9 = makeCard(root, y, 152, "⑨ LvChart —— 曲线（两条 series，持续推入数据点）", cjk)
    let chart = LvChart.create(c9)
    chart.setType(LVGLCJ_CHART_TYPE_LINE)
    chart.setPointCount(24)
    chart.setRange(LVGLCJ_CHART_AXIS_PRIMARY_Y, 0, 100)
    chart.setRange(LVGLCJ_CHART_AXIS_SECONDARY_Y, 0, 50)
    chart.setPos(PAD, 34).setSize(440, 108)
    let s0 = chart.addSeries(0xEF5350)
    let s1 = chart.addSeries(0x42A5F5, axis: LVGLCJ_CHART_AXIS_SECONDARY_Y)

    let chartLab = LvLabel.create(c9)
    chartLab.setText("红：主轴 0..100\n蓝：副轴 0..50（series 用索引标识，见契约）").setPos(PAD + 470, 56)
    bodyStyle(chartLab, cjk, twoLine: true)
    y += 160

    /* ================= ⑩ LvTable / LvImage ================= */
    let c10 = makeCard(root, y, 152, "⑩ LvTable / LvImage —— 表格与图片", cjk)
    let tb = LvTable.create(c10)
    tb.setColumnCount(3)
    tb.setRowCount(3)
    tb.setCellValue(0, 0, "列一")
    tb.setCellValue(0, 1, "列二")
    tb.setCellValue(0, 2, "列三")
    tb.setCellValue(1, 0, "1")
    tb.setCellValue(1, 1, "2")
    tb.setCellValue(1, 2, "3")
    tb.setCellValue(2, 0, "合并 →")
    tb.mergeRight(2, 0)
    tb.setTextCrop(2, 0, true)
    tb.setPos(PAD, 34).setSize(320, 108)

    /* 图片：★ 这里**故意**指向不存在的路径，用来展示一条契约 ——
     * 路径不存在时 LVGL 不报错、只是不显示（本层无法提前发现）。
     * 要看真实图片还需两步：启用解码器 + 提供存在的文件，都不属于本示例的范围。 */
    let img = LvImage.create(c10)
    img.setScale(256)
    img.setSrc("A:/no/such/image.bin")
    img.setPos(PAD + 350, 40).setSize(80, 80)

    let imgLab = LvLabel.create(c10)
    imgLab.setText("图片：路径不存在时不显示\n（LVGL 对此不报错，见契约）").setPos(PAD + 450, 60)
    bodyStyle(imgLab, cjk, twoLine: true)

    /* ================= 动画（除非 --static） =================
     * 用 LvAnim 而不是在主循环里手改数值：动画由 LVGL 在 **LVGL 线程**上推进，
     * 主循环只管 SDL 事件 —— 这也是唯一正确的做法（主循环已不是 LVGL 线程）。 */
    if (!staticMode) {
        LvAnim.create().target(bar).values(0, 100).duration(1200).repeat(1000000).onExec {v => bar.setValue(v)}.start()
        LvAnim
            .create()
            .target(slider)
            .values(0, 100)
            .duration(1800)
            .repeat(1000000)
            .onExec {v => slider.setValue(v)}
            .start()
        LvAnim.create().target(arc).values(0, 100).duration(900).repeat(1000000).onExec {v => arc.setValue(v)}.start()
        LvAnim
            .create()
            .target(led)
            .values(30, 255)
            .duration(700)
            .repeat(1000000)
            .onExec {v => led.setBrightness(v)}
            .start()
    }

    /* pointer indev：读回调把 SDL 采集到的鼠标状态喂给 LVGL */
    let indev = LvIndev.create(LvIndevType.Pointer).bind(disp)
    indev.onRead {
        backend.feedIndev()
    }

    /* 启动 C 侧主循环线程；此后主线程只负责 SDL 事件循环 */
    rt.startThreaded()

    println("[gallery_cj] 窗口已启动：19 个控件分 11 张卡片；画面超出屏幕时可按住鼠标拖动滚动")
    if (staticMode) {
        println("[gallery_cj] --static：全部动画已关闭（用于对照「静止时是否也抖」）")
    }
    if (frameBudget > 0) {
        println("[gallery_cj] --smoke：${frameBudget} 轮后自动干净退出")
    }

    /*
     * 跨线程交换用的容器：主线程每秒 post 一次、到 LVGL 线程上采样，
     * 采样结果写进 ArrayList；主线程只读它来打印。
     * （与 hello_cj 的点击计数同一模式：不用可变局部变量捕获，因为它无法逃逸。）
     */
    let perfLine = ArrayList<String>()
    perfLine.add("（未采样到）")
    let tick = ArrayList<Int64>()
    tick.add(0)
    let lastUs = ArrayList<Int64>()
    lastUs.add(monotonicUs())

    var rounds: Int64 = 0
    while (!backend.pollEvents()) {
        sleep(Duration.millisecond * 2)
        rounds++

        /* 每秒一次：在 LVGL 线程上采样 + 推进图表数据 */
        let nowUs = monotonicUs()
        if (nowUs - lastUs[0] >= 1000000) {
            lastUs[0] = nowUs
            let idx = tick[0]
            tick[0] = idx + 1
            rt.post {
                let p = LvDebug.perfSample()
                let s = "FPS ${p.fps}　CPU ${p.cpuPercent}%　刷新 ${p.refrTimeMs} ms　绘制 ${p.drawTimeMs} ms\n" +
                    "对象 ${p.objCount} 个　动画 ${animCountRunning()} 个　动画上下文 ${animCtxCount()} 个　" +
                    "任务队列 ${taskQueueSize()}"
                perfLine[0] = s
                stat.setText(s)

                /* 图表数据：红走锯齿、蓝走反向，便于肉眼判断有无撕裂或回退 */
                if (!staticMode) {
                    let v0 = (idx * 7) % 100
                    let v1 = 50 - ((idx * 11) % 50)
                    s0.push(Int32(v0))
                    s1.push(Int32(v1))
                }
            }
        }

        if (frameBudget > 0 && rounds >= frameBudget) {
            break
        }
    }

    /* 退出时把最后一份采样打出来 —— 这样「看着不对」可以带回一串数字来对照 */
    println("[gallery_cj] 末次采样：${perfLine[0]}")

    /* 退出顺序与 hello_cj 一致：先停主循环（交回 LVGL 线程身份），再关后端，最后反初始化 */
    rt.stop(shutdownMode: ShutdownMode.Discard)
    backend.close()
    rt.deinitLvgl()
    println("[gallery_cj] 已干净退出；渲染帧数 ${backend.presentCount()}，累计点击 ${clicks[0]} 次")
}
