#!/bin/sh
# ============================================================================
# target_soak.sh —— 在**目标机**上跑长跑，并做进程外 RSS 采样
# ============================================================================
#
# 为什么必须在目标机上跑（§11.3 的要求）：
#   长跑要回答的是「这台机器长时间运行会不会不收敛」。开发机（WSL2，x86，
#   内存充裕、GC 与调度行为都不同）上的结论**不能**搬到目标形态硬件上 ——
#   本目标机是 Raspberry Pi Zero 2 W：415MB 总量、可用常年只有 100–190MB，
#   内存压力与颠簸特征完全不同。
#
# ★ RSS 为什么由**进程外**采样：
#   被测进程自报的 RSS 无法排除"自己写坏自己的账本"这种可能，
#   而长跑要回答的恰恰是"长期运行会不会把账本写坏"。
#   用同一份账本既当被测对象又当裁判，是没有意义的。
#   （这与 scripts/soak.sh 在开发机上由外部采样的理由完全一致。）
#
# 结果落在三个文件（都在二进制同目录）：
#   soak_rss.csv      —— elapsed_s,rss_kb（每 60 秒一个样本）
#   soak_stdout.log   —— 被测进程的完整输出（含它自己每 60 秒打的计数进度）
#   .soak_done        —— 完成后写入退出码，用于判定"跑完了"而不是"读到一半"
#
# 用法：
#   sh target_soak.sh [秒数]        # 默认 86400 = 24 小时
#
# 中断与查看（都不影响被测进程）：
#   grep -c . soak_rss.csv ; tail -3 soak_rss.csv ; tail -3 soak_stdout.log
# ============================================================================

SECS="${1:-86400}"
cd "$(dirname "$0")" || exit 1

RSS_CSV="soak_rss.csv"
LOG="soak_stdout.log"
DONE=".soak_done"

rm -f "$RSS_CSV" "$LOG" "$DONE"
echo "elapsed_s,rss_kb" > "$RSS_CSV"

SDL_VIDEODRIVER=dummy ./test_soak_asan --seconds "$SECS" > "$LOG" 2>&1 &
PID=$!
START=$(date +%s)

echo "已启动：pid=$PID  时长=${SECS}s  开始=$(date '+%F %T')"

# 采样循环：被测进程一走完就退出，不做无谓等待
while kill -0 "$PID" 2>/dev/null; do
    E=$(( $(date +%s) - START ))
    R=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null)
    if [ -n "$R" ]; then
        echo "$E,$R" >> "$RSS_CSV"
    fi
    sleep 60
done

wait "$PID"
RC=$?
echo "$RC" > "$DONE"

echo "结束：退出码 $RC  结束=$(date '+%F %T')"

# 概要：只报能立刻看出问题的量。收敛性判断需要看整条曲线，不能只看首末两点。
awk -F, '
    NR > 1 {
        n++
        if (min == 0 || $2 < min) min = $2
        if ($2 > max) max = $2
        last = $2
    }
    END {
        if (n == 0) { print "RSS: 无样本（进程可能立即失败，见 soak_stdout.log）"; exit }
        printf "RSS: 样本 %d 个 / 最小 %d KB / 最大 %d KB / 最后 %d KB\n", n, min, max, last
        printf "     上升量 %d KB（%.1f%%）—— ★ 短跑内的小幅上升不构成结论，\n", max - min, 100.0 * (max - min) / min
        printf "        需要看 soak_rss.csv 整条曲线是否在振荡后回到同一水平\n"
    }
' "$RSS_CSV"

echo "--- 被测进程输出的最后 3 行 ---"
tail -3 "$LOG"
