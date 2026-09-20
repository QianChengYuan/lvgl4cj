#!/usr/bin/env bash
# ============================================================================
# env.sh —— 仓颉工具链环境变量（所有脚本 source 本文件）
#
# 为什么需要它：
#   Ubuntu 默认的 ~/.bashrc 开头有 `case $- in *i*) ;; *) return;; esac`，
#   非交互 shell（如 IDE 里执行的 `wsl -e bash -c "..."`）source 它会立刻返回，
#   因此 cjc/cjpm 不在 PATH 上。这里显式导出，保证脚本在任何 shell 模式下都能跑。
#
# 用法： source "$(dirname "$0")/env.sh"
# ============================================================================

export CANGJIE_HOME="${CANGJIE_HOME:-$HOME/cangjie/cangjie_1.1.0/cangjie}"
export PATH="$CANGJIE_HOME/bin:$CANGJIE_HOME/tools/bin:$PATH"

_RUNTIME_LIB="$CANGJIE_HOME/runtime/lib/linux_x86_64_cjnative"
if [[ -d "$_RUNTIME_LIB" ]]; then
    export LD_LIBRARY_PATH="$_RUNTIME_LIB:${LD_LIBRARY_PATH:-}"
fi

# 扩展标准库 stdx（P0 未使用，保留以便后续）
_STDX_DIR="$HOME/cangjie/cangjie_stdx/linux_x86_64_cjnative/dynamic/stdx"
if [[ -d "$_STDX_DIR" ]]; then
    export CANGJIE_STDX_PATH="$_STDX_DIR"
fi

# 本工程 C 库目录（探针/示例运行时需要）
_LVGLCJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LVGLCJ_ROOT="$_LVGLCJ_ROOT"
export LD_LIBRARY_PATH="$_LVGLCJ_ROOT/libs:${LD_LIBRARY_PATH:-}"

# 构建目录默认放 WSL 原生 FS（/mnt/c 是 9p 挂载，小文件 I/O 慢）
export LVGLCJ_BUILD_DIR="${LVGLCJ_BUILD_DIR:-$HOME/lvgl4cj-build}"
