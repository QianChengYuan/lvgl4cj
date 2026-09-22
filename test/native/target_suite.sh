#!/bin/sh
# ============================================================================
# target_suite.sh —— 在**目标机**上运行全部 C 层用例（本文件部署到目标机执行）
# ============================================================================
#
# 为什么不走 ctest：
#   目标机（Raspberry Pi Zero 2 W）**没有 cmake 且装不了** —— 实测它连不上
#   deb.debian.org。而 ctest 只是调度器：直接跑二进制能得到同样的输出
#   （每个用例自己打印「=== 结果：N 项检查，M 项失败 ===」），却少一个部署依赖。
#
# 为什么结果写文件、而不是直接打到 stdout：
#   目标机走 WiFi，实测会间歇性整段失联。跑长一点的用例时 SSH 会话可能断掉，
#   若结果只在 stdout 上，断线就丢结果。写进 report.txt 后，本地只需
#   「断线就下次再读」，测试本身不受影响（由 run_on_target.sh 以 nohup 启动）。
#   这也是为什么完成标志是一个独立文件 .done —— 它和报告一起被读到才算跑完。
#
# 用法（在目标机上，于二进制所在目录执行）：
#   sh target_suite.sh
# ============================================================================

cd "$(dirname "$0")" || exit 1

: > report.txt
pass=0
fail=0
skip=0

# 与 CI / gate.sh 的清单保持一致（顺序固定，便于三方逐项对照）
for t in test_handle_table test_queue test_display_indev test_null_backend \
         test_sdl2_backend test_style_widgets test_anim test_leak \
         test_soak_asan test_fullscreen_refresh; do
    if [ ! -x "./$t" ]; then
        # SDL2 后端未交叉编译时 test_sdl2_backend 不会存在，这里如实标注而非跳过
        printf '  %-24s (未构建)\n' "$t" >> report.txt
        skip=$((skip + 1))
        continue
    fi

    s=$(date +%s)
    out=$(SDL_VIDEODRIVER=dummy ./"$t" 2>&1)
    rc=$?
    e=$(( $(date +%s) - s ))
    # ★ 匹配中间那句「N 项检查，M 项失败」，**不要**匹配前缀。
    #   各用例的汇总前缀并不统一：test_queue 打 "=== 结果：…"，
    #   而 test_handle_table 打 "=== PASS：…"。
    #   首版按 "结果：" 匹配，于是 test_handle_table 明明 rc=0、33 项全过，
    #   却被计成"失败" —— 一次纯粹的识别错误被当成了产品问题。
    sm=$(printf '%s' "$out" | grep -E '项检查' | tail -1)

    printf '  %-24s %4ss rc=%d %s\n' "$t" "$e" "$rc" "$sm" >> report.txt

    if printf '%s' "$sm" | grep -q '0 项失败'; then
        pass=$((pass + 1))
    elif [ -z "$sm" ]; then
        # ★ 没有汇总 ≠ 通过。退出码 0 但没产出汇总，说明进程在打印汇总前就结束了；
        #   把它当成功，等于用"没有证据"冒充证据。
        #   （与 CI 里"SIGKILL 被杀却被报成断言失败"是同一类错的镜像。）
        fail=$((fail + 1))
        printf '        ★ 未产出汇总（退出码 %d）—— 不是断言失败，是进程提前结束\n' "$rc" >> report.txt
        printf '%s\n' "$out" | tail -5 >> report.txt
    else
        fail=$((fail + 1))
        # 直接列出失败项：否则要把整份日志拷回来才知道是哪几条
        printf '%s\n' "$out" | grep -E '\[FAIL\]' | head -5 >> report.txt
    fi
done

printf '\n  ==== 通过 %d / 失败 %d / 未构建 %d ====\n' "$pass" "$fail" "$skip" >> report.txt

# 完成标志写在最后：本地读到它才认为整轮结束（而不是读到一半的报告就下结论）
echo done > .done
