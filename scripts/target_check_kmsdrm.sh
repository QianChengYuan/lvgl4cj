#!/bin/bash
# ============================================================================
# target_check_kmsdrm.sh —— 体检：这台目标机**能不能**用 KMSDRM 直接上屏
# ============================================================================
#
# 为什么需要它：
#   在有桌面的机器上，SDL2 后端走 X11/Wayland；而在嵌入式设备上通常没有桌面，
#   此时 SDL2 **自带的 kmsdrm 驱动**是常规出路 —— 它直接对着 DRM/KMS 编程，
#   把画面写到显示控制器上，不需要 X/Wayland。
#
#   但「能不能用」不是一句话能答的，它取决于四件事，必须逐个查实：
#     1. 有 DRM 设备（/dev/dri/card*），且**当前用户有权限打开它**；
#     2. 有**已连接**的显示器（connector status = connected）——
#        没有连接时内核不提供可用模式，kmsdrm 会失败；
#     3. 本机这份 libSDL2 **在编译时带上了 kmsdrm 驱动**
#        （Debian / Raspberry Pi OS 的 libsdl2 一般带，但不能假设）；
#     4. 没有别的进程占着 DRM master（X / Wayland / 另一个 kmsdrm 程序在跑就不行）。
#
#   ★ 这四件事里任意一件不成立，失败的现象都可能很像别的问题（"初始化失败"
#     "no available video device"），所以先把现状打全，再谈试跑。
#
# 本脚本**只读**：不写文件、不改配置、不装包、不需要 root。可以直接读结果。
#
# 用法（在目标机上）：
#   bash target_check_kmsdrm.sh
# ============================================================================

echo "=== 1. 设备本体 ==="
echo "  model : $(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
echo "  架构  : $(uname -m)   内核: $(uname -r)"
echo "  OS    : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
free -m | awk 'NR==2 {printf "  内存  : %dMB 总 / %dMB 可用\n", $2, $7}'
echo "  当前用户: $(id -un)  组: $(id -Gn)"

echo
echo "=== 2. DRM 设备与权限（第 1、4 条）==="
if [ -d /dev/dri ]; then
    ls -l /dev/dri 2>&1 | sed 's/^/  /'
    for d in /dev/dri/card* /dev/dri/renderD*; do
        [ -e "$d" ] || continue
        if [ -r "$d" ] && [ -w "$d" ]; then
            echo "  可读写: $d  ✓"
        else
            echo "  权限不足: $d  ✗ —— 需要 root，或把用户加入 video/render 组"
        fi
    done
else
    echo "  ✗ 没有 /dev/dri —— 这台机器没有 KMS 驱动暴露给用户空间"
    echo "    （可能未启用 vc4-kms-v3d 覆盖层，或内核不是 KMS 形态）"
fi

echo
echo "=== 3. 显示器连接状态（第 2 条，最关键）==="
found=0
for f in /sys/class/drm/card*-*/status; do
    [ -e "$f" ] || continue
    found=1
    name="$(echo "$f" | sed 's|.*/drm/||; s|/status||')"
    st="$(cat "$f" 2>/dev/null)"
    extra=""
    mf="$(dirname "$f")/modes"
    if [ -r "$mf" ] && [ -s "$mf" ]; then
        extra="  模式: $(head -1 "$mf")"
    fi
    printf '  %-22s %s%s\n' "$name" "$st" "$extra"
done
[ "$found" = "0" ] && echo "  ✗ 找不到 /sys/class/drm/card*-*/status"
if [ "$found" = "1" ]; then
    if grep -q '^connected' /sys/class/drm/card*-*/status 2>/dev/null; then
        echo "  ✓ 至少有一个已连接的显示器 —— kmsdrm 有可用模式"
    else
        echo "  ✗ **没有任何已连接的显示器**。此时内核不提供模式，kmsdrm 起不来。"
        echo "    这不是代码问题：请先接上 HDMI/DSI 屏再测。"
    fi
fi

echo
echo "=== 4. 谁占着 DRM master（第 4 条）==="
if pgrep -f 'Xorg|/usr/bin/X|labwc|wayfire|weston|sway|gnome-shell|lightdm|gdm|sddm' > /dev/null 2>&1; then
    echo "  发现显示服务/桌面进程："
    pgrep -a -f 'Xorg|/usr/bin/X|labwc|wayfire|weston|sway|gnome-shell|lightdm|gdm|sddm' | head -6 | sed 's/^/    /'
    echo "  ⚠ 它们通常持有 DRM master —— kmsdrm 需要独占，二者不能同时在跑。"
else
    echo "  ✓ 没有发现 X / Wayland / 显示管理器在跑（kmsdrm 可以独占 DRM）"
fi
echo "  systemd 默认目标: $(systemctl get-default 2>/dev/null)"
echo "  DISPLAY=[${DISPLAY:-}]  WAYLAND_DISPLAY=[${WAYLAND_DISPLAY:-}]"
echo "  KMS 覆盖层: $(grep -hE '^\s*dtoverlay=.*(vc4|kms)' /boot/firmware/config.txt /boot/config.txt 2>/dev/null | head -3 | tr '\n' ' ')"

