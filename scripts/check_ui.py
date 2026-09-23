#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_ui.py —— 给「界面到底哪里不对」出一份可复现的体检报告。

为什么要有它
------------
肉眼描述界面异常很吃力（"看着不对，但说不上哪里不对"），而口头转述一次就多一层误差。
这个脚本把"看起来不对"换成几个**可判定的量**，每条都打印原始数值，结论可以复核：

  · 客户端区域在哪   —— 把窗口标题栏、边框、截图工具的描边排除在外
  · 卡片带（内容分段）有几段、各自在哪个 y 范围
  · 白色块（键盘/输入框）有多宽、是否超出限定值
  · ★ 有没有「同一段内容出现在两个 y」—— 也就是重影 / 残留旧像素

依赖
----
只用 Python 3 标准库（自己解 PNG / PPM，含调色板 PNG 与全部 5 种行滤波）。
以下都是可选，缺了照样能跑：
  · Pillow             —— 有则用它解码（更快），没有走内置解码器
  · xwd                —— 现场抓窗口（X11）
  · ImageMagick import —— 现场抓窗口的备选路径
  · python-xlib        —— 现场抓窗口的第三条路径

用法
----
  python3 scripts/check_ui.py                      # 抓当前窗口（WSLg / X11）
  python3 scripts/check_ui.py shot.png             # 分析已有截图（推荐）
  python3 scripts/check_ui.py shot.png --map       # 附带 ASCII 颜色图
  python3 scripts/check_ui.py a.png b.png          # 一次分析多张并逐张报告
  python3 scripts/check_ui.py --repeat 3           # 连抓 3 张，看是否稳定

判据取值都来自 examples/gallery_cj 实际用的颜色；分类用「通道关系」而不是精确相等，
这样能抗缩放、抗抗锯齿，也抗截图工具的轻微色偏。
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import zlib

# ---------------------------------------------------------------------------
# 调色板分类
# ---------------------------------------------------------------------------

BLACK, SCREEN, CARD, WHITE, LIGHT, BLUE, GREEN, RED, YELLOW, OTHER = range(10)

NAME = {BLACK: "纯黑", SCREEN: "屏幕底", CARD: "卡片底", WHITE: "白", LIGHT: "浅",
        BLUE: "蓝", GREEN: "绿", RED: "红", YELLOW: "黄", OTHER: "其他"}
CHAR = {BLACK: "X", SCREEN: ".", CARD: "#", WHITE: "W", LIGHT: "-", BLUE: "B",
        GREEN: "G", RED: "R", YELLOW: "Y", OTHER: "?"}

# 「客户端区域」的标记色：本应用独有的深色调。
# 刻意不含白/浅：窗口标题栏是中性灰，会被归到「浅」，于是不会被算进客户端区域。
MARKER = (BLACK, SCREEN, CARD)

# 「内容像素」：文本、控件、图形会用到的亮色。
# 重影检测要靠它把"卡片内部的纯色空行"筛掉 —— 那种空行签名天然相同，不是重影。
CONTENT = (WHITE, LIGHT, BLUE, GREEN, RED, YELLOW)

# 「背景像素」：卡片之间/之下的底色，以及**从未被写过的纹理像素**。
BACKGROUNDISH = (BLACK, SCREEN)


def classify(r, g, b):
    v = (r + g + b) // 3
    # ★ 纯黑要单独成一类：本示例的任何颜色都不是纯黑（屏幕底 #10141C 偏蓝、
    #   卡片底 #2A3242），所以出现大片纯黑只有一个解释 ——
    #   那是**纹理里从未被写过的像素**（新建纹理的内容未定义，通常是 0）。
    #   早期版本把纯黑并进"屏幕底"，于是脚本对这种现象完全失明。
    if v <= 6:
        return BLACK
    if v < 40:
        return SCREEN
    # 卡片底是**偏蓝的**深灰（0x2A3242 → b-r=24、b-g=16）。
    # 要求"偏蓝"是为了不把窗口标题栏那种中性灰（r≈g≈b）误判成卡片。
    if 25 <= v <= 95 and (b - r) >= 14 and (b - g) >= 8:
        return CARD
    if v >= 215:
        return WHITE
    if v >= 165:
        return LIGHT
    if b > r + 30 and b > g + 10:
        return BLUE
    if b > r + 20 and b > g + 20:
        return BLUE
    if g > r + 25 and g > b + 25:
        return GREEN
    if r > g + 40 and r > b + 40:
        return RED
    if r > 140 and g > 110 and b < 120:
        return YELLOW
    return OTHER


