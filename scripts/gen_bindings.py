#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_bindings.py —— 从 C 侧 ABI 生成仓颉 L1 的绑定包装

================================================================================
【为什么需要它：现状是三处手写同一件事】
================================================================================
一个样式属性目前要在**四个地方**保持一致：
    native/include/lvglcj_bridge.h   ABI 声明
    native/src/style.c               C 实现（ST_INT 宏，一行一个）
    src/ffi_bridge.cj                foreign 声明
    src/style.cj                     L1 包装
四处都要改、且没有任何机制保证它们同步。实测后果：**C 侧有 53 个样式 setter，
L1 只包了 26 个** —— 差的 27 个里包括 padLeft/padRight/padTop/padBottom、
shadowOffsetX（而 shadowOffsetY 有）、lineWidth、borderOpa 等。
也就是说「某个属性能不能用」取决于当初谁顺手写了哪个，而不是取决于设计。

生成器的输入选 **C 侧 ABI**（而不是 LVGL 头文件，也不是另维护一张表）：
  · LVGL 的 lv_style_set_* 在 v9 里是宏生成的，直接解析头部收益低；
  · 而 C 侧 ABI 已经是「按子系统系统化整理过」的那一层，
    并且它自己就是 L1 的唯一依赖 —— 从它生成，L1 与 ABI 的一致性自动成立。

================================================================================
【用法】
================================================================================
    python3 scripts/gen_bindings.py --check     # 只比对，不改文件（CI 用）
    python3 scripts/gen_bindings.py --emit      # 打印生成的包装代码

--check 退出码：0 = 一致；1 = 有漂移（缺失/写法不符）；2 = 用法或解析错误

★ --check 的判据是「一致性」，不是「必须存在」：
  生成的集合是 C 侧 ABI 的**函数**，所以它天然会随 ABI 增长。
  它抓的是「ABI 有的、L1 没有」这种单向漂移 —— 那正是上面积累出 27 个缺口的原因。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "native/include/lvglcj_bridge.h")
STYLE_CJ = os.path.join(ROOT, "src/style.cj")

# ---------------------------------------------------------------- 类型映射
#
# C 参数类型 → (仓颉参数名, 仓颉类型)
#
# 参数名刻意按语义区分：颜色用 c、数值用 v，与现有手写代码保持一致 ——
# 生成器若产出与手写风格不同的名字，会造成一次无意义的全量重写。
# 遇到未登记的 C 类型时**报错退出**，而不是猜一个类型：
# 猜错会生成能编译但语义错误的代码，那比缺一个包装危险得多。
PARAM_RULES = {
    "uint32_t": ("c", "UInt32"),   # 颜色（0xRRGGBB）
    "int32_t": ("v", "Int32"),
    "int64_t": ("v", "Int64"),
    "bool": ("v", "Bool"),
}


# ---------------------------------------------------------------- 特例（必须显式跳过）
#
# 有些 ABI setter 的 L1 包装**不应该**按通用规则生成：它们的参数是
# 「句柄 / 类型安全的包装对象」而不是裸整数，手写版本提供了更强的类型保证。
#
# 生成器必须显式跳过它们，否则会产生一个与手写版本**同名的重载** ——
# 那能编译通过，但语义重复、且把更弱的那份（裸 Int64）暴露给了使用者。
# 这类"能编译的错误"正是生成器最该防的，所以特例用白名单写死在这里。
SPECIAL_CASE = {
    "text_font": "手写：参数是 LvFont（而非 Int64 句柄），把类型安全留在 L1",
}


def camel(snake):
    p = snake.split("_")
    return p[0] + "".join(w.capitalize() for w in p[1:])


def parse_abi_setters():
    """
    解析桥接头里的 style setter 声明，返回 [(snake, ctype, 参数名)]。
    形如： int32_t lvglcj_style_set_bg_color(int64_t s, uint32_t color);
    """
    src = open(BRIDGE, encoding="utf-8").read()
    out = []
    for m in re.finditer(
            r"^int32_t\s+(lvglcj_style_set_[a-z_0-9]+)\s*\(\s*int64_t\s+(\w+)\s*,\s*"
            r"([a-z0-9_]+)\s+(\w+)\s*\)\s*;", src, re.M):
        cname, _h, ctype, _p = m.group(1), m.group(2), m.group(3), m.group(4)
        if ctype not in PARAM_RULES:
            print("★ 未登记的 C 参数类型：%s（函数 %s）" % (ctype, cname), file=sys.stderr)
            print("  请在 PARAM_RULES 里显式登记，不要猜类型。", file=sys.stderr)
            sys.exit(2)
        out.append((cname[len("lvglcj_style_set_"):], ctype))
    return out


def emit_wrapper(snake, ctype):
    """生成一个 L1 setter。形态与现有手写代码逐字符一致（见 --check 的比对）。"""
    name = camel(snake)
    pname, cjtype = PARAM_RULES[ctype]
    return (
        "    public func %s(%s: %s): LvStyle {\n"
        "        check(unsafe { lvglcj_style_set_%s(handle, %s) }, \"%s\")\n"
        "        return this\n"
        "    }\n" % (name, pname, cjtype, snake, pname, name)
    )


