#!/usr/bin/env bash
# ============================================================================
# soak.sh —— 长时间运行压力测试（设计文档 §11.3 内存指标 / §11.4 稳定性指标）
#
# 判据（§11.1 / §11.3）：
#   · 无崩溃                        退出码为 0，且运行时长达到目标
#   · RSS 增长 ≤ 10 MB              **由本脚本在进程之外**采样 /proc/<pid>/statm
#   · 句柄表大小 稳态后不再增长      读 bench_cj 的 [soak] 状态行
#   · 闭包表大小 对象删除后归零
#   · anim_ctx 数量 稳态后归零
#
# ★ 为什么 RSS 必须由**进程之外**采样：
#   进程自己报的内存数字，无法排除「它把自己的账本也写坏了」这种可能；
#   而从 /proc 读的是内核记账，与应用的自述相互独立。
#   这与 §7.3「句柄数与对象数必须独立测量」是同一条原则。
#
# ============================================================================
# ★★ ASan 口径（§11.4 与 §9.5 互斥，已决策为「方案 a」）★★
# ============================================================================
#
# §11.4 要求 24h 运行满足「零崩溃、**零 ASan 报告**、内存收敛」，
# 但 §9.5 已实测确立「**ASan 与仓颉运行时硬不兼容**」：
# 混合二进制在 ASan 下会崩在退出路径（CJ_ScheduleStop 的线程簿记），
# 且 `detect_leaks=0` 也避不开 —— 不是 LSan 的问题，是拦截器与运行时的冲突。
#
# 也就是说「24h 混跑 + 零 ASan 报告」这个组合**不存在可实现路径**，
# 二者互斥。决策为方案 a：
#
#   · **主进程 24h（本脚本）不带 ASan**，按 §11.3 判 RSS 与四类计数收敛；
#   · **ASan 覆盖另由纯 C 长跑承载**：test/native/test_soak_asan.c
#     跑同样形状的负载（建/删/级联/样式/事件/动画），纳入 ASan 门禁。
#
# 两者的覆盖边界必须说清楚（不能含糊成"ASan 也测了"）：
#   · 本脚本：**运行时行为** + 内存/计数收敛，覆盖到仓颉侧闭包表；
#     但没有 ASan 的越界/UAF 检测能力。
#   · test_soak_asan：**有完整 ASan 检测**（越界、UAF、泄漏），
#     但只覆盖 **C 层状态**，看不到仓颉侧的闭包表。
#   二者互补，缺一不可 —— 单看任何一边都不能声称"长跑已验证"。
#
# 因此本脚本会**拒绝**被 ASan 插桩的二进制（见下面的守卫）：
# 那不是"更严格"，而是会崩在半路、白白浪费 24 小时。
#
# 用法：
#   bash scripts/soak.sh                          # 24h（86400 秒）
#   bash scripts/soak.sh --seconds 300            # 5 分钟（CI 冒烟用）
#   bash scripts/soak.sh --seconds 86400 --out /tmp/soak24h.txt
#
#   长跑可随时 Ctrl-C / SIGTERM 中断：**保留已采样数据并出部分报告**。
#   24h 跑一半发现要改代码时，不必从头再来。
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SECONDS_TARGET=86400
OUT_FILE=""
SAMPLE_INTERVAL=5
RSS_LIMIT_KB=$((10 * 1024))   # §11.3：24h RSS 增长 ≤ 10 MB
BIN_OVERRIDE=""               # --bin：仅用于验证守卫与调试，正常长跑不要用

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) SECONDS_TARGET="$2"; shift 2 ;;
        --out)     OUT_FILE="$2"; shift 2 ;;
        --interval) SAMPLE_INTERVAL="$2"; shift 2 ;;
        --bin)     BIN_OVERRIDE="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "[soak] 未知参数：$1" >&2; exit 2 ;;
    esac
done

[[ -n "${OUT_FILE}" ]] || OUT_FILE="${ROOT_DIR}/docs/benchmarks/soak_last.txt"

