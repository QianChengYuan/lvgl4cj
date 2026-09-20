#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cjk_audit.py —— 回答「我到底用了哪些汉字？其中哪些显示不出来？」

================================================================================
【这个脚本解决的是什么问题】
================================================================================
内置的 CJK 字库（LV_FONT_SIMSUN_16_CJK）**有缺字**，官方描述是
"1000 of the most common CJK radicals"，实测是一份简体/繁体/日文异体混合的清单
（详见 native/probe/probe_font_coverage.c 与 docs/P0_RESULTS.md §14）。

于是「界面里写中文」这件事有一个很别扭的性质：

    ★ 用哪些汉字是可以确定的（就在我的源码里），
      但**没有人能靠阅读把它列全** —— 汉字不是字母表，
      你没法凭记忆枚举自己刚敲过的那几十个字。

    所以它必须由机器扫出来，而不能由人保证。

本脚本就做这件事：扫源码里的**字符串字面量**（跳过注释），
列出用到的全部汉字，再交给 C 探针核对哪些在字库里没有字形。

================================================================================
【为什么要分「界面文案」与「其它字面量」】
================================================================================
这是本脚本最容易做错的地方，两件事都要做对：

  1. **注释要跳过**。注释里的汉字不会渲染。把它们算进来会产生大量无关的
     "缺字"，让报告失去意义 —— 本仓库注释里就有大量中文，
     其中「显」这个字恰恰是缺的，而它在注释里出现得很多。

  2. ★ **字面量还要按用途分流**。只跳过注释是不够的：
     源码里大部分中文字面量其实是 `println` / 异常信息 / 日志，
     **它们输出到控制台，根本不经过字形渲染**。
     首版脚本没分流，于是把 272 个汉字全报成"界面用字"，
     其中一半"缺失"—— 全是诊断文案，看的人只会把它当噪声。
     一份喊狼来了的报告等于没有报告。

     所以按**调用点**分流：
       · 界面文案 —— `setText(...)` 等 UI 调用，**必须全覆盖**，缺字即失败
       · 其它字面量 —— 控制台/异常信息，缺字只提示，不判失败

  「界面文案」的覆盖面靠 UI_CALLS 白名单，它**是启发式的**：
  漏掉某个新 API 会让该处文案退化成"只提示"。宁可漏判成提示，
  也不要把控制台文案误判成必须覆盖 —— 后者会让人习惯性忽略整个报告。

================================================================================
【用法】
================================================================================
    python3 scripts/cjk_audit.py                 # 默认扫 src 与 examples
    python3 scripts/cjk_audit.py src examples    # 指定路径
    python3 scripts/cjk_audit.py --no-check      # 只列清单，不核对字库
    python3 scripts/cjk_audit.py --all           # 连"其它字面量"一起当界面文案判失败

退出码：0 = 界面文案全部可渲染；5 = 界面文案有字渲染不出来；2 = 用法/环境错误

★ 缺字不是"脚本报错"，而是**产品缺陷**：那些字在界面上就是一块空白，
  而 LVGL 只会打一行日志，不报错、不抛异常。所以本脚本对缺字返回非零码，
  可以（也应该）直接进 CI。
