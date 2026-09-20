#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_conf_const.py —— 把 probe_conf 的 JSON 输出转成仓颉常量文件

设计文档 §3.5.2 的构建链路第 4 步。

为什么需要它：
  lv_conf.h 里的 LV_* 宏是**编译期**常量，仓颉侧读不到。
  桥接层把它们导出成 lvglcj_conf_*() 查询函数 + 一个配置哈希；
  本脚本把哈希与相关常量在**构建时**固化进仓颉代码，
  于是启动时就能比对（§3.5.3）：不一致即抛 InvalidConfig，
  把一个「换机器构建导致的隐蔽花屏」前置成一条明确报错（ADR-008）。

输入来源（按优先级）：
  1. 命令行传入的 JSON 文件路径
  2. 环境变量 LVGLCJ_PROBE_CONF（probe_conf 可执行文件路径）→ 直接运行取 stdout
  3. 默认路径 <BUILD_DIR>/bin/probe_conf

产出：src/generated/conf_const.cj（package lvgl4cj.generated）
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PATH = os.path.join(ROOT, "src", "generated", "conf_const.cj")


def load_conf(argv):
    if len(argv) > 1 and argv[1]:
        with open(argv[1], "r", encoding="utf-8") as f:
            return json.load(f), argv[1]

    probe = os.environ.get("LVGLCJ_PROBE_CONF")
    if not probe:
        build_dir = os.environ.get("LVGLCJ_BUILD_DIR",
                                   os.path.join(os.path.expanduser("~"), "lvgl4cj-build"))
        probe = os.path.join(build_dir, "bin", "probe_conf")

    if not os.path.isfile(probe):
        sys.stderr.write(
            "[gen_conf_const] 找不到 probe_conf：%s\n"
            "  请先执行： bash scripts/build_native.sh --probes\n" % probe)
        sys.exit(1)

    out = subprocess.run([probe], capture_output=True, text=True, check=True).stdout
    return json.loads(out), probe


def main():
    conf, source = load_conf(sys.argv)

    offs = conf["offsets"]
    codes = conf["status_codes"]
    states = conf["handle_states"]

    lines = []
    a = lines.append

    a("/*")
    a(" * conf_const.cj —— 构建期配置常量（由 scripts/gen_conf_const.py 自动生成）")
    a(" *")
    a(" * 【禁止手工编辑】改配置请改 scripts/gen_lv_conf.sh 后重新执行：")
    a(" *     bash scripts/build_native.sh")
    a(" *")
    a(" * 生成来源 : %s" % source)
    a(" * LVGL     : %s" % conf["lvgl_version_string"])
    a(" */")
    a("package lvgl4cj.generated")
    a("")
    a("/* ---------------------------------------------------------- 版本与配置 */")
    a("public const LVGL_VERSION_STRING: String = \"%s\"" % conf["lvgl_version_string"])
    a("public const LVGL_VERSION_MAJOR: Int32 = %d" % conf["lvgl_version_major"])
    a("public const LVGL_VERSION_MINOR: Int32 = %d" % conf["lvgl_version_minor"])
    a("public const LVGL_VERSION_PATCH: Int32 = %d" % conf["lvgl_version_patch"])
    a("")
    a("/// 构建期配置哈希：启动时必须与运行库的 lvglcj_conf_hash() 一致（§3.5.3）")
    # 注意：仓颉整型字面量后缀只能是 u8/u16/u32/u64/i8/i16/i32/i64，裸 'u' 非法
    a("public const CONF_HASH: UInt32 = %du32" % conf["conf_hash"])
    a("public const CONF_COLOR_DEPTH: Int32 = %d" % conf["color_depth"])
    a("public const CONF_USE_LOG: Int32 = %d" % conf["use_log"])
    a("public const CONF_MEM_SIZE: Int32 = %d" % conf["mem_size"])
    a("public const CONF_STRIDE_ALIGN: Int32 = %d" % conf["stride_align"])
    a("public const CONF_USE_FS_POSIX: Int32 = %d" % conf["use_fs_posix"])
    a("public const CONF_DEF_REFR_PERIOD: Int32 = %d" % conf["def_refr_period"])
    a("")
    a("/* ---------------------------------------------- FFI 布局契约（§10.2）")
    a(" * 由 C 侧 lvglcj_probe_offsets() 实测得到，")
    a(" * 与仓颉侧 @C struct 的 sizeOf/alignOf 比对，防止两侧布局漂移。 */")
    a("public const C_OFFSET_X1: Int32 = %d" % offs["x1_offset"])
    a("public const C_OFFSET_Y1: Int32 = %d" % offs["y1_offset"])
    a("public const C_OFFSET_X2: Int32 = %d" % offs["x2_offset"])
    a("public const C_OFFSET_Y2: Int32 = %d" % offs["y2_offset"])
    a("public const C_SIZEOF_AREA: Int32 = %d" % offs["sizeof_area"])
    a("public const C_SIZEOF_INDEV_DATA: Int32 = %d" % offs["sizeof_indev_data"])
    a("public const C_SIZEOF_PERF: Int32 = %d" % offs["sizeof_perf"])
    a("public const C_SIZEOF_ERROR_CTX: Int32 = %d" % offs["sizeof_error_ctx"])
    a("public const C_SIZEOF_TREE_DUMP: Int32 = %d" % offs["sizeof_tree_dump"])
    a("")
    a("/* ------------------------------------- 错误码与句柄四态（附录 B / §3.1.1 冻结）")
    a(" * 两侧数值必须逐项一致；test/ffi_contract_test.cj 会做断言。 */")
    for name, value in codes.items():
        a("public const CODE_%s: Int32 = %d" % (name, value))
    # ★ 这里**不能**用裸空行做分组：cjfmt 会把「两组同类型声明之间」的空行折叠掉，
    #   于是「生成 → 提交 → cjfmt 门禁」必然失败（实测如此）。
    #   改用一行注释分组：既保住可读性，又是 cjfmt 不会删的形式。
    a("")
    a("/* 句柄四态（§3.1.1）：数值与 C 侧 handle_table.h 逐项一致 */")
    for name, value in states.items():
        a("public const HSTATE_%s: Int32 = %d" % (name, value))
    a("")

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("[gen_conf_const] 已生成 %s" % OUT_PATH)
    print("[gen_conf_const] CONF_HASH = 0x%08X  LVGL = %s"
          % (conf["conf_hash"], conf["lvgl_version_string"]))


if __name__ == "__main__":
    main()