def parse_existing_l1():
    """
    解析 src/style.cj 里已有的 L1 setter，返回 {camel名: (参数名, 仓颉类型, ABI名)}。
    先把连续空白压成单空格，避免多行写法导致匹配失败。
    """
    src = open(STYLE_CJ, encoding="utf-8").read()
    flat = re.sub(r"\s+", " ", src)
    found = {}
    # 实参允许是裸名字或点号访问（如 `f.handle`）—— 手写的特例 textFont 正是后者。
    # 首版只写了 `\w+`，于是把 textFont 误判成"手写版本也不在"。
    for m in re.finditer(
            r"public func (\w+)\((\w+): (\w+)\): LvStyle \{ "
            r"check\(unsafe \{ (lvglcj_style_set_\w+)\(handle, [\w.]+\) \}, \"(\w+)\"\) "
            r"return this \}", flat):
        name, pname, cjtype, abi, label = m.groups()
        found[name] = (pname, cjtype, abi, label)
    return found


def main(argv):
    mode_check = "--check" in argv
    mode_emit = "--emit" in argv
    mode_emit_missing = "--emit-missing" in argv

    abi = parse_abi_setters()
    if not abi:
        print("★ 未解析到任何 style setter —— 桥接头的写法可能变了。", file=sys.stderr)
        print("  这是「解析失败」而不是「没有属性」，必须修解析规则，不能当作空集。",
              file=sys.stderr)
        return 2

    existing = parse_existing_l1()

    if mode_emit:
        for snake, ctype in abi:
            if snake in SPECIAL_CASE:
                continue
            sys.stdout.write(emit_wrapper(snake, ctype))
        return 0

    if mode_emit_missing:
        """只打印 L1 还缺的那些包装 —— 供人工/CI 增量补齐，不触碰已有代码。"""
        for snake, ctype in abi:
            if snake in SPECIAL_CASE:
                continue
            if camel(snake) not in existing:
                sys.stdout.write(emit_wrapper(snake, ctype))
        return 0

    # ---- 比对
    abi_by_camel = {camel(s): (s, t) for s, t in abi}
    missing = []
    mismatched = []
    special = []
    for name, (snake, ctype) in sorted(abi_by_camel.items()):
        if snake in SPECIAL_CASE:
            # 特例：不算缺失，但要确认"手写版本确实在"，否则就是真的漏了
            special.append((name, snake, name in existing))
            continue
        if name not in existing:
            missing.append((name, snake, ctype))
            continue
        pname, cjtype, abi_name, label = existing[name]
        want_p, want_t = PARAM_RULES[ctype]
        if pname != want_p or cjtype != want_t or abi_name != "lvglcj_style_set_" + snake or label != name:
            mismatched.append((name, "手写=(%s:%s, %s, \"%s\") 期望=(%s:%s, %s, \"%s\")"
                               % (pname, cjtype, abi_name, label, want_p, want_t,
                                  "lvglcj_style_set_" + snake, name)))

    extra = sorted(set(existing) - set(abi_by_camel) - {camel(s) for s in SPECIAL_CASE})

    print("=== 样式 setter：ABI ↔ L1 一致性 ===")
    print("  ABI 提供：%d（其中 %d 个为特例）" % (len(abi_by_camel), len(special)))
    print("  L1 已有 ：%d（其中 %d 个不来自 ABI 的 style setter）"
          % (len(existing), len(extra)))

    rc = 0
    if special:
        print("\n  特例（生成器显式跳过，由手写提供）：")
        for name, snake, present in special:
            mark = "手写版本在" if present else "★ 手写版本也不在 —— 真的漏了"
            print("      %-24s %s（%s）" % (name, mark, SPECIAL_CASE[snake]))
            if not present:
                rc = 1

    if missing:
        rc = 1
        print("\n  ★ ABI 有而 L1 **缺失**：%d 个" % len(missing))
        for name, snake, ctype in missing:
            print("      %-24s （ABI: lvglcj_style_set_%s，类型 %s）" % (name, snake, ctype))
        print("    → 补齐：python3 scripts/gen_bindings.py --emit-missing")

    if mismatched:
        rc = 1
        print("\n  ★ 写法与生成规则不符：%d 个" % len(mismatched))
        for name, detail in mismatched:
            print("      %-24s %s" % (name, detail))

    if extra:
        # 不是错误：可能是 applyTo/removeFrom/close 这类非 setter，或手写的特殊包装
        print("\n  （提示）以下 L1 方法不在 ABI 的 style setter 里，可能是非 setter 或特例：")
        for name in extra:
            print("      %s" % name)

    if rc == 0:
        print("\n  [ PASS ] ABI 的每个 style setter 都能在 L1 找到一致的包装")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