"""

import os
import re
import subprocess
import sys

# ============================================================ 界面文案的判定

# ★ 这些调用点的字符串参数会**进入渲染路径**（画到屏幕上），因此必须全覆盖。
#   白名单是启发式的，理由见文件头「为什么要分界面文案与其它字面量」。
#
# ★ 维护规则：**新增任何「接受文案并让它显示出来」的 API 时，必须加到这里**，
#   否则该处文案会退化成"只提示"，缺字就漏过去了。
#   本仓库当前（P0）的完整入口只有下面两个 —— 已用
#   `grep -nE "public (static )?func [a-zA-Z]+\(.*String" src/widgets.cj` 核对过：
#     LvLabel.setText(t)      widgets.cj:74
#     LvButton.withLabel(text) widgets.cj:111   （内部转发 setText）
UI_CALLS = {
    "setText",          # LvLabel.setText
    "withLabel",        # LvButton.withLabel —— 与 setText 同为渲染入口
    "setTitle",         # 预留：后续控件
    "setPlaceholder",   # 预留
    "setOneLine",       # 预留
    "setOptions",       # 预留
}

# ★ 本脚本**看不到**的情况（残余缺口，必须知道）：
#   文案若先赋给变量/常量再传进去，例如
#       let s = "点我"
#       label.setText(s)
#   则 `"点我"` 的前邻调用不是 setText，会被归到"其它字面量"里 ——
#   也就是**不会被硬判定**。
#   词法级扫描无法跟踪这种数据流，这不是能靠加白名单解决的。
#   对这类文案，正确的核对手段是**运行时**的 L1 API：
#       LvFont.builtin(LvBuiltinFont.Cjk16).missingGlyphs(s)
#   （见 src/font_test.cj 中 testExampleRenderedTextIsFullyCovered 的做法）。
#   两者互补：脚本管"直接写出来的文案"，API 管"算出来的文案"。

# ============================================================ 字符分类

# 汉字（含扩展 A 与兼容区）。只统计这些 —— 全角标点与假名单独报，
# 因为「界面里的中文会不会缺字」问的是汉字，混在一起会淹没重点。
HAN_RANGES = (
    (0x3400, 0x4DBF),   # CJK 扩展 A
    (0x4E00, 0x9FFF),   # CJK 统一表意文字
    (0xF900, 0xFAFF),   # CJK 兼容表意文字
)


def is_han(cp: int) -> bool:
    return any(lo <= cp <= hi for lo, hi in HAN_RANGES)


def scan_dirs(paths):
    """收集 .cj 文件（跳过 generated/ 与测试数据目录）"""
    files = []
    for p in paths:
        if os.path.isfile(p):
            if p.endswith(".cj"):
                files.append(p)
            continue
        for root, dirs, names in os.walk(p):
            # generated/ 是脚本产物，不是人写的文案，不纳入审计
            dirs[:] = [d for d in dirs if d not in ("generated", "target", ".git")]
            for n in names:
                if n.endswith(".cj"):
                    files.append(os.path.join(root, n))
    return sorted(files)


# ============================================================ 字面量提取

def call_name_before(src, pos):
    """
    猜这个字面量属于哪个函数调用的参数 —— 即它前面紧邻的那个 `ident(`。

    只用于把「界面文案」与「控制台文案」分开；判错只会影响**报告分类**，
    不影响字符统计本身。取不到就返回空串（会被归入"其它"）。
    """
    seg = src[max(0, pos - 200):pos]
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*$", seg)
    return m.group(1) if m else ""


def collect_literals(src):
    """
    取出源码中所有字符串字面量，返回 [(内容, 所属调用名)]，**跳过注释**。

    ★ 为什么自己写状态机而不用正则：
      仓颉里单引号与双引号都能表示字符串，三引号是多行字符串，
      而注释符可能出现在字符串内部（"// 这不是注释"），
      字符串也可能出现在注释内部（/* "这不是字面量" */）。
      正则无法区分这几种情况，一次顺序扫描可以。

    这是**词法级**的近似，不是完整的仓颉词法分析器；它刻意只处理
    与本任务相关的四类结构（行注释 / 块注释 / 三引号串 / 单双引号串）。
    要审计的是"文案里有哪些字"，不需要也不可能靠它做语法分析。
    """
    out = []
    i = 0
    n = len(src)

    while i < n:
        # ---- 行注释
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue

        # ---- 块注释
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue

        # ---- 三引号多行字符串
        if src.startswith('"""', i) or src.startswith("'''", i):
            q = src[i:i + 3]
            j = src.find(q, i + 3)
            if j < 0:
                break
            out.append((src[i + 3:j], call_name_before(src, i)))
            i = j + 3
            continue

        ch = src[i]
        # ---- 单/双引号字符串（含 'x' 形式的 Rune/字节字面量，会被 ASCII 过滤掉）
        if ch in "\"'":
            start = i
            j = i + 1
            buf = []
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == ch:
                    break
                buf.append(src[j])
                j += 1
            out.append(("".join(buf), call_name_before(src, start)))
            i = j + 1
            continue

        i += 1

    return out


# ============================================================ 主流程

