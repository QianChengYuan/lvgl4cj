#!/usr/bin/env bash
# ============================================================================
# run_on_target.sh —— 在**真实** arm64 目标机上构建并运行 C 层用例
# ============================================================================
#
# 与 qemu 的区别（这是本脚本存在的全部理由）：
#   qemu 只能证明「二进制能执行、逻辑结果一致」；它证明不了目标机的
#     · 真实内存压力（本目标机 Raspberry Pi Zero 2 W：415MB 总量、可用 ~100–190MB）
#     · 真实调度与线程时序（并发/队列用例的实际行为）
#     · 真实 CPU 能力（性能指标必须在目标平台判定，见 §11.2/§11.3）
#   所以「arm64 已验证」这句话此前一直带着"仅 qemu"的限定；本脚本把它撤掉。
#
# ★★ 本脚本是按「链路会抖」写的 —— 不是保守，是被实测教育的 ★★
#   目标机走 WiFi，实测 RTT 抖动 5.7ms–1028ms，并会间歇性整段 No route to host。
#   四条硬约束，每一条都对应一次踩坑：
#     1. **不用 set -e**。单次连接失败要能重试，不能把整个脚本带停。
#        首版用了 set -e，一次抖动就让输出停在 "=== 3. 部署 ===" 之后一片空白。
#     2. **rsync --partial --append-verify 续传**，而不是 tar 单流。
#        tar 一旦中断就得从头再来（实测连续 5 次都死在传输中），
#        rsync 则在断点继续，抖动最终只是"多花几次"而不是"永远传不完"。
#        （注意：`--contimeout` 只用于 rsync daemon 模式，走 ssh 时必须去掉，
#          否则 rsync 直接报错退出，看起来像链路问题其实是参数用错。）
#     3. 保活要**放宽**。默认 ServerAliveCountMax=3 时，链路一停顿就被判死
#        （"Timeout, server not responding"），而实际上它只是在慢慢传。
#     4. 测试在目标机上 **nohup 后台**跑，本地只轮询结果文件 ——
#        会话断掉不会杀掉测试。（目标机没装 tmux，nohup 是唯一可用手段。）
#
# 用法：
#   bash scripts/run_on_target.sh [ssh别名]      # 默认 pi
#
# 前置：
#   · 目标机已配置免密登录（需一次性密码操作）
#   · 目标机 glibc **不旧于**宿主机交叉工具链的 glibc。实测宿主 2.39 / 目标 2.41 ✓。
#     若目标机更旧，会在运行时以 "version `GLIBC_2.39' not found" 失败，
#     那种情况下必须改为在目标机上原生构建，而不是在本机交叉编译。
# ============================================================================
set -uo pipefail

HOST="${1:-pi}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LVGLCJ_TARGET_BUILD:-$HOME/lvgl4cj-build-a64rel}"
REMOTE_DIR="${LVGLCJ_TARGET_DIR:-lvgl4cj-target}"

# 统一：免密、放宽保活（链路会停顿时不该判死）
SSHOPT="-o BatchMode=yes -o ConnectTimeout=25 -o ServerAliveInterval=20 -o ServerAliveCountMax=30"
sshq() { timeout 60 ssh $SSHOPT "$HOST" "$@"; }

echo "=== 1. 等待目标机（链路会抖：重试，而不是放弃）==="
ready=0
for i in $(seq 1 24); do
    if timeout 30 ssh $SSHOPT "$HOST" "echo ok" > /dev/null 2>&1; then
        ready=1
        echo "  第 $i 次探测：可达"
        break
    fi
    sleep 5
done
if [ "$ready" != "1" ]; then
    echo "  目标机不可达，中止"
    exit 1
fi

sshq 'echo "  目标: $(hostname) $(uname -m) $(cat /proc/device-tree/model 2>/dev/null)"; \
      echo "  内存: $(free -m | sed -n 2p | awk "{print \$2\"MB 总 / \"\$7\"MB 可用\"}")"; \
      echo "  glibc: $(/usr/lib/aarch64-linux-gnu/libc.so.6 2>/dev/null | head -1)"'

echo
echo "=== 2. 关闭目标机 WiFi 省电（链路掉线的头号成因）==="
# 目标机上 iw / iwconfig **都没装**（而且装不了：它连不上 deb.debian.org），
# 但 Raspberry Pi OS Bookworm 起由 NetworkManager 管理网络，nmcli 是有的 —— 走它。
# wifi.powersave: 2 = 关闭省电。省电模式下网卡会长时间休眠，
# 表现正是"小包过去、突发流量把连接拖死"。
# ★ 改连接需要 root，而 sudo 密码**不能写进仓库**。所以这一步是可选的，
#   只有显式提供 LVGLCJ_TARGET_SUDO_PW 时才执行。
#   （首版直接就调 nmcli，拿到的是 "Insufficient privileges"，
#    却仍打印出一行看起来正常的 powersave 现状 —— 失败了却像成功。）
#
#   为什么值得做：省电模式下网卡会长时间休眠，典型症状正是
#   "小包能过、突发流量把连接拖死"。实测在旧热点下 8.2MB 的传输
#   连续 5 次都死在中途；换到稳定热点后一次通过 —— 所以这是"可能不需要，
#   但抖动时第一个该查的地方"。
if [ -n "${LVGLCJ_TARGET_SUDO_PW:-}" ]; then
    sshq "CON=\$(nmcli -t -f NAME,TYPE connection show 2>/dev/null | awk -F: '/wireless/{print \$1; exit}'); \
          if [ -z \"\$CON\" ]; then echo '  未找到无线连接，跳过'; exit 0; fi; \
          echo \"  无线连接: \$CON\"; \
          echo '${LVGLCJ_TARGET_SUDO_PW}' | sudo -S nmcli connection modify \"\$CON\" wifi.powersave 2 2>&1 | grep -v '^\[sudo\]' | head -2; \
          echo '${LVGLCJ_TARGET_SUDO_PW}' | sudo -S nmcli connection up \"\$CON\" > /dev/null 2>&1; \
          echo -n '  powersave 现状: '; \
          nmcli -f 802-11-wireless.powersave connection show \"\$CON\" | tail -1"
