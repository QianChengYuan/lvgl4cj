#!/usr/bin/env bash
# ============================================================================
# run_on_target_kmsdrm.sh —— 在真实目标机上做一次 KMSDRM 上屏实测
# ============================================================================
#
# 它做五件事，每一件都是前一件的前提：
#   1. 等目标机可达（链路会抖：重试，而不是放弃 —— 同 run_on_target.sh）
#   2. **交叉编译 aarch64 静态库**（含 SDL2 后端）
#   3. 把源码与 aarch64 的 .a 同步上去（目标机没有 cmake，装不了；见下）
#   4. 在目标机上构建 hello_cj
#   5. 用 KMSDRM 跑一次，并把截图取回来
#
# ---------------------------- 两个容易踩空的地方 ----------------------------
#
# ★ A. 为什么必须在宿主机交叉编译，而不能在目标机上编：
#      目标机（Raspberry Pi Zero 2 W）**没有 cmake 且装不了**（实测连不上
#      deb.debian.org）。C 层只能在宿主机交叉编译好、把 .a 传过去。
#      注意产物必须放到**独立的构建目录**，绝不能覆盖工程的 libs/ ——
#      那里放的是宿主的 x86 库，覆盖掉会让本地构建与测试悄悄坏掉（不报错、只是跑错东西）。
#
# ★ B. 为什么交叉编译 SDL2 后端需要"垫一层头文件"：
#      Debian 把 SDL_config.h 做成了**架构化的间接层**：
#          /usr/include/SDL2/SDL_config.h  →  #include <SDL2/_real_SDL_config.h>
#      而那个真文件装在**编译器默认包含目录**里（本机只有 x86_64 那份：
#      /usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h）。
#      交叉编译时没有 aarch64 那一份 —— 这就是文档里"宿主无 arm64 SDL2 开发包"
#      的具体形态。表现为一句与架构毫无关系的报错：
#          fatal error: SDL2/_real_SDL_config.h: No such file or directory
#      本次绕法：把本机那份复制到构建目录下的垫片目录，并用 -I 指过去。
#      ★ 代价要说清楚：这样编出来的 .a 是**对着 x86_64 的 SDL_config.h** 编译的。
#        对本后端的用法（只用不透明指针与整型常量，不碰结构体布局）是安全的，
#        而且真正的链接与运行都在目标机上、用的是目标机自己的 libSDL2 —— 
#        所以结论仍由实测定，不由这份配置头定。但它**不能**当作
#        "交叉编译期可移植性已完全验证"的证据。
#
# 用法：
#   bash scripts/run_on_target_kmsdrm.sh [ssh别名]      # 默认 pi
# ============================================================================
set -uo pipefail

HOST="${1:-pi}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LVGLCJ_TARGET_BUILD_KMS:-$HOME/lvgl4cj-a64kms}"
REMOTE_DIR="${LVGLCJ_TARGET_DIR:-lvgl4cj}"
SSHOPT="-o BatchMode=yes -o ConnectTimeout=25 -o ServerAliveInterval=20 -o ServerAliveCountMax=30"
sshq() { timeout 120 ssh $SSHOPT "$HOST" "$@"; }

echo "=== 1. 等目标机（链路会抖：重试）==="
ready=0
for i in $(seq 1 24); do
    if timeout 30 ssh $SSHOPT "$HOST" "echo ok" > /dev/null 2>&1; then
        echo "  第 $i 次探测：可达"
        ready=1
        break
    fi
    sleep 5
done
if [ "$ready" != "1" ]; then
    echo "  目标机不可达，中止"
    exit 1
fi

echo
echo "=== 2. 交叉编译 aarch64 静态库（含 SDL2 后端）==="
SHIM="$BUILD/sdl2-shim"
mkdir -p "$SHIM/SDL2"
REAL="$(ls /usr/include/*/SDL2/_real_SDL_config.h 2>/dev/null | head -1)"
if [ -z "$REAL" ]; then
    echo "  ✗ 本机找不到 _real_SDL_config.h（SDL2 头文件装了吗？）"
    exit 1