def _print_chars(chars, per_line=24):
    """按码位排序，每行 per_line 个打印"""
    if not chars:
        print("  （无）")
        return
    row = []
    for ch in sorted(chars, key=ord):
        row.append(ch)
        if len(row) == per_line:
            print("  " + "".join(row))
            row = []
    if row:
        print("  " + "".join(row))


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    do_check = "--no-check" not in argv
    treat_all_as_ui = "--all" in argv
    paths = args or ["src", "examples"]

    files = scan_dirs(paths)
    if not files:
        print("未找到任何 .cj 文件（路径：%s）" % " ".join(paths))
        return 2

    # 分成两组统计。★ 分组是本脚本的核心判断，理由见文件头。
    ui_han, ui_other = {}, {}      # 界面文案（进渲染路径）
    con_han, con_other = {}, {}    # 其它字面量（控制台 / 异常信息）

    for f in files:
        try:
            with open(f, "r", encoding="utf-8") as fh:
                src = fh.read()
        except OSError as e:
            print("读取失败：%s（%s）" % (f, e))
            return 2

        for lit, call in collect_literals(src):
            is_ui = treat_all_as_ui or (call in UI_CALLS)
            hs, os_ = (ui_han, ui_other) if is_ui else (con_han, con_other)
            for ch in lit:
                cp = ord(ch)
                if cp < 0x80:
                    continue        # ASCII 一定全覆盖（实测 95/95）
                tgt = hs if is_han(cp) else os_
                tgt[ch] = tgt.get(ch, 0) + 1

    print("=== 扫描范围 ===")
    print("  目录：%s" % " ".join(paths))
    print("  文件：%d 个 .cj（已跳过注释，只统计字符串字面量）" % len(files))

    print()
    print("=== ★ 界面文案用字（进渲染路径，必须全覆盖）：%d 个汉字 ===" % len(ui_han))
    _print_chars(ui_han)
    if ui_other:
        print("  其它非 ASCII：%s" % "".join(sorted(ui_other, key=ord)))

    # 只列出界面文案里没有的字，避免同一批字在两处重复刷屏
    extra_han = {c: n for c, n in con_han.items() if c not in ui_han}
    print()
    print("=== 其它字面量用字（控制台/异常信息，**不渲染**）："
          "%d 个汉字，其中 %d 个界面文案未用到 ===" % (len(con_han), len(extra_han)))
    _print_chars(extra_han)
    print("  ↑ 这些字只出现在 println / 异常信息里，缺字形不影响显示。")

    if not do_check:
        return 0

    # 硬判定集合：只有界面文案。--all 时把所有字面量都算进来。
    hard = set(ui_han) | set(ui_other)
    if treat_all_as_ui:
        hard |= set(con_han) | set(con_other)

    if not hard:
        print()
        print("没有需要核对的界面文案。")
        return 0

    # ---- 交给 C 探针核对字形覆盖
    print()
    print("=== ★ 字形覆盖核对（LV_FONT_SIMSUN_16_CJK）===")

    probe = _find_probe()
    if probe is None:
        print("  未找到 probe_font_coverage 可执行文件，跳过核对。")
        print("  构建方式：")
        print("    cmake -S native -B build-probe -DLVGLCJ_BUILD_PROBES=ON \\")
        print("          -DLVGLCJ_BUILD_SDL2=OFF && cmake --build build-probe \\")
        print("          --target probe_font_coverage")
        return 2

    codes = " ".join("%X" % ord(c) for c in sorted(hard, key=ord))
    try:
        proc = subprocess.run([probe, "codes"], input=codes,
                              capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        print("  调用探针失败：%s" % e)
        return 2

    print("  待核对字符数：%d" % len(hard))
    print(proc.stdout.rstrip())
    if proc.stderr.strip():
        print(proc.stderr.rstrip())

    if proc.returncode == 5:
        print()
        print("  ★ 上述字符在界面里**不会显示** —— 画面上少一块，而 LVGL 不报错、")
        print("    不抛异常，所以它会一路静默到验收。处理方式（代价从低到高）：")
        print("      1. 改文案，避开这些字（只有几个字时最划算）")
        print("      2. 换覆盖完整的自定义字体：LvFont.load(...)（正式产品推荐）")
        return 5

    if proc.returncode == 0:
        print("  ✓ 界面文案全部可渲染。")
        return 0

    return 2


def _find_probe():
    """在常见的探针构建目录里找一个可用的 probe_font_coverage"""
    cands = []
    for base in ("build-probe", "build", "../build-probe"):
        cands.append(os.path.join(base, "bin", "probe_font_coverage"))
    home = os.path.expanduser("~")
    cands.append(os.path.join(home, "lvgl4cj-build-probe", "bin", "probe_font_coverage"))
    for c in cands:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return None


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