echo
echo "=== 5. SDL2 是否有 kmsdrm 驱动（第 3 条）==="
# ★ 必须写绝对路径 /sbin/ldconfig：Debian 普通用户的 PATH 里**没有 /sbin**，
#   于是裸调 `ldconfig` 会 command not found —— 而它被 2>/dev/null 吞掉之后，
#   SDLSO 变成空串，脚本就会打印"✗ 找不到 libSDL2-2.0.so.0"。
#   这是一个**看着像结论的假结论**：库明明在（见第 7 节的 libs/ 软链），
#   报告却说没有。本脚本第一版就是这么写的，实测当场被第 7 节打回。
SDLSO="$(/sbin/ldconfig -p 2>/dev/null | awk '/libSDL2-2\.0\.so\.0/ {print $NF; exit}')"
if [ -z "$SDLSO" ]; then
    # 兜底：直接找。ldconfig 缓存也可能没跟上（例如库是手工放进去的）。
    SDLSO="$(ls /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 \
                /usr/lib/libSDL2-2.0.so.0 /lib/aarch64-linux-gnu/libSDL2-2.0.so.0 \
                2>/dev/null | head -1)"
    [ -n "$SDLSO" ] && echo "  （ldconfig 缓存里没有，但文件存在——按文件判定）"
fi
if [ -n "$SDLSO" ]; then
    echo "  库文件: $SDLSO  ($(ls -l "$SDLSO" 2>/dev/null | awk '{print $5" 字节"}'))"
    echo "  版本  : $(strings "$SDLSO" 2>/dev/null | grep -m1 -E '^2\.[0-9]+\.[0-9]+$' || echo '?')"
    echo "  包    : $(dpkg -S "$SDLSO" 2>/dev/null | head -1 || echo '（dpkg 未登记——可能是手工放入的）')"
    if strings "$SDLSO" 2>/dev/null | grep -qi 'kmsdrm'; then
        echo "  ✓ 该库**带有** kmsdrm 驱动（字符串里有 KMSDRM 标识）"
    else
        echo "  ✗ 该库看不出 kmsdrm —— 可能编译时未启用 KMS/GBM"
    fi
    echo "  已编入的视频驱动（从字符串推断）:"
    for d in kmsdrm x11 wayland dummy offscreen; do
        if strings "$SDLSO" 2>/dev/null | grep -qi "SDL_VIDEODRIVER.*$d\|bootstrap_$d\|\"$d\""; then
            echo "      $d"
        fi
    done
else
    echo "  ✗ 找不到 libSDL2-2.0.so.0"
fi

echo
echo "=== 6. 构建/链接仓颉示例需要的东西 ==="
# ★ 仓颉侧**不需要 SDL2 的 C 头文件**：Pi 上不编译 C 层（那是宿主交叉编译好的，
#   见 run_on_target.sh），仓颉侧只要求**链接期**有个可用的 `.so`。
#   而 Debian 的 libsdl2-dev 才会提供 libSDL2.so 这个软链；没装 dev 包时
#   只有 libSDL2-2.0.so.0，`-lSDL2` 就找不到 —— 本工程的做法是在仓库里
#   自带一个软链（libs/libSDL2.so → 系统库），从而免掉 dev 包依赖。
if [ -f /usr/include/SDL2/SDL.h ]; then
    echo "  C 头文件: 有（/usr/include/SDL2/SDL.h）—— 但仓颉侧用不到"
else
    echo "  C 头文件: 无 —— **不影响**仓颉侧（Pi 上不编 C 层）"
fi
LIBSO="$(ls -l "$HOME"/lvgl4cj/libs/libSDL2.so 2>/dev/null | sed 's/.*-> //')"
if [ -n "$LIBSO" ]; then
    if [ -e "$LIBSO" ]; then
        echo "  链接软链: ✓ 仓库自带 libs/libSDL2.so → $LIBSO"
    else
        echo "  链接软链: ✗ 仓库里的 libs/libSDL2.so 指向不存在的 $LIBSO"
    fi
else
    echo "  链接软链: 无（仓库里没有 libs/libSDL2.so）"
fi
echo "  系统软链: $(ls /usr/lib/aarch64-linux-gnu/libSDL2.so 2>/dev/null || echo '无（未装 libsdl2-dev，正常）')"
if [ -f "$HOME/cangjie/envsetup.sh" ]; then
    echo "  仓颉 SDK: ✓ $HOME/cangjie"
else
    d="$(ls -d "$HOME"/cangjie* 2>/dev/null | head -1)"
    echo "  仓颉 SDK: ${d:-✗ 未找到 $HOME/cangjie/envsetup.sh}"
fi
echo "  cjHeapSize（当前 shell）: ${cjHeapSize:-未设置（构建/运行需要，且必须带单位）}"

echo
echo "=== 7. 目标机上的工程副本 ==="
if [ -d "$HOME/lvgl4cj" ]; then
    echo "  目录: $HOME/lvgl4cj"
    echo "  版本: $(git -C "$HOME/lvgl4cj" rev-parse --short HEAD 2>/dev/null || echo '（不是 git 工作副本）')"
    echo "  已构建的示例:"
    for m in "$HOME"/lvgl4cj/examples/*/target/release/bin/main; do
        [ -e "$m" ] || continue
        printf '      %-56s %s\n' "$m" "$(ls -l "$m" | awk '{print $5" 字节"}')"
    done
    echo "  预编译的 liblvgl4cj（若示例依赖）:"
    ls -l "$HOME"/lvgl4cj/libs/*.so* 2>/dev/null | sed 's/^/      /' | head -6
else
    echo "  ✗ 没有 $HOME/lvgl4cj（需要先把工程同步上去）"
fi

echo
echo "=== 体检结论 ==="
echo "  上面五节里，第 2 节（显示器已连接）与第 3 节（SDL2 带 kmsdrm）是硬前提。"
echo "  两者都为 ✓ 才值得进入实测；任一为 ✗，实测的失败都会有别的解释。"