fi
cp -f "$REAL" "$SHIM/SDL2/_real_SDL_config.h"
# ★★ 第二层问题（第一层修完才露出来，实测撞到）：
#    本机那份配置是 **x86_64** 的，它把 x86 的 SIMD 特性全打开了，于是
#    SDL_cpuinfo.h 会去找 <immintrin.h>（x86 专有头文件），交叉编译时报：
#        fatal error: immintrin.h: No such file or directory
#    这句看着像"少了个系统头文件"，其实是"用错了架构的配置头"——
#    一个很容易往错误方向排查的报错。
#
#    ★ 修它踩过一次空，值得记：第一版在垫片末尾撤掉 `SDL_HAS_IMMINTRIN_H`，
#      一点用都没有 —— 因为真正的门槛叫 **HAVE_IMMINTRIN_H**（autoconf 风格），
#      而 `SDL_HAS_IMMINTRIN_H` 是另一回事。两个名字看着像，作用完全不同。
#
#    最终两层都做，各自解决各自的问题：
#      · 层一（改配置）：把 x86 特性宏撤掉，让这份配置看起来像目标架构的。
#        追加在文件末尾即可 —— SDL_config.h 先 include 本文件、再 include
#        SDL_cpuinfo.h，所以末尾的 #undef 对后面全部可见。
#      · 层二（用开关）：SDL_cpuinfo.h **自带** SDL_DISABLE_* 开关，
#        用它比改它的配置头正当得多，也是真正解决引架构专有头的那一条。
cat >> "$SHIM/SDL2/_real_SDL_config.h" <<'SHIMEOF'

/* ---- lvgl4cj 交叉编译垫片：撤掉 x86 专有的 SIMD 特性宏 ---- */
#undef SDL_HAS_MMX
#undef SDL_HAS_3DNOW
#undef SDL_HAS_SSE
#undef SDL_HAS_SSE2
#undef SDL_HAS_SSE3
#undef SDL_HAS_SSSE3
#undef SDL_HAS_SSE4_1
#undef SDL_HAS_SSE4_2
#undef SDL_HAS_AVX
#undef SDL_HAS_AVX2
#undef SDL_HAS_AVX512F
#undef SDL_HAS_IMMINTRIN_H
SHIMEOF
echo "  垫片头文件: $SHIM/SDL2/_real_SDL_config.h  ← $REAL（两处修正：撤 x86 特性宏 + SDL_DISABLE 开关）"

# ★ 开关清单取自 SDL_cpuinfo.h 自身。**刻意不含 SDL_DISABLE_ARM_NEON_H**：
#   NEON 是目标架构原生能力，aarch64 上本就该启用。
SDL_DIS=""
for _d in IMMINTRIN MMINTRIN XMMINTRIN EMMINTRIN PMMINTRIN MM3DNOW LSX LASX; do
    SDL_DIS="$SDL_DIS -DSDL_DISABLE_${_d}_H"
done

cmake -S "$ROOT/native" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/native/cmake/aarch64-linux-gnu.cmake" \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-I$SHIM $SDL_DIS" \
    -DLVGLCJ_BUILD_TESTS=OFF \
    -DLVGLCJ_BUILD_SDL2=ON > /tmp/kms_cmake.log 2>&1 || { tail -20 /tmp/kms_cmake.log; exit 1; }
if ! cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" > /tmp/kms_build.log 2>&1; then
    tail -25 /tmp/kms_build.log
    echo "  交叉编译失败"
    exit 1
fi
WARN="$(grep -cE 'warning:|error:' /tmp/kms_build.log || true)"
echo "  告警/错误数: $WARN"
if [ "$WARN" != "0" ]; then
    grep -E 'warning:|error:' /tmp/kms_build.log | head -10
    exit 1
