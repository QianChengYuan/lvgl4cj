#!/usr/bin/env bash
# ============================================================================
# build_native.sh —— 一键构建 L2 C 桥接层
#
# 步骤（对应设计文档 §9.1 构建链路）：
#   1. fetch_lvgl.sh        拉取/校验锁定版本 LVGL
#   2. gen_lv_conf.sh       由模板生成 native/include/lv_conf.h
#   3. cmake configure      LVGL + 桥接层 + 可选后端/探针/测试
#   4. cmake build
#   5. 复制 liblvgl.a / liblvgl4cj_bridge.a → 工程根 libs/（供 cjpm link-option 使用）
#
# 为什么构建目录默认放 WSL 原生文件系统（$HOME/lvgl4cj-build）：
#   /mnt/c 是 9p/DrvFs 挂载，小文件 I/O 与文件监视都明显慢于原生 FS，
#   编译 LVGL 的数千个源文件时差异显著。源码保留在工作区便于查看，
#   构建产物放原生 FS，两边兼得。
#
# 用法：
#   bash scripts/build_native.sh                 # Debug 构建
#   bash scripts/build_native.sh --release       # Release
#   bash scripts/build_native.sh --probes        # 附加构建 V1–V7 探针
#   bash scripts/build_native.sh --tests         # 附加构建 C 侧单测
#   bash scripts/build_native.sh --asan          # 启用 ASan
#   bash scripts/build_native.sh --clean         # 清理后重建
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ★ 默认目录**故意与 gate.sh 不同**（gate 用 $HOME/lvgl4cj-build）。
#
#   原先两者共用同一个目录，而选项默认值相反：
#       build_native.sh : -DLVGLCJ_BUILD_TESTS=OFF （还有一个 -DLVGLCJ_BUILD_PROBES=OFF）
#       gate.sh         : -DLVGLCJ_BUILD_TESTS=ON
#   CMake 是"最后配置者获胜"，于是**谁最后跑谁说了算**：
#   提交前跑一次 build_native.sh 就会把该目录里的测试目标移除，
#   下次 cmake --build 报成功却什么都不测，二进制还是旧的。
#
#   实测踩到两次，且两次都很容易被误判成"我改的测试没生效"——
#   因为症状是"检查数没变"，而不是任何报错。
#   本质问题是：**一次性配置的目录状态被两个不同用途的脚本反复翻转**，
#   而翻转过程完全静默。分开目录是最小且彻底的修法。
#
#   注意产物不受影响：本脚本最终把库同步到 <工程根>/libs/，
#   而仓颉侧读的是 libs/，不是这个构建目录。
BUILD_DIR="${LVGLCJ_BUILD_DIR:-$HOME/lvgl4cj-build-native}"
BUILD_TYPE="Debug"
BUILD_PROBES=OFF
BUILD_TESTS=OFF
ASAN=OFF
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release"; shift ;;
        --debug)   BUILD_TYPE="Debug"; shift ;;
        --probes)  BUILD_PROBES=ON; shift ;;
        --tests)   BUILD_TESTS=ON; shift ;;
        --asan)    ASAN=ON; shift ;;
        --clean)   CLEAN=1; shift ;;
        -j)        shift 2 ;;
        *)         echo "[build_native] 未知参数：$1" >&2; exit 2 ;;
    esac
done

JOBS="$(nproc 2>/dev/null || echo 4)"

echo "[build_native] 工程根  : ${ROOT_DIR}"
echo "[build_native] 构建目录: ${BUILD_DIR}"
echo "[build_native] 构建类型: ${BUILD_TYPE}   并行度: ${JOBS}"

# ------------------------------------------------------------------ 1. LVGL
bash "${SCRIPT_DIR}/fetch_lvgl.sh"

# ------------------------------------------------------------- 2. lv_conf.h
bash "${SCRIPT_DIR}/gen_lv_conf.sh"

# ------------------------------------------------------------ 3. configure
if [[ "${CLEAN}" == "1" ]]; then
    echo "[build_native] 清理 ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${ROOT_DIR}/native" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DLVGLCJ_BUILD_PROBES="${BUILD_PROBES}" \
    -DLVGLCJ_BUILD_TESTS="${BUILD_TESTS}" \
    -DLVGLCJ_ASAN="${ASAN}"