if [[ -z "${CANGJIE_HOME:-}" ]]; then
    for d in "$HOME"/cangjie/*/cangjie; do
        [[ -x "${d}/bin/cjc" ]] && CANGJIE_HOME="${d}" && break
    done
fi
export LD_LIBRARY_PATH="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative:${ROOT_DIR}/libs:${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=dummy
export PATH="${CANGJIE_HOME:-}/bin:${PATH}"

BIN="${BIN_OVERRIDE:-${ROOT_DIR}/examples/bench_cj/target/release/bin/main}"
if [[ ! -x "${BIN}" ]]; then
    echo "[soak] 找不到 ${BIN}" >&2
    echo "[soak] 请先：cd examples/bench_cj && cjpm build" >&2
    exit 2
fi

# ---------------------------------------------------------------- ASan 守卫
# ★ 这里**必须明确拒绝**，而不是"警告一下继续跑"。
#   被 ASan 插桩的混合二进制会在退出路径崩溃（§9.5 / R3），
#   而 24h 的长跑最不能接受的就是「跑到一半崩了、数据不可用」——
#   那不只是浪费 24 小时，还会被误读成"soak 发现崩溃"。
#   宁可在这里 2 秒钟失败。
if ldd "${BIN}" 2>/dev/null | grep -qi 'libasan'; then
    echo "[soak] ✗ 该二进制链接了 AddressSanitizer，不能用于主进程 soak。" >&2
    echo "        原因：ASan 与仓颉运行时硬不兼容（§9.5），混合二进制会崩在退出路径，24h 会白跑。" >&2
    echo "        请用普通（未插桩）的 C 库重建：" >&2
    echo "          bash scripts/build_native.sh && (cd examples/bench_cj && cjpm build)" >&2
    echo "        ASan 覆盖请用纯 C 长跑：test/native/test_soak_asan.c（见脚本头「ASan 口径」）。" >&2
    exit 2
fi

echo "[soak] 目标时长 ${SECONDS_TARGET} 秒；RSS 采样间隔 ${SAMPLE_INTERVAL}s；报告 → ${OUT_FILE}"

RAW_STATUS="$(mktemp)"
RSS_SAMPLES="$(mktemp)"
trap 'rm -f "${RAW_STATUS}" "${RSS_SAMPLES}"' EXIT

# ---------------------------------------------------------------- 中断处理
# ★ 24h 长跑必须可中断且**保留成果**：跑到一半发现要改代码时，
#   不该被迫从头再来。收到 INT/TERM 时停掉负载、照常出报告，
#   并在报告里标明这是**部分数据** —— 不假装跑满了。
PARTIAL=0
APP_PID=""
on_signal() {
    PARTIAL=1
    echo "[soak] 收到中断信号：停止负载，输出**部分**报告（已采样数据保留）"
    if [[ -n "${APP_PID}" ]]; then
        kill -TERM "${APP_PID}" 2>/dev/null || true
    fi
}
trap on_signal INT TERM

# ---------------------------------------------------------------- 启动负载
"${BIN}" soak --seconds "${SECONDS_TARGET}" > "${RAW_STATUS}" 2>&1 &
APP_PID=$!
echo "[soak] 负载进程 PID=${APP_PID}"

START_TS=$(date +%s)
WARMUP_KB=""
declare -a RSS_AT=()

# ---------------------------------------------------------------- 采样循环
while kill -0 "${APP_PID}" 2>/dev/null; do
    ELAPSED=$(( $(date +%s) - START_TS ))
    # /proc/<pid>/statm 第 2 个字段 = 常驻页数；页大小 = 4096B → KB = pages × 4
    PAGES=$(awk '{print $2}' "/proc/${APP_PID}/statm" 2>/dev/null)
    if [[ -n "${PAGES}" ]]; then
        RSS_KB=$(( PAGES * 4 ))
        echo "${ELAPSED} ${RSS_KB}" >> "${RSS_SAMPLES}"
        # 预热基线取「目标时长的 10% 或 20 秒」之中较大的那个时点
        if [[ -z "${WARMUP_KB}" ]]; then
            WARMUP_AT=$(( SECONDS_TARGET / 10 ))
            (( WARMUP_AT < 20 )) && WARMUP_AT=20
            if (( ELAPSED >= WARMUP_AT )); then
                WARMUP_KB="${RSS_KB}"
                echo "[soak] 预热基线（t=${ELAPSED}s）：RSS=${RSS_KB} KB"
            fi
        fi
    fi
    sleep "${SAMPLE_INTERVAL}"
done

wait "${APP_PID}"
APP_RC=$?
RUN_SECONDS=$(( $(date +%s) - START_TS ))

# ---------------------------------------------------------------- 汇总判定
FIRST_KB=$(head -1 "${RSS_SAMPLES}" 2>/dev/null | awk '{print $2}')
LAST_KB=$(tail -1 "${RSS_SAMPLES}" 2>/dev/null | awk '{print $2}')
MIN_KB=$(awk 'NR==1{m=$2} {if($2<m)m=$2} END{print m+0}' "${RSS_SAMPLES}")
MAX_KB=$(awk 'NR==1{m=$2} {if($2>m)m=$2} END{print m+0}' "${RSS_SAMPLES}")
[[ -n "${WARMUP_KB}" ]] || WARMUP_KB="${FIRST_KB:-0}"

DRIFT_LINE=$(grep '^\[soak\] drift' "${RAW_STATUS}" | tail -1)
DONE_LINE=$(grep '^\[soak\] done' "${RAW_STATUS}" | tail -1)

FAILED=0
REPORT=()
ok()  { REPORT+=("  [ PASS ] $1"); }
bad() { REPORT+=("  [ FAIL ] $1"); FAILED=$((FAILED+1)); }

# 1) 无崩溃
#
# ★ 被手动中断时这两条（无崩溃 / 时长达标）**不判失败**，只标注。
#   中断是使用者的主动动作，不是被测对象的失败；把它记成 FAIL 会让报告失真，
#   也会让人以后不敢中断长跑（那才是真的浪费）。
if (( PARTIAL == 1 )); then
    REPORT+=("  [ NOTE ] ★ 手动中断：以下是 ${RUN_SECONDS}s 的**部分**数据，不构成完整时长判定")
    REPORT+=("           （崩溃与时长两项在部分运行下无意义，故不判；RSS 与计数收敛仍有效）")
else
    if [[ "${APP_RC}" == "0" ]]; then
        ok "无崩溃（退出码 0，实际运行 ${RUN_SECONDS}s / 目标 ${SECONDS_TARGET}s）"
    else
        bad "进程异常退出：退出码 ${APP_RC}（实际 ${RUN_SECONDS}s）"
    fi

    # 2) 运行时长达到目标
    #    容差 15s：RUN_SECONDS 是脚本侧墙钟（含进程启动与退出），
    #    与负载内部按 monotonicUs 的计时存在秒级偏差 ——
    #    把这条判据卡成「差 7 秒就算失败」只会制造噪音，而它要拦的是「早就退出了」。
    if (( RUN_SECONDS + 15 >= SECONDS_TARGET )); then
        ok "运行时长达标（实际 ${RUN_SECONDS}s / 目标 ${SECONDS_TARGET}s）"
    else
        bad "运行时长不足：${RUN_SECONDS}s < ${SECONDS_TARGET}s"
    fi
fi

# 3) RSS 增长 ≤ 10 MB
#
# ★ 判据用**上包络**（各时间窗内最大值）比较首段与末段，而不是「末值 − 预热基线」。
#   原因由实测给出：一次 300s 运行里 RSS 从 1.3 MB 升到 30 MB 后又**回落到 18.5 MB** ——
#   仓颉是带 GC 的运行时，回收时 RSS 会明显下降，因此 RSS **根本不是单调量**。
#   用「末值 − 基线」会得出荒谬结论：正好落在回收之后的那一瞬显示「负增长」，
#   而落在回收之前则把尚未回收的峰值当成泄漏。
#   取每个窗口的最大值再比首末段，衡量的才是「同样负载下进程能到达的峰值
#   是否随运行时间抬高」—— 这正是 §11.3「RSS 收敛」的本意。
BUCKETS=10
ENV_FIRST=$(awk -v k="${BUCKETS}" '
    {t[NR]=$1; r[NR]=$2; if($1>maxt) maxt=$1}
    END{ if(NR==0){print 0; exit}
         win=maxt/k; if(win<=0) win=1; best=0;
         for(i=1;i<=NR;i++){ if(int(t[i]/win)==0){ if(r[i]>best) best=r[i] } }
         print best }' "${RSS_SAMPLES}")
ENV_LAST=$(awk -v k="${BUCKETS}" '
    {t[NR]=$1; r[NR]=$2; if($1>maxt) maxt=$1}
    END{ if(NR==0){print 0; exit}
         win=maxt/k; if(win<=0) win=1; best=0;
         for(i=1;i<=NR;i++){ if(int(t[i]/win)>=k-1){ if(r[i]>best) best=r[i] } }
         print best }' "${RSS_SAMPLES}")
[[ -n "${ENV_LAST}" && "${ENV_LAST}" != "0" ]] || ENV_LAST="${LAST_KB}"
RSS_GROWTH=$(( ENV_LAST - ENV_FIRST ))
if (( RSS_GROWTH <= RSS_LIMIT_KB )); then
    ok "RSS 上包络增长 ${RSS_GROWTH} KB ≤ ${RSS_LIMIT_KB} KB（首段峰值 ${ENV_FIRST} → 末段峰值 ${ENV_LAST}；全程区间 ${MIN_KB}~${MAX_KB}）"
else
    bad "RSS 上包络增长 ${RSS_GROWTH} KB > ${RSS_LIMIT_KB} KB（首段峰值 ${ENV_FIRST} → 末段峰值 ${ENV_LAST}）"
fi

# 3b) 短跑不足以判定收敛 —— 必须**明说**，而不是让它悄悄通过
if (( SECONDS_TARGET < 3600 )); then
    REPORT+=("  [ NOTE ] 本次仅 ${SECONDS_TARGET}s（<1h）。此尺度**不足以判定 24h 收敛**：")
    REPORT+=("           实测 300s 内 RSS 从 1.3 MB 升至 30 MB 再回落至 18.5 MB（GC 回收），上包络仍在抬升。")
    REPORT+=("           §11.3 要求 24h，请用 --seconds 86400 复测后再下结论。")
fi

# 4) 计数漂移（由负载自己报告，与 RSS 相互独立）
if [[ -n "${DRIFT_LINE}" ]]; then
    D_ALIVE=$(echo "${DRIFT_LINE}"   | sed -n 's/.*alive=\(-\?[0-9]*\).*/\1/p')
    D_OBJ=$(echo "${DRIFT_LINE}"     | sed -n 's/.*obj=\(-\?[0-9]*\).*/\1/p')
    D_CLOS=$(echo "${DRIFT_LINE}"    | sed -n 's/.*closures=\(-\?[0-9]*\).*/\1/p')
    ok "计数漂移：alive=${D_ALIVE} obj=${D_OBJ} closures=${D_CLOS}（三者应为 0）"
    [[ "${D_ALIVE}" == "0" ]] || bad "句柄数漂移 ${D_ALIVE}（§11.3 要求稳态不增长）"
    [[ "${D_OBJ}"   == "0" ]] || bad "对象数漂移 ${D_OBJ}"
    [[ "${D_CLOS}"  == "0" ]] || bad "闭包数漂移 ${D_CLOS}（§11.3 要求对象删除后归零）"
