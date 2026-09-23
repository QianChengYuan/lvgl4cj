#!/usr/bin/env bash
# ============================================================================
# gen_lv_conf.sh —— 由 LVGL 官方模板生成 lvgl4cj 的固定 lv_conf.h
#
# 为什么用「模板 + 定点覆盖」而不是手写最小配置：
#   lv_conf_internal.h 虽然为部分宏提供了 #ifndef 回退，但**回退值不可靠**
#   （例如 LV_USE_DRAW_SW 的回退值是 0，会导致没有软件渲染器）。
#   LVGL 官方文档推荐的方式就是拷贝 lv_conf_template.h 并修改，
#   本脚本把「修改」固化为可复现的 sed 规则，配合 LVGL 锁定的 tag 使用。
#
# 用法：  bash scripts/gen_lv_conf.sh
# 产出：  native/include/lv_conf.h   （禁止手工编辑，改本脚本）
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

LVGL_DIR="${ROOT_DIR}/third_party/lvgl"
TEMPLATE="${LVGL_DIR}/lv_conf_template.h"
OUT_DIR="${ROOT_DIR}/native/include"
OUT="${OUT_DIR}/lv_conf.h"

if [[ ! -f "${TEMPLATE}" ]]; then
    echo "[gen_lv_conf] 错误：找不到 LVGL 模板 ${TEMPLATE}" >&2
    echo "[gen_lv_conf] 请先执行：bash scripts/fetch_lvgl.sh" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

# ---------------------------------------------------------------------------
# 覆盖项说明（左＝模板原文，右＝lvgl4cj 取值）
# ---------------------------------------------------------------------------
#   LV_COLOR_DEPTH 16 -> 32          桌面 SDL2 用 XRGB8888/ARGB8888，颜色更准
#   LV_MEM_SIZE                       64KB -> 512KB，桌面有充足 RAM，避免池耗尽
#   LV_USE_LOG 0 -> 1                 日志转发到仓颉（§3.12.3 / §7.3）
#   LV_USE_ASSERT_STYLE/OBJ 0 -> 1    Debug 下提前暴露样式/对象误用
#   LV_USE_FS_POSIX 0 -> 1            §3.13.1 MVP 的 POSIX 文件系统驱动
#   LV_FS_POSIX_LETTER '\0' -> 'A'    驱动盘符，必须是非零字符
#   LV_FS_POSIX_PATH "" -> "$LVGL4CJ_FS_ROOT"  默认工作目录
#   LV_USE_SYSMON 0 -> 1              MEM/PERF monitor 的前置条件
#   LV_USE_PERF_MONITOR 0 -> 1        §7.3 性能采样（FPS/CPU/重绘耗时）
#   LV_USE_MEM_MONITOR 0 -> 1         §7.3 内存监控 + §11.3 内存指标
#   LV_USE_PROFILER 保持 0
#       ★ 与附录 C 的有据偏离：附录 C 建议开启性能计数器。实测下来它不能默认开：
#         开启后内置 profiler 会对每次布局/绘制往日志回调写一行
#         "tracing_mark_write: B|1|lv_obj_update_layout"，跑一次 hello_cj 就是成百上千行，
#         把真正的 WARN/ERROR 彻底淹没。而它本就该配一个 trace 消费者（Perfetto 之流），
#         没有消费者时纯属噪声。
#       ★ 另外这两个开关是**耦合**的：LV_USE_PROFILER=1 会把 LV_PROFILER_BEGIN/END
#         路由到 LV_PROFILER_BUILTIN_*，若同时把 BUILTIN 设为 0，那两个宏就没有定义，
#         LVGL 自己编译不过（实测报 "LV_PROFILER_BUILTIN_END undeclared"）。
#         因此不能「开 PROFILER、关 BUILTIN」，只能整体关闭后提供自定义 profiler 头。
#       ★ 影响面：§5.11 的 perf_sample 走的是 sysmon / perf monitor（LV_USE_SYSMON 等），
#         **不依赖**本开关，因此 P0 的可观测性能力不受影响。
#       需要 trace 时的正确做法：置 1 + 置 BUILTIN 1 + 接上 trace 消费者。
#   LV_USE_SDL 保持 0                 §8.1.6 决策：手写 flush/read，不用内置 SDL 驱动
# ---------------------------------------------------------------------------
LVGL4CJ_FS_ROOT="${LVGL4CJ_FS_ROOT:-.}"