fi
# ★ 新符号必须真的进了 aarch64 的那份库：只"编过了"不代表它在这个归档里
for sym in lvglcj_sdl2_video_driver lvglcj_sdl2_screenshot; do
    if nm -g --defined-only "$BUILD/lib/liblvgl4cj_backend_sdl2.a" 2>/dev/null | grep -q "$sym"; then
        echo "  ✓ $sym 在 aarch64 backend_sdl2.a 里"
    else
        echo "  ✗ $sym 不在 aarch64 backend_sdl2.a 里（头文件声明了却没实现？）"
        exit 1
    fi
done

echo
echo "=== 3. 同步到目标机（只传需要的，逐路径同步、不用 --delete）==="
# 逐路径同步而不是整仓库：third_party/lvgl 有几十 MB，走 WiFi 不值得，
# 而目标机侧只需要"C 层产物 + 仓颉源码"。用 --delete 会误删目标机上的东西，故不用。
sync_path() {
    local src="$1" dst="$2" i
    for i in $(seq 1 12); do
        if rsync -az --partial --append-verify --timeout=180 -e "ssh $SSHOPT" \
                "$src" "$HOST:$dst" > /tmp/kms_rsync.log 2>&1; then
            echo "  ✓ $src"
            return 0
        fi
        echo "    第 $i 次中断：$(tail -1 /tmp/kms_rsync.log | tr -d '\r' | cut -c1-70)"
        sleep 3
    done
    echo "  ✗ 同步失败：$src"
    return 1
}
sshq "mkdir -p ~/$REMOTE_DIR/libs ~/$REMOTE_DIR/scripts ~/$REMOTE_DIR/examples/hello_cj" > /dev/null 2>&1
sync_path "$ROOT/src"                        "~/$REMOTE_DIR/"        || exit 1
sync_path "$ROOT/examples/hello_cj"          "~/$REMOTE_DIR/examples/" || exit 1
sync_path "$ROOT/scripts"                    "~/$REMOTE_DIR/"        || exit 1
sync_path "$ROOT/cjpm.toml"                  "~/$REMOTE_DIR/"        || exit 1
# ★ 这里传的是**目录**而不是 "$BUILD/lib/"*.a：后者会被 shell 展开成多个参数，
#   而 sync_path 只认「源、目标」两个 —— 于是目标会变成第二个 .a 文件，
#   报出来的是 rsync 的 "errors selecting input/output files, dirs (code 3)"，
#   看起来像链路问题，其实是参数用错了（本项目已经踩过同类一次：
#   run_on_target.sh 里 --contimeout 那条注释）。
sync_path "$BUILD/lib/"                      "~/$REMOTE_DIR/libs/"   || exit 1
# 目标机上 libSDL2.so 是仓库自带的软链（指向系统库）——不装 libsdl2-dev 也能链接，
# 这一步是为了让"为什么链接得到"有据可查
sshq "cd ~/$REMOTE_DIR/libs && ls -l *.a libSDL2.so 2>&1 | head -8"

echo
echo "=== 4. 在目标机上构建 hello_cj（nohup + 轮询）==="
# ★★ 这一步必须 **nohup 后台跑 + 轮询结果文件**，理由与 run_on_target.sh 的测试套件完全相同：
#    目标机走 WiFi，实测会整段断连（"client_loop: send disconnect: Broken pipe"）。
#    前台跑时，一次断连就 SIGHUP 掉构建过程；而脚本随后**能找到上一次的旧二进制**，
#    于是这次断线就被伪装成一次成功 —— 跑的却是旧产物。
#    ★ 本项目这次真的踩到了：旧二进制里没有新加的 slotClobber 计数，
#      却报告"实测通过"，直到看见输出里缺字段才发现。
BUILD_PROJ="~/$REMOTE_DIR/examples/hello_cj"
sshq "rm -f /tmp/build_cj.log /tmp/build_cj.done && \
      nohup bash -c 'bash $REMOTE_DIR/scripts/target_build_cj.sh $BUILD_PROJ \
                     > /tmp/build_cj.log 2>&1; echo \$? > /tmp/build_cj.done' \
      > /dev/null 2>&1 & echo LAUNCHED" > /dev/null 2>&1

