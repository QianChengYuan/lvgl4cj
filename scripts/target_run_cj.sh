#!/bin/bash
# ============================================================================
# target_run_cj.sh —— 在**目标机**上运行仓颉可执行文件（自动带好运行环境）
# ============================================================================
#
# 为什么需要它：
#   1. 仓颉可执行文件依赖 SDK 的运行时库（libcangjie-runtime.so 等），
#      直接运行会报 "cannot open shared object file" —— 必须先 source
#      SDK 的 envsetup.sh（它设的不止 LD_LIBRARY_PATH，见 target_build_cj.sh 的说明）。
#   2. 小内存设备必须压住运行时堆大小（cjHeapSize，且**必须带单位**），
#      否则运行时拒绝启动，表现得像"宏不存在"。
#   3. 把这些收在一个脚本里，就不必在每条 ssh 命令里重复拼环境 ——
#      漏一个变量的表现各不相同，而且都很像别的问题。
#
# 用法（在目标机上）：
#   bash target_run_cj.sh <可执行文件> [参数...]
#   bash target_run_cj.sh ~/lvgl4cj/examples/bench_cj/target/release/bin/main perf
# ============================================================================

. "${CANGJIE_HOME:-$HOME/cangjie}/envsetup.sh"

# 与 target_build_cj.sh 保持一致；单位不能省
export cjHeapSize="${cjHeapSize:-192mb}"

if [ $# -eq 0 ]; then
    echo "用法: bash target_run_cj.sh <可执行文件> [参数...]"
    exit 1
fi

exec "$@"