else
    echo "  跳过（未提供 LVGLCJ_TARGET_SUDO_PW）"
    echo "  需要时：LVGLCJ_TARGET_SUDO_PW=<密码> bash scripts/run_on_target.sh"
fi

echo
echo "=== 3. 交叉编译（Release；SDL2 关）==="
# ★ CMAKE_TOOLCHAIN_FILE 必须给**绝对路径**：给相对路径时 CMake 相对**构建目录**解析，
#   找不到就静默忽略，随后以一条与工具链毫无关系的报错失败
#   （实测报的是 "Generator: execution of make failed"，看起来像 make 不见了）。
if [ ! -d "$BUILD" ]; then
    cmake -S "$ROOT/native" -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/native/cmake/aarch64-linux-gnu.cmake" \
        -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
        -DCMAKE_BUILD_TYPE=Release \
        -DLVGLCJ_BUILD_TESTS=ON \
        -DLVGLCJ_BUILD_SDL2=OFF || exit 1
fi
if ! cmake --build "$BUILD" -j"$(nproc)" > /tmp/target_build.log 2>&1; then
    tail -20 /tmp/target_build.log
    echo "  构建失败"
    exit 1
fi
# 告警也要拦：本项目纪律是零告警，而"交叉编译零告警"只有在这里才真正被检验
WARN="$(grep -cE 'warning:|error:' /tmp/target_build.log || true)"
echo "  告警/错误数: $WARN"
if [ "$WARN" != "0" ]; then
    grep -E 'warning:|error:' /tmp/target_build.log | head -10
    exit 1
fi

echo
echo "=== 4. 部署（rsync 续传）==="
# 测试二进制是**静态链接**（readelf -d 只看到 libc.so.6 与 ld-linux-aarch64），
# 所以只传 bin/ 即可，不需要部署 liblvglcj.so —— 实测确认，不是假设。
sshq "mkdir -p ~/$REMOTE_DIR/bin" > /dev/null 2>&1

deployed=0
for i in $(seq 1 15); do
    if rsync -az --partial --append-verify --timeout=180 \
            -e "ssh $SSHOPT" \
            "$BUILD/bin/" "$HOST:~/$REMOTE_DIR/bin/" > /tmp/target_rsync.log 2>&1; then
        deployed=1
        echo "  第 $i 次传输：完成"
        break
    fi
    echo "  第 $i 次中断：$(tail -2 /tmp/target_rsync.log | tr -d '\r' | tr '\n' ' ' | cut -c1-90)"
    sleep 3
done
if [ "$deployed" != "1" ]; then
    echo "  部署失败"
    exit 1
fi
# 目标机侧的执行脚本单独部署：它存在于目标机上、而不是藏在字符串里，
# 才能被单独阅读与修改（也更少一层引号转义）
rsync -az -e "ssh $SSHOPT" "$ROOT/test/native/target_suite.sh" \
    "$HOST:~/$REMOTE_DIR/bin/target_suite.sh" > /dev/null 2>&1
echo "  文件数: $(sshq "ls ~/$REMOTE_DIR/bin | wc -l")"

echo
echo "=== 5. 在目标机上后台运行（nohup：会话断了不影响测试）==="
sshq "cd ~/$REMOTE_DIR/bin && rm -f .done report.txt && nohup sh ./target_suite.sh > /dev/null 2>&1 & echo LAUNCHED"

echo
echo "=== 6. 轮询结果（链路断了只是少读一次，不影响测试）==="
for i in $(seq 1 90); do
    sleep 10
    out="$(timeout 60 ssh $SSHOPT "$HOST" \
        "cd ~/$REMOTE_DIR/bin 2>/dev/null && { cat report.txt 2>/dev/null; test -f .done && echo __DONE__; }" 2>/dev/null)"
    if printf '%s' "$out" | grep -q '__DONE__'; then
        printf '%s\n' "$out" | grep -v '__DONE__'
        echo
        echo "目标机运行完成。"
        exit 0
    fi
    printf '  等待中… (%d/90)\r' "$i"
done
echo
echo "  超时未完成，取当前部分结果："
sshq "cat ~/$REMOTE_DIR/bin/report.txt 2>/dev/null" || true
exit 1
