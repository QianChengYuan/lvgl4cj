/*
 * gallery_cj —— 全控件示例（设计文档 §7.1 的 P1 清单，19 个控件）
 *
 * 用途：
 *   ① **看**：19 个控件一次摆在窗口里，肉眼确认渲染；
 *   ② **量**：顶部一行统计每秒刷新（ASCII，保证一定可见），退出时打印末次采样 ——
 *      于是「看着不对」能变成一串可比对的数字；
 *   ③ **自证字形**：启动时按 `LvFont.missingGlyphs` 逐条检查示例文本，把缺字直接列出来。
 *
 * ★★ 为什么需要 ③ —— 这是本次踩到的最重要一件事：
 *   本库内置的 CJK 字体只有 `lv_font_simsun_16_cjk`（lv_conf 里 LV_FONT_SIMSUN_16_CJK=1），
 *   它的覆盖范围是 **1000 个常用字**。超出的字会被 LVGL **静默丢弃**（每个字一行 WARN 日志），
 *   而多数界面里文字就是主要内容 —— 于是现象是「窗口一片黑」，而根因在字体覆盖面上。
 *   从画面倒推这件事很费时，所以本示例启动时直接把缺字打出来。
 *   要显示任意中文，需要启用 TTF 加载（LV_USE_TINY_TTF）或使用转换后的 .bin 字体 ——
 *   两者都属于构建配置层面的选择，不在示例内做决定。
 *
 * ★ 另一个"看得见"的取舍：卡片底色刻意比屏幕底色亮一档并加边框。
 *   原先两者都偏暗，字一缺就等于整屏全黑，连布局对不对都判断不了。
 *
 * ★ 初始化顺序与 hello_cj 一致，且**不是风格问题**（ADR-013）：
 *   initLvgl() 把当前线程登记为 LVGL 线程；startThreaded() 之后主线程不再是 LVGL 线程，
 *   再调 lv_* 会被线程断言拒绝。所以：**所有建 UI 的代码必须在 startThreaded() 之前**，
 *   之后要用 LVGL 只能通过 rt.post { }（在 LVGL 线程上执行）。
 *
 * ★ 链式调用的一条硬规则：setPos/setSize/align 返回的是 **LvObject**（基类），
 *   所以「先 setPos 再 setColor/setText」会丢类型、编译不过。
 *   本文件统一写成 **控件自有方法在前、对象通用方法在后**：
 *       x.setRange(0, 100).setValue(30).setPos(PAD, 44).setSize(220, 18)
 *
 * ★ 控件**内部**的字符串（下拉选项、滚轮选项、表格单元格、输入框内容）一律用 ASCII：
 *   那些文字由 LVGL 内部标签绘制，字体是否从控件继承并不由本层保证；
 *   用 ASCII 就不必赌这条继承规则 —— 中文只出现在**我们自己挂字体样式**的标签上。
 */
package gallery_cj

import std.collection.*
import std.time.*
import lvgl4cj.*

const SMOKE_ROUNDS: Int64 = 900

/* 卡片排版：屏幕 800 宽，两侧各留 16 */
const CARD_W: Int32 = 768
const PAD: Int32 = 16

/* 配色：卡片比屏幕亮一档 + 明确边框，缺字时也能看清布局。
 * 0xRRGGBB 与 bgColor/textColor 的入参一致（与 canvas/led 的颜色约定相同）。 */
const SCREEN_BG: UInt32 = 0x10141C
const CARD_BG: UInt32 = 0x2A3242
const CARD_BORDER: UInt32 = 0x505C70
const TITLE_FG: UInt32 = 0x8AB4FF
const BODY_FG: UInt32 = 0xD8DEE8

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
        .bgColor(CARD_BG)
        .bgOpa(255)
        .radius(10)
        .borderWidth(2)
        .borderColor(CARD_BORDER)
        .padAll(0)
        .applyTo(c)

    let t = LvLabel.create(c)
    t.setText(title).setPos(PAD, 6)
    LvStyle.create().textColor(TITLE_FG).textFont(cjk).applyTo(t)
    return c
}