# ---------------------------------------------------------------- 4. build
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# ------------------------------------------------- 5. 产物同步到 libs/
LIBS_DIR="${ROOT_DIR}/libs"
mkdir -p "${LIBS_DIR}"

shopt -s nullglob
copied=0
for f in "${BUILD_DIR}/lib/"*.a "${BUILD_DIR}/lib/"*.so; do
    cp -f "$f" "${LIBS_DIR}/"
    copied=$((copied + 1))
done
shopt -u nullglob

if [[ "${copied}" == "0" ]]; then
    echo "[build_native] 错误：${BUILD_DIR}/lib 下没有产物" >&2
    exit 1
fi

echo "[build_native] 已同步 ${copied} 个库到 ${LIBS_DIR}"
ls -la "${LIBS_DIR}"

# --------------------------------------------------- 6. C 侧单测（可选）
if [[ "${BUILD_TESTS}" == "ON" ]]; then
    echo "[build_native] 运行 CTest"
    (cd "${BUILD_DIR}" && ctest --output-on-failure)
fi

# ------------------------------------------- 6.5 生成仓颉侧配置常量（§3.5.2）
# 放在 cjpm 之前：src/generated/conf_const.cj 是仓颉构建的输入，
# 必须在 cjpm build 之前存在且与当前 C 库一致。
if [[ -f "${BUILD_DIR}/bin/probe_conf" ]]; then
    if command -v python3 >/dev/null 2>&1; then
        LVGLCJ_BUILD_DIR="${BUILD_DIR}" python3 "${SCRIPT_DIR}/gen_conf_const.py"
    else
        echo "[build_native] 警告：未找到 python3，跳过 conf_const.cj 生成" >&2
        echo "[build_native]       兜底方案见设计文档 §9.1（纯 CMake configure_file）" >&2
    fi
else
    echo "[build_native] 提示：未构建 probe_conf，跳过 conf_const.cj 生成"
    echo "[build_native]       如需生成请加 --probes"
fi

# ----------------------------------------------------- 7. 探针（可选）
if [[ "${BUILD_PROBES}" == "ON" ]]; then
    echo "[build_native] 探针可执行文件位于 ${BUILD_DIR}/bin："
    ls -1 "${BUILD_DIR}/bin" 2>/dev/null || true
    echo "[build_native] 运行全部探针： bash scripts/run_probe.sh"
fi

# ================= 8. ABI 符号存在性自检 =================
# 实测教训（本项目真实踩到）：C 静态库**只在被引用时才拉入目标文件**，
#   因此「头文件声明了、但没有实现（或实现名字写错）」可以一路编译+链接通过，
#   直到某个调用方真的引用它，才在链接期以 undefined reference 爆炸。
#   这类问题越晚暴露越贵，所以在构建末尾把「声明集合 − 定义集合」算出来。
sed -e 's|/\*.*\*/||g' "${ROOT_DIR}/native/include/lvglcj_bridge.h" \
    | grep -vE '^[[:space:]]*(\*|#)' \
    | grep -oE '\blvglcj_[a-z0-9_]+' \
    | grep -vE '_t$' \
    | sort -u > /tmp/lvglcj_declared.txt || true

nm -g --defined-only "${LIBS_DIR}"/*.a 2>/dev/null \
    | awk '{print $NF}' | grep -E '^lvglcj_' | sort -u > /tmp/lvglcj_defined.txt || true

missing="$(comm -23 /tmp/lvglcj_declared.txt /tmp/lvglcj_defined.txt || true)"
if [[ -n "${missing}" ]]; then
    count="$(printf '%s\n' "${missing}" | wc -l)"
    echo "[build_native] ABI 符号自检：${count} 个声明尚未实现（随各子系统落地递减）"
    printf '%s\n' "${missing}" | head -40 | sed 's/^/      /'
    if [[ "${LVGLCJ_STRICT_SYMBOLS:-0}" == "1" ]]; then
        echo "[build_native] LVGLCJ_STRICT_SYMBOLS=1 → 视为构建失败" >&2
        exit 1
    fi
else
    echo "[build_native] ABI 符号自检：头文件声明与产物定义完全一致"
fi

echo "[build_native] 完成。下一步：cd examples/hello_cj && cjpm run"
