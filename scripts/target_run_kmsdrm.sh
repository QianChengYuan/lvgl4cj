#!/bin/bash
# ============================================================================
# target_run_kmsdrm.sh —— 用 KMSDRM 把画面**真的**显示到目标机的屏幕上
# ============================================================================
#
# 为什么需要这个脚本，而不是直接跑可执行文件：
#
#   1. KMSDRM 要求**独占 DRM master**。Raspberry Pi OS 默认起 lightdm + labwc
#      （Wayland 合成器）—— 它们占着 master。此时 kmsdrm 初始化会失败，
#      而 SDL **不会报错退出**，它会静默回落到别的驱动（labwc 在跑时通常是 wayland）。
#      必须先让出 DRM，跑完再还回去（本脚本用 trap，异常退出也还）。
#
#   2. 窗口尺寸必须**精确匹配**一个内核模式：后端建的是非全屏窗口
#      （SDL_WINDOW_SHOWN，见 backend/sdl2/sdl2_backend.c），SDL 不会替你挑模式。
#      本脚本直接读内核给出的模式原样传下去 —— 手抄那个数字是没必要的错误来源。
#
#   3. 「是不是真的在 KMSDRM 上」必须由**实际生效的驱动名**判定，不能由
#      SDL_VIDEODRIVER 判定（那只是请求）。脚本最后检查这一项，
#      不符合就**以非 0 退出** —— 否则"回落成功"会被当成"上屏成功"。
#
# 用法（在目标机上）：
#   bash target_run_kmsdrm.sh <可执行文件> [截图输出路径]
#   LVGLCJ_SUDO_PW=<密码> bash target_run_kmsdrm.sh ...   # 需要停/起 lightdm 时
#
# 退出码：0 = 确实在 KMSDRM 上跑完并有截图
#         1 = 前提不满足（没接屏 / 没有密码停桌面）
#         2 = 驱动名不符（回落了）或没拿到截图
# ============================================================================
set -uo pipefail

BIN="${1:?用法: bash target_run_kmsdrm.sh <可执行文件> [截图输出路径]}"
SHOT="${2:-$HOME/kmsdrm_shot.ppm}"
CONN="${LVGLCJ_CONN:-/sys/class/drm/card0-HDMI-A-1}"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "=== 1. 显示器与模式 ==="
STATUS="$(cat "$CONN/status" 2>/dev/null)"
MODE="$(head -1 "$CONN/modes" 2>/dev/null)"
echo "  连接器: $CONN"
echo "  状态  : ${STATUS:-未知}"
echo "  模式  : ${MODE:-无}"
if [ "$STATUS" != "connected" ] || [ -z "$MODE" ]; then
    echo "  ✗ 没有已连接的显示器或没有可用模式 —— kmsdrm 起不来。" >&2
    echo "    这不是代码问题：请先给目标机接一块屏（HDMI / DSI）。" >&2
    exit 1
fi
W="${MODE%x*}"
H="${MODE#*x}"

echo
echo "=== 2. 让出 DRM master ==="
HELD=""
pgrep -f 'labwc|wayfire|weston|sway|gnome-shell' > /dev/null 2>&1 && HELD="合成器"
pgrep -f '/usr/sbin/lightdm' > /dev/null 2>&1 && HELD="$HELD 显示管理器"
STOPPED=0

# 无论怎么退出都要把桌面还回去：否则目标机会停在一个没有桌面的状态
restore_desktop() {
    if [ "$STOPPED" = "1" ]; then
        echo
        echo "=== 6. 还回桌面 ==="
        printf '%s\n' "$LVGLCJ_SUDO_PW" | sudo -S systemctl start lightdm 2>&1 | grep -v '^\[sudo\]' | head -3
        echo "  已重新启动 lightdm"
    fi
}
trap restore_desktop EXIT

if [ -n "$HELD" ]; then
    echo "  正在运行:$HELD —— 它们持有 DRM master"
    if [ -z "${LVGLCJ_SUDO_PW:-}" ]; then
        echo "  ✗ 必须先停掉它们。两条路：" >&2
        echo "      提供密码： LVGLCJ_SUDO_PW=<密码> bash $0 $BIN $SHOT" >&2
        echo "      或手工  ： sudo systemctl stop lightdm   （跑完 sudo systemctl start lightdm）" >&2
        exit 1
    fi
    printf '%s\n' "$LVGLCJ_SUDO_PW" | sudo -S systemctl stop lightdm 2>&1 | grep -v '^\[sudo\]' | head -3
    STOPPED=1
    # 合成器退出与 DRM master 释放之间有一点时间差，必须等它真的退出
    for _i in $(seq 1 30); do
        pgrep -f 'labwc|/usr/sbin/lightdm' > /dev/null 2>&1 || break
        sleep 0.5
    done
    if pgrep -f 'labwc|/usr/sbin/lightdm' > /dev/null 2>&1; then
        echo "  ✗ 停止后仍有进程在跑，DRM master 可能没释放（继续下去结论不可信）" >&2
        exit 1
    fi
    echo "  ✓ 已让出（lightdm/labwc 都已退出）"
else
    echo "  ✓ 没有发现桌面进程，DRM 是空闲的"
fi

echo
echo "=== 3. 用 KMSDRM 运行（窗口尺寸取内核模式 ${W}x${H}）==="
export SDL_VIDEODRIVER=kmsdrm
rm -f "$SHOT"
OUT="$(bash "$HERE/target_run_cj.sh" "$BIN" --smoke --size "${W}x${H}" --shot "$SHOT" 2>&1)"
RC=$?
printf '%s\n' "$OUT" | sed 's/^/    /'
echo "  进程退出码: $RC"

echo
echo "=== 4. 判定：看**实际生效**的驱动名 ==="
DRV="$(printf '%s\n' "$OUT" | sed -n 's/.*视频驱动 \([^，]*\)，.*/\1/p' | head -1)"
echo "  实际驱动: ${DRV:-（没能从输出里取到）}"
if [ -z "$DRV" ]; then
    echo "  ✗ 取不到驱动名 —— 输出格式变了吗？" >&2
    exit 2
fi
case "$DRV" in
    KMSDRM|kmsdrm)
        echo "  ✓ 确实在 KMSDRM 上（不是回落到别的驱动）" ;;
    *)
        echo "  ✗ 期望 KMSDRM，实际是 $DRV —— SDL **回落到**了别的驱动。" >&2
        echo "    这不是「上屏成功」，恰恰相反：kmsdrm 没起来。" >&2
        echo "    常见原因：DRM master 仍被占用 / 窗口尺寸没有匹配的模式。" >&2
        exit 2 ;;
esac

echo
echo "=== 5. 帧与截图 ==="
printf '%s\n' "$OUT" | grep -a '已干净退出' | sed 's/^/    /'
if [ -f "$SHOT" ]; then
    echo "  ✓ 截图: $SHOT（$(stat -c %s "$SHOT") 字节，P6 PPM）"
else
    echo "  ✗ 没有截图文件" >&2
    exit 2
fi
echo
echo "  说明：上面是 --smoke 有界运行（约 1.2 秒）。想在屏幕上多看一会儿，"
echo "        去掉 --smoke 再跑一次即可： SDL_VIDEODRIVER=kmsdrm bash $HERE/target_run_cj.sh $BIN --size ${W}x${H}"