# ---------------------------------------------------------------------------
# 图像载入（纯标准库）
# ---------------------------------------------------------------------------

class Img(object):
    __slots__ = ("w", "h", "rgb")

    def __init__(self, w, h, rgb):
        self.w = w
        self.h = h
        self.rgb = rgb  # bytearray，RGB8，逐行

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]


def _unfilter(raw, w, h, nch):
    """解 PNG 的 5 种行滤波（0..4）。返回解滤波后的字节串。"""
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:      # Sub
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:    # Average
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:    # Paeth
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise ValueError("未知的 PNG 行滤波类型 %d（第 %d 行）" % (f, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out, stride


def _to_rgb(buf, w, h, nch, palette=None):
    rgb = bytearray(w * h * 3)
    if nch == 3:
        rgb[:] = buf
    elif nch == 4:
        for i in range(w * h):
            rgb[i * 3:i * 3 + 3] = buf[i * 4:i * 4 + 3]
    elif nch == 1:
        if palette is not None:
            for i in range(w * h):
                rgb[i * 3:i * 3 + 3] = palette[buf[i]]
        else:
            for i in range(w * h):
                v = buf[i]
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v
    elif nch == 2:
        for i in range(w * h):
            v = buf[i * 2]
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v
    else:
        raise ValueError("不支持的通道数 %d" % nch)
    return Img(w, h, rgb)


def load_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG 文件")
    pos = 8
    idat = bytearray()
    plte = None
    w = h = depth = ctype = None
    while pos + 8 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, _c, _f, inter = struct.unpack(">IIBBBBB", body)
            if inter:
                raise ValueError("暂不支持隔行（interlace）PNG，请另存为非隔行")
        elif typ == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if depth != 8:
        raise ValueError("只支持 8 位/通道的 PNG（本次 depth=%s）" % depth)
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    buf, _stride = _unfilter(zlib.decompress(bytes(idat)), w, h, nch)
    return _to_rgb(buf, w, h, nch, plte if ctype == 3 else None)


def load_ppm(path_or_bytes):
    if isinstance(path_or_bytes, bytes):
        data = path_or_bytes
    else:
        data = open(path_or_bytes, "rb").read()
    # 跳过注释行，读三个数
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    magic, w, h, maxv = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
    if magic != b"P6":
        raise ValueError("只支持二进制 PPM（P6），本次是 %r" % magic)
    i += 1
    px = data[i:i + w * h * 3]
    if maxv != 255:
        px = bytes((v * 255) // maxv for v in px)
    return Img(w, h, bytearray(px))


def load_any(path):
    low = path.lower()
    if low.endswith(".png"):
        return load_png(path)
    if low.endswith((".ppm", ".pnm")):
        return load_ppm(path)
    # 后缀不认识就按内容猜
    with open(path, "rb") as f:
        head = f.read(8)
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return load_png(path)
    if head[:2] == b"P6":
        return load_ppm(path)
    raise ValueError("认不出格式（只支持 PNG / PPM）：%s" % path)


# ---------------------------------------------------------------------------
# 现场抓窗口（可选路径，缺工具就明确报出来）
# ---------------------------------------------------------------------------

def _xwininfo_window(title="lvgl4cj"):
    try:
        out = subprocess.run(["xwininfo", "-root", "-tree"],
                             capture_output=True, timeout=8).stdout.decode("utf-8", "replace")
    except Exception:
        return None
    for line in out.splitlines():
        if title in line:
            m = re.search(r"(0x[0-9a-fA-F]+)", line)
            if m:
                return m.group(1)
    return None


def capture_window(title="lvgl4cj"):
    """返回 (Img, 说明) 或 (None, 失败原因)。"""
    reasons = []
    wid = _xwininfo_window(title)
    if wid is None:
        return None, "找不到标题含 %r 的窗口（应用没在跑？或 xwininfo 不可用）" % title

    # 1) ImageMagick import（最省事）
    try:
        r = subprocess.run(["import", "-window", wid, "ppm:-"],
                           capture_output=True, timeout=20)
        if r.stdout[:2] == b"P6":
            return load_ppm(r.stdout), "import -window %s" % wid
        reasons.append("import 无输出")
    except Exception as e:
        reasons.append("import 不可用(%s)" % e)

    # 2) xwd + 自解
    try:
        r = subprocess.run(["xwd", "-id", wid, "-silent"],
                           capture_output=True, timeout=20)
        if r.stdout[:4] == b"\x00\x00\x00\x07":
            return _load_xwd(r.stdout), "xwd -id %s" % wid
        reasons.append("xwd 无输出")
    except Exception as e:
        reasons.append("xwd 不可用(%s)" % e)

    # 3) python-xlib
    try:
        from Xlib import display as _d, X as _X
        d = _d.Display()
        root = d.screen().root
        for w in root.query_tree().children:
            try:
                nm = w.get_wm_name() or ""
            except Exception:
                nm = ""
            if title in nm:
                g = w.get_geometry()
                im = w.get_image(0, 0, g.width, g.height, _X.ZPixmap, 0xffffffff)
                data = im.data
                if isinstance(data, str):
                    data = data.encode("latin-1")
                rgb = bytearray(g.width * g.height * 3)
                for i in range(g.width * g.height):
                    p = data[i * 4:i * 4 + 4]
                    rgb[i * 3] = p[2]
                    rgb[i * 3 + 1] = p[1]
                    rgb[i * 3 + 2] = p[0]
                return Img(g.width, g.height, rgb), "python-xlib"
    except Exception as e:
        reasons.append("python-xlib 不可用(%s)" % e)

    return None, "；".join(reasons)


def _load_xwd(data):
    hdr = struct.unpack(">25I", data[:100])
    (_hs, _ver, _fmt, _depth, w, h, _xoff, byte_order, _bunit, _bborder, _bpad,
     bpp, bpl, _vclass, rmask, gmask, bmask, _brgb, cmap_n, ncolors) = hdr[:20]
    pos = 100 + ncolors * 12
    px = data[pos:pos + bpl * h]
    rgb = bytearray(w * h * 3)
    nbytes = bpp // 8
    rshift, gshift, bshift = _mask_shift(rmask), _mask_shift(gmask), _mask_shift(bmask)
    for y in range(h):
        row = px[y * bpl:(y + 1) * bpl]
        base = y * w * 3
        for x in range(w):
            raw = int.from_bytes(row[x * nbytes:(x + 1) * nbytes],
                                 "big" if byte_order == 1 else "little")
            rgb[base + x * 3] = (raw & rmask) >> rshift
            rgb[base + x * 3 + 1] = (raw & gmask) >> gshift
            rgb[base + x * 3 + 2] = (raw & bmask) >> bshift
    return Img(w, h, rgb)


def _mask_shift(mask):
    if mask == 0:
        return 0
    s = 0
    while not (mask >> s) & 1:
        s += 1
    return s


# ---------------------------------------------------------------------------
# 分析
# ---------------------------------------------------------------------------

def label_rows(img):
    """逐像素分类，返回 list[bytearray]（每行一个标签数组）。"""
    rows = []
    rgb = img.rgb
    for y in range(img.h):
        base = y * img.w * 3
        row = bytearray(img.w)
        for x in range(img.w):
            i = base + x * 3
            row[x] = classify(rgb[i], rgb[i + 1], rgb[i + 2])
        rows.append(row)
    return rows


def find_client_box(labels, w, h):
    """用「本应用独有的深色调」的行/列密度找客户端区域。"""
    def dense(seq, n, need):
        return sum(1 for v in seq if v in MARKER) >= need

    top = bottom = None
    need = max(4, w // 4)
    for y in range(h):
        if dense(labels[y], w, need):
            top = y
            break
    for y in range(h - 1, -1, -1):
        if dense(labels[y], w, need):
            bottom = y
            break
    if top is None:
        return None
    needc = max(4, (bottom - top + 1) // 4)
    left = right = None
    for x in range(w):
        col = [labels[y][x] for y in range(top, bottom + 1)]
        if dense(col, h, needc):
            left = x
            break
    for x in range(w - 1, -1, -1):
        col = [labels[y][x] for y in range(top, bottom + 1)]
        if dense(col, h, needc):
            right = x
            break
    if left is None or right is None or right <= left or bottom <= top:
        return None
    return (left, top, right - left + 1, bottom - top + 1)


def card_bands(labels, box, min_h=6, gap=2):
    """卡片带 = 连续若干「非屏幕底像素占多数」的行。"""
    x0, y0, bw, bh = box
    flags = []
    for y in range(y0, y0 + bh):
        row = labels[y][x0:x0 + bw]
        non_bg = sum(1 for v in row if v not in BACKGROUNDISH)
        flags.append(non_bg * 100 // max(1, bw) >= 50)
    bands = []
    y = 0
    while y < bh:
        if flags[y]:
            ys = y
            blank = 0
            ye = y
            while y < bh:
                if flags[y]:
                    ye = y
                    blank = 0
                else:
                    blank += 1
                    if blank > gap:
                        break
                y += 1
            if ye - ys + 1 >= min_h:
                bands.append((ys + y0, ye + y0))
        else:
            y += 1
    return bands


def white_bands(labels, box, min_w=40):
    """
    白色（含浅色）**最长连续段**：用于看键盘/输入框有多宽。

    ★ 一定要量"最长连续段"，而不是"该行所有白色像素的跨度"：
      后者会把**文字分散在卡片上**误判成一条几百像素宽的白块
      （第一版就是这么写的，结果第 1 张正常截图也被报"过宽 769 px"）。
      键盘的判据是"一整行连成一片"，所以只有连续段才说明问题。
    """
    x0, y0, bw, bh = box
    rows = []
    for y in range(y0, y0 + bh):
        row = labels[y][x0:x0 + bw]
        best = 0
        run = 0
        best_start = 0
        for i, v in enumerate(row):
            if v in (WHITE, LIGHT):
                run += 1
                if run > best:
                    best = run
                    best_start = i - run + 1
            else:
                run = 0
        if best >= min_w:
            rows.append((y, best, best_start + x0, best_start + best - 1 + x0))
    out = []
    i = 0
    while i < len(rows):
        j = i
        while j + 1 < len(rows) and rows[j + 1][0] == rows[j][0] + 1:
            j += 1
        block = rows[i:j + 1]
        if len(block) >= 4:
            out.append((block[0][0], block[-1][0],
                        min(r[2] for r in block), max(r[3] for r in block),
                        max(r[1] for r in block)))
        i = j + 1
    return out


def find_repeats(labels, box, win=8, gap=10, cols=96, min_run=20):
    """
    重影检测：把每行降采样成 cols 个标签做签名，找「同一串签名在相隔较远的 y 再次出现」。
    这正是"旧像素没被清掉 / 同一内容画了两次"的形状。

    ★ 必须过滤掉「没有内容的窗口」：卡片内部的**纯色空行**签名天然相同，
      不过滤的话会刷出几十条 y=70..75 与 y=80..85 之类的噪音，把真正的重影埋掉。
      判据：窗口里至少要有一定比例的**内容像素**（白/浅/蓝/绿/红/黄）。
    返回 (命中列表, 被判为无内容而跳过的窗口数)。
    """
    x0, y0, bw, bh = box
    sigs = []
    step = max(1, bw // cols)
    ncol = min(cols, bw // step)
    for y in range(y0, y0 + bh):
        row = labels[y]
        sigs.append(bytes(row[x0 + i * step] for i in range(ncol)))

    need_content = max(2, (ncol * win) // 25)  # 约 4% 的内容像素

    def content_sig(seg):
        blob = b"".join(seg)
        content = sum(1 for v in blob if v in CONTENT)
        return blob, content

    seen = {}
    hits = []
    skipped = 0
    for y in range(0, len(sigs) - win + 1):
        seg = sigs[y:y + win]
        key, content = content_sig(seg)
        if content < need_content or len(set(key)) < 2:
            skipped += 1
            continue
        if key in seen:
            prev = seen[key]
            if y - prev >= gap:
                # ★ 只认**成块**的重复：往后逐行延伸，统计连续相同的行数。
                #   单独几行相同是巧合（样式统一的卡片，边缘本来就长得像），
                #   而"旧像素没被清掉"会留下一整块内容，所以要求连续足够长。
                run = 0
                while (y + run < len(sigs) and prev + run < y
                       and sigs[prev + run] == sigs[y + run]):
                    run += 1
                total = run + win
                if total >= min_run:
                    hits.append((prev + y0, prev + run + win - 1 + y0,
                                 y + y0, y + run + win - 1 + y0, content, total))
        else:
            seen[key] = y
    # 去掉互相重叠的报告，按重复块长度排序（越长越可能是真重影）
    out = []
    for h in sorted(hits, key=lambda t: -t[5]):
        if not any(abs(h[0] - o[0]) < win and abs(h[2] - o[2]) < win for o in out):
            out.append(h)
    return out, skipped


def render_map(labels, box, cols=100, rows_n=46):
    x0, y0, bw, bh = box
    lines = []
    for r in range(rows_n):
        y = y0 + (bh * r) // rows_n
        s = []
        for c in range(cols):
            x = x0 + (bw * c) // cols
            s.append(CHAR[labels[y][x]])
        lines.append("".join(s))
    return lines


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------

def report(img, src_name):
    labels = label_rows(img)
    print("=" * 78)
    print("图像 %s  %dx%d" % (src_name, img.w, img.h))
    box = find_client_box(labels, img.w, img.h)
    if box is None:
        print("  ✗ 找不到客户端区域：这张图里没有本应用的深色调，可能不是它的截图。")
        return
    x0, y0, bw, bh = box
    print("  客户端区域：x=%d y=%d 宽=%d 高=%d" % (x0, y0, bw, bh))

    # ★ 纯黑带：本示例不用纯黑，所以大片纯黑 = 纹理里从未被写过的像素。
    #   这是最值得单独报的一项：它把"没画过"和"画成深色"区分开。
    blacks = []
    for y in range(y0, y0 + bh):
        row = labels[y][x0:x0 + bw]
        if sum(1 for v in row if v == BLACK) * 100 // max(1, bw) >= 80:
            if blacks and y == blacks[-1][1] + 1:
                blacks[-1] = (blacks[-1][0], y)
            else:
                blacks.append((y, y))
    blacks = [b for b in blacks if b[1] - b[0] >= 3]
    if blacks:
        area = sum(b[1] - b[0] + 1 for b in blacks) * bw
        print("  ★★ 纯黑带 %d 段，合计 %d 行（占客户端高度 %d%%）："
              % (len(blacks), sum(b[1] - b[0] + 1 for b in blacks),
                 sum(b[1] - b[0] + 1 for b in blacks) * 100 // max(1, bh)))
        for (a, b) in blacks[:8]:
            print("      y=%d..%d  高 %d  面积 %d px" % (a, b, b - a + 1, (b - a + 1) * bw))
        print("      → 纯黑不是本示例用的颜色（屏幕底 #10141C、卡片底 #2A3242）。")
        print("        这些像素**从未被写过**：要么 LVGL 没渲染这块，要么贴到纹理上的")
        print("        是缓冲区里没画过的那一段。它和\"旧像素残留\"是两回事，要分开查。")
    else:
        print("  纯黑带：无 ✓")

    bands = card_bands(labels, box)
    print("  卡片带 %d 段：" % len(bands))
    for i, (a, b) in enumerate(bands):
        print("      #%d  y=%d..%d  高 %d" % (i, a, b, b - a + 1))
    tallest = max((b - a + 1) for a, b in bands) if bands else 0
    if tallest > 280:
        print("      ★ 最高的带 %d 行 —— 卡片之间本该有空隙（示例里单张卡片最高 252 行）；"
              "这么高说明空隙被内容填住了：内容越界或旧像素残留" % tallest)

    wb = white_bands(labels, box)
    print("  白色块 %d 段（最长连续白段占客户端宽度 >45%% 视为过宽，疑似键盘没限定宽度）：" % len(wb))
    for (a, b, wx, wx2, mw) in sorted(wb, key=lambda t: -t[4])[:6]:
        pct = mw * 100 // bw
        flag = "  ← ★ 过宽" if pct > 45 else ""
        print("      y=%d..%d  最长连续 %d px（占客户端 %d%%）  位于 x=%d..%d%s"
              % (a, b, mw, pct, wx, wx2, flag))

    # 卡片节距：相邻带起点的中位数间隔 —— 用来判断"错位"是不是页面结构造成的
    pitch = 0
    if len(bands) >= 2:
        gaps = sorted(bands[i + 1][0] - bands[i][0] for i in range(len(bands) - 1))
        pitch = gaps[len(gaps) // 2]

    reps, skipped = find_repeats(labels, box)
    if reps:
        # 与卡片节距成倍数关系的，多半只是"同类卡片内部布局相同"，不是重影。
        # 容差取 20 px：带的起点受卡片间距（8 px）与合并影响，节距会有十几像素的漂移。
        def like_pitch(off):
            return bool(pitch) and abs(off - round(off / float(pitch)) * pitch) <= 20

        byp = [h for h in reps if like_pitch(h[2] - h[0])]
        print("  ★ 疑似重影 %d 处（连续 >=20 行的含内容签名在相隔 >=10 px 处再次出现；"
              "另有 %d 个片段按噪音跳过）；其中与卡片节距(%d px)成倍数的 %d 处，"
              "其余 %d 处需要看：" % (len(reps), skipped, pitch, len(byp), len(reps) - len(byp)))
        for (a1, b1, a2, b2, content, total) in reps[:6]:
            off = a2 - a1
            note = "  ← 与卡片节距成倍数（可能只是同类卡片内部布局相同）" if like_pitch(off) else ""
            print("      y=%d..%d  与  y=%d..%d  完全相同（重复块 %d 行，错位 %d px，内容像素 %d）%s"
                  % (a1, b1, a2, b2, total, off, content, note))
        print("      → 重复块长、且错位与卡片节距无关时，即是「同一块内容画了两次」；")
        print("        那说明旧像素没被清掉，而不是内容位置在抖。")
    else:
        print("  重影检测：未发现重复的内容块 ✓（跳过的无内容片段 %d 个）" % skipped)
    return box, labels


def main():
    ap = argparse.ArgumentParser(description="lvgl4cj 界面体检（可传截图，也可现场抓窗口）")
    ap.add_argument("images", nargs="*", help="要分析的截图（PNG/PPM）；不给则现场抓窗口")
    ap.add_argument("--title", default="lvgl4cj", help="窗口标题关键字（默认 lvgl4cj）")
    ap.add_argument("--repeat", type=int, default=1, help="现场连抓几次（默认 1）")
    ap.add_argument("--delay", type=float, default=1.5, help="每次抓图前的等待秒数")
    ap.add_argument("--map", action="store_true", help="打印 ASCII 颜色图")
    ap.add_argument("--map-only", action="store_true", help="只打印颜色图")
    args = ap.parse_args()

    todo = []
    if args.images:
        for p in args.images:
            if not os.path.exists(p):
                print("✗ 文件不存在：%s" % p)
                return 2
            todo.append(p)
        loader = lambda p: load_any(p)
    else:
        import time
        time.sleep(max(0.0, args.delay))
        loader = None

    for path in todo:
        if args.map_only:
            img = loader(path)
            box = find_client_box(label_rows(img), img.w, img.h)
            if box:
                print("\n".join(render_map(label_rows(img), box)))
            continue
        img = loader(path)
        r = report(img, path)
        if args.map and r:
            print("  ASCII 颜色图（. 屏幕底  # 卡片底  W 白  - 浅  B 蓝  G 绿  R 红  Y 黄）：")
            for line in render_map(r[1], r[0]):
                print("      " + line)

    if not args.images:
        import time
        got = 0
        for k in range(max(1, args.repeat)):
            if k:
                time.sleep(max(0.0, args.delay))
            img, how = capture_window(args.title)
            if img is None:
                print("✗ 抓图失败：%s" % how)
                print("  可退一步：先用截图工具存成 PNG，再把文件路径传给本脚本。")
                return 3
            print("# 抓图方式：%s" % how)
            r = report(img, "现场第 %d 张" % (k + 1))
            if args.map and r:
                for line in render_map(r[1], r[0]):
                    print("      " + line)
            got += 1
        print("\n共分析 %d 张。" % got)
    return 0


if __name__ == "__main__":
    sys.exit(main())