/* 小工具：给对象上正文样式（形参用 LvObject：勾选框这类非 LvLabel 的控件也要用） */
func bodyStyle(l: LvObject, cjk: LvFont, twoLine!: Bool = false): Unit {
    let s = LvStyle.create().textColor(BODY_FG).textFont(cjk)
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

    /* ================= 字形自检（放在建 UI 之前，最早把问题说清楚） ================= */
    let samples = ArrayList<String>()
    samples.add("运行统计")
    samples.add("文本：长文本自动换行")
    samples.add("开关 / 勾选")
    samples.add("指示与忙碌")
    samples.add("数值显示")
    samples.add("选择：下拉与滚轮")
    samples.add("输入：文本框与键盘")
    samples.add("自绘：画布与折线")
    samples.add("曲线：两条 series")
    samples.add("表格与图片")
    var missingTotal = 0
    for (i in 0..samples.size) {
        let m = cjk.missingGlyphs(samples[i])
        if (m.size > 0) {
            missingTotal += m.size
            println("[gallery_cj][字形] 缺 ${m.size} 个字：${samples[i]}")
        }
    }
    if (missingTotal > 0) {
        println("[gallery_cj][字形] 合计缺 ${missingTotal} 个字 —— 界面上的中文会**静默消失**（每个字一行 WARN）")
        println("            原因：内置 CJK 字体 lv_font_simsun_16_cjk 只覆盖 1000 个常用字")
        println("            要显示任意中文：启用 LV_USE_TINY_TTF（直接读 TTF）或换用转换后的 .bin 字体")
    } else {
        println("[gallery_cj][字形] 示例文本覆盖完整")
    }

    LvStyle.create().bgColor(SCREEN_BG).bgOpa(255).applyTo(scr)

    /* 根容器比屏幕高 → 内容超出后可按住鼠标拖动滚动（LVGL 基础对象的默认行为） */
    let root = LvObject.create(scr)
    root.setPos(0, 0).setSize(800, 1420)
    LvStyle.create().bgOpa(0).padAll(0).applyTo(root)

    var y: Int32 = 8

    /* ================= 0) 统计：全 ASCII，保证在任何字体下都看得见 ================= */
    let statCard = makeCard(root, y, 96, "0) stats / 运行统计", cjk)
    let stat = LvLabel.create(statCard)
    stat.setText("press 'r' ... waiting for first sample").setPos(PAD, 28).setSize(CARD_W - 2 * PAD, 62)
    LvStyle.create().textColor(rgb(0xE8, 0xEE, 0xFF)).textFont(cjk).textLineSpace(4).applyTo(stat)
    y += 104

    /* ================= 1) LvLabel ================= */
    let c1 = makeCard(root, y, 96, "1) LvLabel - 文本", cjk)
    let lab = LvLabel.create(c1)
    lab.setText("标签：长文本会自动换行；改字号或字体后行距应保持稳定。")
    lab.setPos(PAD, 32).setSize(CARD_W - 2 * PAD, 54)
    bodyStyle(lab, cjk, twoLine: true)
    y += 104

    /* ================= 2) LvButton ================= */
    let c2 = makeCard(root, y, 88, "2) LvButton - 点击事件", cjk)
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
    /* 按钮文字用 ASCII + 一个在 1000 字内的"次"，保证一定看得见 */
    let btnLabel = btn.withLabel("clicked 0")
    LvStyle.create().textFont(cjk).applyTo(btnLabel)
    let clicks = ArrayList<Int32>()
    clicks.add(0)
    btn.on(LvEventCode.Clicked) {
        _ =>
            clicks[0] = clicks[0] + 1
            btnLabel.setText("clicked ${clicks[0]}")
    }
    y += 96

    /* ================= 3) LvSwitch / LvCheckbox ================= */
    let c3 = makeCard(root, y, 88, "3) LvSwitch / LvCheckbox - 开关与勾选", cjk)
    let sw = LvSwitch.create(c3)
    sw.setChecked(true).setPos(PAD, 40)

    let cb = LvCheckbox.create(c3)
    cb.setText("check me") /* 控件内部字符串：ASCII */
    cb.setChecked(true).setPos(PAD + 180, 36)
    bodyStyle(cb, cjk)
    y += 96

    /* ================= 4) LvLed / LvSpinner ================= */
    let c4 = makeCard(root, y, 88, "4) LvLed / LvSpinner - 指示与忙碌", cjk)
    let led = LvLed.create(c4)
    led.setColor(0x00E676)
    led.setBrightness(255)
    led.setOn(true)
    led.setPos(PAD, 40)

    let spin = LvSpinner.create(c4)
    spin.setPos(PAD + 80, 34).setSize(48, 48)

    let ledLab = LvLabel.create(c4)
    ledLab.setText("LED 亮度由动画驱动；右侧转圈是 Spinner").setPos(PAD + 150, 42)
    bodyStyle(ledLab, cjk)
    y += 96

    /* ================= 5) LvBar / LvSlider / LvArc（都由动画驱动） ================= */
    let c5 = makeCard(root, y, 116, "5) LvBar / LvSlider / LvArc - 数值（均由 LvAnim 驱动）", cjk)
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

    /* ================= 6) LvDropdown / LvRoller ================= */
    let c6 = makeCard(root, y, 116, "6) LvDropdown / LvRoller - 选择", cjk)
    let dd = LvDropdown.create(c6)
    dd.setOptions("One\nTwo\nThree") /* 控件内部字符串：ASCII */
    dd.setSelected(0)
    dd.setPos(PAD, 40).setSize(200, 40)

    let roller = LvRoller.create(c6)
    roller.setOptions("A\nB\nC\nD")
    roller.setSelected(1)
    roller.setVisibleRowCount(3)
    roller.setPos(PAD + 260, 38).setSize(150, 70)

    let selLab = LvLabel.create(c6)
    selLab.setText("下拉与滚轮：点击后可滚动选择").setPos(PAD + 440, 58)
    bodyStyle(selLab, cjk)
    y += 124

    /* ================= 7) LvTextarea + LvKeyboard ================= */
    let c7 = makeCard(root, y, 252, "7) LvTextarea + LvKeyboard - 输入", cjk)
    let ta = LvTextarea.create(c7)
    ta.setOneLine(true)
    ta.setPlaceholderText("type here") /* ASCII */
    ta.setText("hello 123")
    ta.setPos(PAD, 34).setSize(360, 46)

    let kb = LvKeyboard.create(c7)
    kb.setMode(LVGLCJ_KEYBOARD_MODE_NUMBER)
    kb.setPopovers(true)
    kb.setTextarea(ta)
    kb.setPos(PAD, 90)

    let kbLab = LvLabel.create(c7)
    kbLab.setText("数字键盘：点按键会写进上面的输入框\n删输入框前必须先 clearTextarea（见契约）")
    kbLab.setPos(PAD + 330, 50).setSize(CARD_W - (PAD + 330) - PAD, 90)
    bodyStyle(kbLab, cjk, twoLine: true)
    y += 260

    /* ================= 8) LvCanvas / LvLine ================= */
    let c8 = makeCard(root, y, 146, "8) LvCanvas / LvLine - 自绘", cjk)
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

    /* ================= 9) LvChart ================= */
    let c9 = makeCard(root, y, 152, "9) LvChart - 曲线（两条 series，持续推点）", cjk)
    let chart = LvChart.create(c9)
    chart.setType(LVGLCJ_CHART_TYPE_LINE)
    chart.setPointCount(24)
    chart.setRange(LVGLCJ_CHART_AXIS_PRIMARY_Y, 0, 100)
    chart.setRange(LVGLCJ_CHART_AXIS_SECONDARY_Y, 0, 50)
    chart.setPos(PAD, 34).setSize(440, 108)
    let s0 = chart.addSeries(0xEF5350)
    let s1 = chart.addSeries(0x42A5F5, axis: LVGLCJ_CHART_AXIS_SECONDARY_Y)

    let chartLab = LvLabel.create(c9)
    chartLab.setText("红：主轴 0..100\n蓝：副轴 0..50").setPos(PAD + 470, 56)
    bodyStyle(chartLab, cjk, twoLine: true)
    y += 160

    /* ================= 10) LvTable / LvImage ================= */
    let c10 = makeCard(root, y, 152, "10) LvTable / LvImage - 表格与图片", cjk)
    let tb = LvTable.create(c10)
    tb.setColumnCount(3)
    tb.setRowCount(3)
    tb.setCellValue(0, 0, "col-1") /* 单元格也是控件内部字符串：ASCII */
    tb.setCellValue(0, 1, "col-2")
    tb.setCellValue(0, 2, "col-3")
    tb.setCellValue(1, 0, "1")
    tb.setCellValue(1, 1, "2")
    tb.setCellValue(1, 2, "3")
    tb.setCellValue(2, 0, "merged")
    tb.mergeRight(2, 0)
    tb.setTextCrop(2, 0, true)
    tb.setPos(PAD, 34).setSize(320, 108)

    /*
     * 图片：这里**故意不设 src**。
     * 上一版指向一个不存在的路径，结果 LVGL 打出一整屏 ERROR，
     * 把真正的问题（字形缺失）淹掉了 —— 诊断价值为负。
     * 要看真实图片需要两步：启用解码器（LV_USE_LODEPNG / LIBPNG，当前都是 0）
     * + 提供存在的文件，两者都不属于本示例的范围，所以这里留空并说明。
     */
    let img = LvImage.create(c10)
    img.setScale(256)
    img.setPos(PAD + 350, 40).setSize(80, 80)

    let imgLab = LvLabel.create(c10)
    imgLab
        .setText("图片：此处留空。\n当前解码器未启用（LODEPNG/LIBPNG=0），设了 src 也显示不了")
        .setPos(PAD + 445, 48)
        .setSize(300, 90)
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

    println("[gallery_cj] 窗口已启动：19 个控件分 11 张卡片；按住鼠标拖动可滚动")
    if (staticMode) {
        println("[gallery_cj] --static：动画已全部关闭（用于对照「静止时是否也抖」）")
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
    perfLine.add("(no sample yet)")
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
                /* 统计行**全 ASCII**：它是判断"有没有在动"的唯一依据，不能依赖中文能否显示 */
                let s = "fps ${p.fps}  cpu ${p.cpuPercent}%  refresh ${p.refrTimeMs} ms  draw ${p.drawTimeMs} ms\n" +
                    "objects ${p.objCount}  anims ${animCountRunning()}  animCtx ${animCtxCount()}  taskQ ${taskQueueSize()}"
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
    println("[gallery_cj] last sample: ${perfLine[0]}")

    /* 退出顺序与 hello_cj 一致：先停主循环（交回 LVGL 线程身份），再关后端，最后反初始化 */
    rt.stop(shutdownMode: ShutdownMode.Discard)
    backend.close()
    rt.deinitLvgl()
    println("[gallery_cj] 已干净退出；渲染帧数 ${backend.presentCount()}，累计点击 ${clicks[0]} 次")
}
