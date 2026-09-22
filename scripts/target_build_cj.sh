#!/bin/bash
# ============================================================================
# target_build_cj.sh —— 在**目标机**上构建仓颉工程
# ============================================================================
#
# ★ 为什么必须 source SDK 自带的 envsetup.sh，而不是手工拼 PATH / LD_LIBRARY_PATH：
#
#   实测手工设置时，@Derive 这类**宏**的展开会失败，报的是：
#       error: macro expansion failed because of runtime initiate failed.
#       error: undeclared identifier 'Derive'
#       （后面跟着 11 个错误，全都是"Derive 未声明"）
#
#   看起来像代码问题，其实是**环境问题**：宏展开由 cjc 启动的**独立进程**完成，
#   它依赖的环境变量不止 PATH 与 LD_LIBRARY_PATH（envsetup.sh 还设了别的），
#   少一个就会以"这个宏不存在"的形式失败 —— 一个极具误导性的报错。
#   这个脚本存在的意义就是让"环境对不对"不再靠人回忆。
#
# 用法（在目标机上，需 bash）：
#   bash target_build_cj.sh [工程目录]
#   CANGJIE_HOME=/path/to/sdk bash target_build_cj.sh /path/to/project
# ============================================================================

CH="${CANGJIE_HOME:-$HOME/cangjie}"
PROJ="${1:-$HOME/lvgl4cj/examples/bench_cj}"

if [ ! -f "$CH/envsetup.sh" ]; then
    echo "找不到 $CH/envsetup.sh"
    echo "请设置 CANGJIE_HOME 指向 SDK 根目录（该目录下应有 bin/ tools/ runtime/ envsetup.sh）"
    exit 1
fi

# ★★ 低内存设备上的必需设置（本目标机 415MB，这个坑必踩）★★
#
#   仓颉运行时的**默认堆大小**是按常见开发机给的，在小内存设备上会直接起不来：
#       E RuntimeParam.heapParam.heapSize must be in range [4MB, system memory size].
#
#   而这行错误**不会**以"内存不足"的形式呈现 —— 它的下游表现是**宏展开失败**：
#       error: macro expansion failed because of runtime initiate failed.
#       error: undeclared identifier 'Derive'        （×11，全是"Derive 未声明"）
#   也就是说，一次内存配置问题看起来像**代码或语法问题**。
#   排查时如果只看最后那几行，会往完全错误的方向走。
#
#   ★ 必须**带单位**（kb/mb/gb）。裸数字会被判为非法参数，而失败方式是
#     "忽略你的设置、回落到 1GB 默认值"，于是错误照旧：
#       E Unsupported cjHeapSize parameter. The unit must be added when configuring...
#       warning: unsupported cjHeapSize for macro, using 1GB as default size
#     也就是说：**设错了比不设更难看懂** —— 你以为已经调小了，其实没有。
#
#   192mb 取本机（415MB 总内存）的一半左右：太小会让编译过程 OOM，
#   太大则运行时直接拒绝启动。
export cjHeapSize="${cjHeapSize:-192mb}"

# envsetup.sh 是 bash 脚本，且**必须**用 source 执行（它靠 BASH_SOURCE 定位自身）
# shellcheck disable=SC1090
. "$CH/envsetup.sh"

cd "$PROJ" || exit 1

echo "工程目录   : $(pwd)"
echo "CANGJIE_HOME: $CANGJIE_HOME"
echo "cjc         : $(command -v cjc)"
echo "cjpm        : $(command -v cjpm)"
echo "--- 开始构建 ---"

exec cjpm build