else
    bad "未拿到计数漂移行（运行时间可能过短，未建立基线）"
fi

# 5) anim_ctx 归零（末态）
if [[ -n "${DONE_LINE}" ]]; then
    A_CTX=$(echo "${DONE_LINE}" | sed -n 's/.*animctx=\(-\?[0-9]*\).*/\1/p')
    if [[ "${A_CTX}" == "0" ]]; then
        ok "末态 anim_ctx = 0（§11.3）"
    else
        bad "末态 anim_ctx = ${A_CTX} ≠ 0"
    fi
else
    bad "未拿到末态行"
fi

# ---------------------------------------------------------------- 写报告
{
    echo "# soak 报告"
    echo
    echo "- 日期：$(date '+%Y-%m-%d %H:%M:%S')"
    echo "- 目标时长：${SECONDS_TARGET}s    实际：${RUN_SECONDS}s"
    echo "- RSS 采样间隔：${SAMPLE_INTERVAL}s    采样点数：$(wc -l < "${RSS_SAMPLES}")"
    echo "- RSS：预热 ${WARMUP_KB} KB → 终值 ${LAST_KB} KB（区间 ${MIN_KB}~${MAX_KB} KB），增长 ${RSS_GROWTH} KB"
    echo "- 末态：${DONE_LINE}"
    echo "- 漂移：${DRIFT_LINE}"
    echo
    echo "## 判定（§11.3 / §11.4）"
    printf '%s\n' "${REPORT[@]}"
    echo
    echo "## 原始 RSS 采样（elapsed_s rss_kb）"
    cat "${RSS_SAMPLES}"
} > "${OUT_FILE}" 2>/dev/null || {
    echo "[soak] 报告写入 ${OUT_FILE} 失败（目录不存在？），改为输出到标准错误" >&2
    printf '%s\n' "${REPORT[@]}" >&2
}

printf '\n=== soak 判定 ===\n'
printf '%s\n' "${REPORT[@]}"
echo
echo "[soak] 报告：${OUT_FILE}"
if (( FAILED > 0 )); then
    echo "[soak] 有 ${FAILED} 项未通过" >&2
    exit 1
fi
echo "[soak] 全部通过"