sed \
    -e 's|^#if 0 /\*Set it to "1" to enable content\*/|#if 1 /*Set it to "1" to enable content - managed by lvgl4cj*/|' \
    -e 's|^#define LV_COLOR_DEPTH 16$|#define LV_COLOR_DEPTH 32|' \
    -e 's|^    #define LV_MEM_SIZE (64 \* 1024U).*$|    #define LV_MEM_SIZE (512 * 1024U)         /*[bytes] lvgl4cj: 512KB*/|' \
    -e 's|^#define LV_USE_LOG 0$|#define LV_USE_LOG 1|' \
    -e 's|^#define LV_USE_ASSERT_STYLE *0|#define LV_USE_ASSERT_STYLE          1|' \
    -e 's|^#define LV_USE_ASSERT_OBJ *0|#define LV_USE_ASSERT_OBJ            1|' \
    -e 's|^#define LV_USE_FS_POSIX 0$|#define LV_USE_FS_POSIX 1|' \
    -e "s|^\(    #define LV_FS_POSIX_LETTER \).*|\1'A'     /*lvgl4cj: 盘符固定为 A，路径形如 A:/xxx*/|" \
    -e "s|^\(    #define LV_FS_POSIX_PATH \).*|\1\"${LVGL4CJ_FS_ROOT}\"|" \
    -e 's|^#define LV_USE_SYSMON   0$|#define LV_USE_SYSMON   1|' \
    -e 's|^    #define LV_USE_PERF_MONITOR 0$|    #define LV_USE_PERF_MONITOR 1|' \
    -e 's|^    #define LV_USE_MEM_MONITOR 0$|    #define LV_USE_MEM_MONITOR 1|' \
    -e 's|^\(    #define LV_PROFILER_INCLUDE \)"lvgl/|\1"|' \
    -e 's|^\(#define LV_FONT_SIMSUN_16_CJK\) *0|\1            1|' \
    -e 's|^#define LV_USE_TINY_TTF 0$|#define LV_USE_TINY_TTF 1|' \
    -e 's|^    #define LV_TINY_TTF_FILE_SUPPORT 0$|    #define LV_TINY_TTF_FILE_SUPPORT 1|' \
    "${TEMPLATE}" > "${OUT}"

# ---------------------------------------------------------------------------
# 生成结果自检：覆盖项必须全部生效，否则说明 LVGL 模板文本变了 → 立即报错
# ---------------------------------------------------------------------------
check() {
    local pattern="$1" desc="$2"
    if ! grep -qE "${pattern}" "${OUT}"; then
        echo "[gen_lv_conf] 覆盖失败：${desc}（模式 '${pattern}' 未命中）" >&2
        echo "[gen_lv_conf] LVGL 模板结构可能已变化，请核对 lv_conf_template.h" >&2
        exit 1
    fi
}

check '^#if 1 /\*Set it to'                 '启用配置内容'
check '^#define LV_COLOR_DEPTH 32$'         'LV_COLOR_DEPTH=32'
check '^    #define LV_MEM_SIZE \(512 \* 1024U\)' 'LV_MEM_SIZE=512KB'
check '^#define LV_USE_LOG 1$'              'LV_USE_LOG=1'
check '^#define LV_USE_ASSERT_STYLE +1'     'LV_USE_ASSERT_STYLE=1'
check '^#define LV_USE_ASSERT_OBJ +1'       'LV_USE_ASSERT_OBJ=1'
check '^#define LV_USE_SYSMON +1$'          'LV_USE_SYSMON=1'
check '^    #define LV_USE_PERF_MONITOR 1$' 'LV_USE_PERF_MONITOR=1'
check '^    #define LV_USE_MEM_MONITOR 1$'  'LV_USE_MEM_MONITOR=1'
check '^#define LV_USE_PROFILER 0$'         'LV_USE_PROFILER=0（避免 trace mark 刷屏）'
# 注意行尾还有 /*...*/ 注释，因此模式不能以 $ 结尾（写成 ...+1$ 会误报未命中）
check '^#define LV_FONT_SIMSUN_16_CJK +1 '  'LV_FONT_SIMSUN_16_CJK=1（中文字形）'
# 仅靠内置 CJK 字体只能覆盖 1000 常用字，界面写中文必然缺字（实测一次缺 33 个）。
# 打开 tiny_ttf 才能直接读 TTF，中文不再受这个限制 —— 这也是本项目示例能显示中文的前提。
check '^#define LV_USE_TINY_TTF 1$'        'LV_USE_TINY_TTF=1（直接读 TTF）'
check '^    #define LV_TINY_TTF_FILE_SUPPORT 1$' 'LV_TINY_TTF_FILE_SUPPORT=1（从文件读，而非仅内存数据）'
check '^    #define LV_PROFILER_INCLUDE "src/misc/lv_profiler_builtin.h"$' 'LV_PROFILER_INCLUDE 相对 lvgl 根的路径'
check '^#define LV_USE_FS_POSIX 1$'         'LV_USE_FS_POSIX=1'
check "^    #define LV_FS_POSIX_LETTER 'A'" 'LV_FS_POSIX_LETTER=A'
check "^    #define LV_FS_POSIX_PATH \"${LVGL4CJ_FS_ROOT}\"\$" 'LV_FS_POSIX_PATH'
check '^#define LV_USE_SDL +0$'             'LV_USE_SDL 保持 0（手写 flush/read）'

echo "[gen_lv_conf] 已生成 ${OUT}"
echo "[gen_lv_conf] LVGL 版本：$(grep -E '^#define LVGL_VERSION_(MAJOR|MINOR|PATCH)' "${LVGL_DIR}/lv_version.h" | tr -s ' ' | tr '\n' ' ')"