BUILD_RC=""
for _i in $(seq 1 120); do
    sleep 5
    BUILD_RC="$(timeout 60 ssh $SSHOPT "$HOST" 'cat /tmp/build_cj.done 2>/dev/null' 2>/dev/null | tr -d '\r')"
    [ -n "$BUILD_RC" ] && break
    printf '  构建中…(%d/120)\r' "$_i"
done
echo
if [ -z "$BUILD_RC" ]; then
    echo "  ✗ 等不到构建结束（10 分钟后仍未写出结果文件）"
    exit 1
fi
echo "  构建退出码: $BUILD_RC"
BUILD_LOG="$(timeout 60 ssh $SSHOPT "$HOST" 'tail -8 /tmp/build_cj.log' 2>/dev/null | tr -d '\r')"
printf '%s\n' "$BUILD_LOG" | sed 's/^/    /'
# ★ 构建必须**硬性成功**才算过：软检查会把"没构建成功但旧产物还在"放行，
#   而那恰恰表现为"跑的是上一次的二进制"，很难看出来。
if [ "$BUILD_RC" != "0" ] || ! printf '%s\n' "$BUILD_LOG" | grep -qE 'cjpm build success|Build success'; then
    echo "  ✗ 目标机构建没有成功（上面是日志尾部）—— 不继续，避免拿旧产物当结果"
    exit 1
fi
BIN="$(sshq "ls ~/$REMOTE_DIR/examples/hello_cj/target/release/bin/main \
             ~/$REMOTE_DIR/examples/hello_cj/target/debug/bin/main 2>/dev/null | head -1")"
BIN="$(printf '%s' "$BIN" | tr -d '\r')"
if [ -z "$BIN" ]; then
    echo "  ✗ 目标机上没有构建出可执行文件"
    exit 1
fi
echo "  可执行文件: $BIN"

echo
echo "=== 5. KMSDRM 实测 ==="
# ★ 让出 DRM master 要停 lightdm，这需要 sudo 密码。两边环境变量名不同，别搞混：
#     · 宿主机侧（本脚本，沿用 run_on_target.sh 的约定）：LVGLCJ_TARGET_SUDO_PW
#     · 目标机侧（target_run_kmsdrm.sh）：              LVGLCJ_SUDO_PW
#   密码**不进仓库**（见 run_on_target.sh 的说明），只经环境变量传。
#   代价说清楚：它会出现在目标机的进程列表里 —— 这是本项目既有的做法，不是新引入的。
#   不提供时，这一步会停下来并打印要手工执行的那条命令（不会静默跳过）。
KMS_PW="${LVGLCJ_TARGET_SUDO_PW:-}"
KMS_PREFIX=""
[ -n "$KMS_PW" ] && KMS_PREFIX="LVGLCJ_SUDO_PW='$KMS_PW' "
RUN_RC=0
timeout 900 ssh $SSHOPT "$HOST" \
    "${KMS_PREFIX}bash ~/$REMOTE_DIR/scripts/target_run_kmsdrm.sh '$BIN' ~/kmsdrm_shot.ppm" 2>&1
RUN_RC=$?

echo
echo "=== 6. 取回截图（把「屏幕上有没有画面」带回宿主机分析）==="
if rsync -az --partial --timeout=180 -e "ssh $SSHOPT" \
        "$HOST:~/kmsdrm_shot.ppm" "/tmp/kmsdrm_shot.ppm" > /dev/null 2>&1; then
    echo "  ✓ /tmp/kmsdrm_shot.ppm（$(stat -c %s /tmp/kmsdrm_shot.ppm) 字节）"
    echo "  下一步： python3 scripts/check_ui.py /tmp/kmsdrm_shot.ppm"
else
    echo "  ✗ 没能取回截图（一次实测结束时，第 5 步的判定仍然有效）"
fi
echo
echo "KMSDRM 实测退出码: $RUN_RC（0 = 确实在 KMSDRM 上跑完）"
exit "$RUN_RC"
