#!/usr/bin/env bash
# ============================================================================
# perf.sh —— §11.2 性能指标采集
#
# 采集三类数据：
#   1. `bench_cj perf` 输出的全部指标（FFI 开销 / 事件延迟 / 输入延迟 / FPS / 内存 / 进程内启动时间）
#   2. **冷启动**：用 /usr/bin/time 从进程之外量「启动到首帧并退出」的整段墙钟，
#      这一段包含进程加载与动态库解析 —— 进程内测不到
#   3. §11.2 门槛对照表（自动判定）
#
# ★ 关于门槛判定的态度：
#   本脚本会打印对照结论，但**在 WSL2/CI 上不把门槛当作失败条件**（退出码仍为 0）。
#   原因：宿主负载、虚拟化与软件渲染都会让数字失真，据此判红会得到一个
#   「时好时坏」的门禁，最后被人为关掉 —— 那比没有更糟。
#   正式判定请在目标平台上跑本脚本，并把输出存入 docs/benchmarks/。
#   回归护栏由 src/perf_test.cj 承担（宽松上界，纳入 cjpm test）。
#
# 用法：
#   bash scripts/perf.sh                      # 采集并打印
#   bash scripts/perf.sh --out /tmp/perf.txt  # 同时存档
#   bash scripts/perf.sh --strict             # 门槛不达标时以非零码退出（目标平台用）
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

OUT_FILE=""
STRICT=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT_FILE="$2"; shift 2 ;;
        --strict) STRICT=1; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "[perf] 未知参数：$1" >&2; exit 2 ;;
    esac
done

if [[ -z "${CANGJIE_HOME:-}" ]]; then
    for d in "$HOME"/cangjie/*/cangjie; do
        [[ -x "${d}/bin/cjc" ]] && CANGJIE_HOME="${d}" && break
    done
fi
export LD_LIBRARY_PATH="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative:${ROOT_DIR}/libs:${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=dummy
export PATH="${CANGJIE_HOME:-}/bin:${PATH}"

BIN="${ROOT_DIR}/examples/bench_cj/target/release/bin/main"
if [[ ! -x "${BIN}" ]]; then
    echo "[perf] 找不到 ${BIN}" >&2
    echo "[perf] 请先：cd examples/bench_cj && cjpm build" >&2
    exit 2
fi

CAPTURE="$(mktemp)"
trap 'rm -f "${CAPTURE}"' EXIT

echo "[perf] 采集指标（约 10 秒）…"
"${BIN}" perf > "${CAPTURE}" 2>&1
PERF_RC=$?

# ------------------------------------------------------- 冷启动（进程外计时）
COLD_START=""
if command -v /usr/bin/time > /dev/null 2>&1; then
    COLD_START=$( { /usr/bin/time -f "%e" "${BIN}" startup > /dev/null; } 2>&1 | tail -1 )
fi

# ------------------------------------------------------- 取指标值
get_metric() {
    sed -n "s/^\[perf\] $1 = \([^（(]*\).*/\1/p" "${CAPTURE}" | head -1 | tr -d ' '
}

FFI_RAW=$(get_metric "ffi.raw.ns_per_call")
EV_AVG=$(get_metric "event.latency.avg_ns")
INPUT_MS=$(get_metric "input.latency.ms")
FPS_PART=$(get_metric "fps.partial")
FPS_FULL=$(get_metric "fps.fullscreen")
STARTUP_MS=$(get_metric "startup.init_to_first_frame_ms")
MEM_PEAK=$(get_metric "mem.peak_percent")

echo
echo "=== §11.2 / §11.3 采集结果 ==="
sed 's/^\[perf\] /  /' "${CAPTURE}" | grep -v '^  ${'
echo "  cold_start.process_wall_s = ${COLD_START:-（/usr/bin/time 不可用）}"
echo

# ------------------------------------------------------- 门槛对照
FAILED=0
check() { # 名称 实测 单位 比较符 门槛
    local name="$1" val="$2" unit="$3" op="$4" limit="$5"
    if [[ -z "${val}" || "${val}" == "skipped" ]]; then
        printf '  %-28s %-10s %s\n' "${name}" "n/a" "（未测出或已跳过，不作判定）"
        return
    fi
    local pass=0
    if [[ "${op}" == "le" ]]; then
        (( val <= limit )) && pass=1 || pass=0
    else
        (( val >= limit )) && pass=1 || pass=0
    fi
    if (( pass == 1 )); then
        printf '  \033[32m[达标]\033[0m %-24s %s%s %s %s\n' "${name}" "${val}" "${unit}" "${op}" "${limit}${unit}"
    else
        printf '  \033[33m[未达]\033[0m %-24s %s%s %s %s\n' "${name}" "${val}" "${unit}" "${op}" "${limit}${unit}"
        FAILED=$((FAILED+1))
    fi
}

echo "=== §11.2 门槛对照（方案 A）==="
check "单次 FFI 调用开销"     "${FFI_RAW}"    "ns" "le" 2000
check "事件回调延迟"          "${EV_AVG}"     "ns" "le" 200000
check "输入延迟"              "${INPUT_MS}"   "ms" "le" 50
check "帧率（局部刷新）"      "${FPS_PART}"   "fps" "ge" 30
check "帧率（全屏动画）"      "${FPS_FULL}"   "fps" "ge" 20
check "启动时间（进程内）"    "${STARTUP_MS}" "ms" "le" 500
echo "=== §11.3 内存 ==="
check "LVGL 内存池峰值占比"   "${MEM_PEAK}"   "%"  "le" 80
echo

# ------------------------------------------------------- 存档
if [[ -n "${OUT_FILE}" ]]; then
    {
        echo "# perf 采集（$(date '+%Y-%m-%d %H:%M:%S')）"
        echo
        echo "> ★ 平台说明：本文件由 perf.sh 生成。**只有目标平台上的数字才具备判定效力**；"
        echo ">   WSL2 与 CI 容器受宿主负载、虚拟化与软件渲染影响，仅供横向比较。"
        echo
        echo '```'
        sed 's/^\[perf\] /  /' "${CAPTURE}"
        echo "  cold_start.process_wall_s = ${COLD_START:-N/A}"
        echo '```'
    } > "${OUT_FILE}" 2>/dev/null && echo "[perf] 已存档：${OUT_FILE}"
fi

if (( FAILED > 0 )); then
    if (( STRICT == 1 )); then
        echo "[perf] 有 ${FAILED} 项未达标（--strict）" >&2
        exit 1
    fi
    echo "[perf] 注意：有 ${FAILED} 项未达标。非 --strict 模式下不算失败 ——"
    echo "       请在目标平台复测后再判定（见文件头说明）。"
fi
exit ${PERF_RC}
