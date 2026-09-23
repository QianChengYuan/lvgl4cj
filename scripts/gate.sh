#!/usr/bin/env bash
# ============================================================================
# gate.sh —— P0 门禁（设计文档 §10.4 / §11.1）
#
# 六项门禁与判定标准：
#   1. cjfmt   零 diff                  （§10.4）
#   2. cjlint  零告警                   （§10.4；规则清单见 config/cjlint_rule_list.json）
#   3. cjpm build 成功
#   4. cjpm test  全绿（0 FAILED / 0 ERROR）
#   5. C 单测（ctest）全绿              （§10.1）
#   6. ASan：工具自证 + C 单测零报告    （§9.5 / §11.1）
#
# ---------------------------------------------------------------------------
# ★ 为什么 ASan 的边界划在 C 侧（这是对 §9.5 原方案的有据修正）
#
# §9.5 原计划是「C 桥接层 + LVGL 用 ASan 编译，用 suppression 屏蔽仓颉运行时符号」，
# 即让 ASan 跑在**混合进程**里。t8 实测证明这条路走不通：
#   把 ASan 运行时链进仓颉可执行文件后，进程在**退出**时中止于
#     AddressSanitizer: CHECK failed: sanitizer_thread_arg_retval.cpp:56 "((t)) != (0)"
#       #4 pthread_join        (ASan 拦截器)
#       #5 CJ_ScheduleAllNonDefaultExit  (libcangjie-runtime.so)
#   原因是仓颉运行时的线程由自身机制创建，ASan 的线程簿记里没有记录，
#   它在 join 时做的一致性检查必然失败。`detect_leaks=0` 也避不开
#   （该簿记不由泄漏检测开关控制）。
#
# 因此门禁改为：ASan **只在纯 C 进程**中运行（C 单测就是纯 C）。
# 这不是降级 —— 本层所有手工内存管理都在 C 侧（四态句柄表、闭包表、延迟删除
# 队列、anim_ctx、任务队列、显示缓冲、样式/树的 dump），C 单测会走遍它们，
# 所以这个边界对「我们自己的代码」是完整覆盖，而且**不依赖仓颉 SDK**，
# 在 CI 上更稳定。
#
# 混合进程的内存检查改用 valgrind（§9.5 第 5 点的既定回退）：
# 实测 9.8 万条报告全部来自仓颉运行时/GC，**没有任何一条的栈包含
# native/src 或 backend 下的源文件**（判定命令见 docs/P0_RESULTS.md §12）。
# ---------------------------------------------------------------------------
#
# 用法：
#   bash scripts/gate.sh            # 全部六项
#   bash scripts/gate.sh --quick    # 只跑 1–4（不做 C 编译与 ASan，快）
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${LVGLCJ_BUILD_DIR:-$HOME/lvgl4cj-build}"
ASAN_BUILD_DIR="${LVGLCJ_ASAN_BUILD_DIR:-$HOME/lvgl4cj-build-asan}"
QUICK=0

for arg in "$@"; do
    case "${arg}" in
        --quick) QUICK=1 ;;
        *) echo "[gate] 未知参数：${arg}" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- 环境
if [[ -z "${CANGJIE_HOME:-}" ]]; then
    # 与 scripts/env.sh 同一约定：SDK 装在 $HOME/cangjie/<版本>/cangjie
    for d in "$HOME"/cangjie/*/cangjie; do
        [[ -x "${d}/bin/cjc" ]] && CANGJIE_HOME="${d}" && break
    done
fi
if [[ -z "${CANGJIE_HOME:-}" || ! -x "${CANGJIE_HOME}/bin/cjc" ]]; then
    echo "[gate] 找不到仓颉 SDK。请先 source scripts/env.sh 或设置 CANGJIE_HOME。" >&2
    exit 2
fi

export CANGJIE_HOME
export PATH="${CANGJIE_HOME}/bin:${CANGJIE_HOME}/tools/bin:${PATH}"
# cjlint 需要 tools/lib 下的 libcjlint.so
export LD_LIBRARY_PATH="${CANGJIE_HOME}/tools/lib:${CANGJIE_HOME}/runtime/lib/linux_x86_64_cjnative:${ROOT_DIR}/libs:${LD_LIBRARY_PATH:-}"

CJFMT="${CANGJIE_HOME}/tools/bin/cjfmt"
CJLINT="${CANGJIE_HOME}/tools/bin/cjlint"
TMPDIR_GATE="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_GATE}"' EXIT

FAILED=()
PASSED=()

ok()   { printf '  \033[32m[ PASS ]\033[0m %s\n' "$1"; PASSED+=("$1"); }
bad()  { printf '  \033[31m[ FAIL ]\033[0m %s\n' "$1"; FAILED+=("$1"); }
head_() { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }

cd "${ROOT_DIR}"

# ============================================================ 1. cjfmt 零 diff
head_ "1/6 cjfmt：零 diff（§10.4）"
# cjfmt 没有 --check 模式，因此「零 diff」用「格式化到临时目录再比对」实现。
# 注意 -o 会再套一层与源目录同名的子目录，比对路径要带上 src。
if "${CJFMT}" -d src -o "${TMPDIR_GATE}/fmt" > "${TMPDIR_GATE}/cjfmt.log" 2>&1; then
    DIFFS="$(diff -rq src "${TMPDIR_GATE}/fmt/src" 2>/dev/null | grep -c 'differ' || true)"
    if [[ "${DIFFS}" == "0" ]]; then
        ok "cjfmt 零 diff"
    else
        bad "cjfmt 有 ${DIFFS} 个文件未格式化（运行 cjfmt -d src 后再提交）"
        diff -rq src "${TMPDIR_GATE}/fmt/src" 2>/dev/null | grep 'differ' | head -10
    fi
else
    bad "cjfmt 执行失败"
    tail -5 "${TMPDIR_GATE}/cjfmt.log"
fi

# ============================================================ 2. cjlint 零告警
head_ "2/6 cjlint：零告警（§10.4）"
# cjlint 的 -c 指「包含 config/ 的目录」，且会把该目录下的**全部**规则配置文件
# 当作自己的配置来源（缺一个就报 open json file failed）。
# 因此这里在临时目录里把 SDK 的配置整体拷一份，再用本项目的规则清单覆盖它 ——
# 好处是仓库里只保留「我们的规则取舍」这一个文件，不必 vendor 工具配置，
# 也不会因为 SDK 升级而留下过期副本。
CLINT_CFG="${TMPDIR_GATE}/cfg"
mkdir -p "${CLINT_CFG}/config"
cp -r "${CANGJIE_HOME}/tools/config/." "${CLINT_CFG}/config/" 2>/dev/null
cp config/cjlint_rule_list.json "${CLINT_CFG}/config/cjlint_rule_list.json"

if timeout 900 "${CJLINT}" -f src -c "${CLINT_CFG}" > "${TMPDIR_GATE}/cjlint.log" 2>&1; then
    # cjlint 即使有告警也可能返回 0，因此必须数输出而不是看返回码
    N="$(sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/cjlint.log" | grep -cE 'warning:|error:' || true)"
    if [[ "${N}" == "0" ]]; then
        ok "cjlint 零告警"
    else
        bad "cjlint 有 ${N} 条告警/错误"
        sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/cjlint.log" | grep -E 'warning:|error:' | head -10
    fi
else
    bad "cjlint 执行失败"
    tail -5 "${TMPDIR_GATE}/cjlint.log"
fi

# ============================================================ 3. cjpm build
head_ "3/6 cjpm build"
if timeout 1200 cjpm build > "${TMPDIR_GATE}/build.log" 2>&1; then
    ok "cjpm build 成功"
else
    bad "cjpm build 失败"
    sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/build.log" | grep -A5 'error:' | head -20
fi

# ============================================================ 4. cjpm test
head_ "4/6 cjpm test"
if timeout 2400 cjpm test > "${TMPDIR_GATE}/test.log" 2>&1; then
    SUMMARY="$(sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/test.log" | grep -E 'PASSED: [0-9]+, SKIPPED' | tail -1)"
    if sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/test.log" | grep -qE 'FAILED: [1-9]|ERROR: [1-9]'; then
        bad "cjpm test 有失败：${SUMMARY}"
    else
        ok "cjpm test 全绿（${SUMMARY}）"
    fi
else
    bad "cjpm test 失败"
    sed 's/\x1b\[[0-9;]*m//g' "${TMPDIR_GATE}/test.log" | tail -15
fi

if [[ "${QUICK}" == "1" ]]; then
    printf '\n[gate] --quick：跳过 C 单测与 ASan\n'
else
    # ======================================================== 5. C 单测
    head_ "5/6 C 单测（ctest，§10.1）"
    if cmake -S native -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug \
             -DLVGLCJ_BUILD_TESTS=ON > "${TMPDIR_GATE}/cmake.log" 2>&1 &&
       cmake --build "${BUILD_DIR}" -j "$(nproc)" >> "${TMPDIR_GATE}/cmake.log" 2>&1; then
        if ctest --test-dir "${BUILD_DIR}" --output-on-failure > "${TMPDIR_GATE}/ctest.log" 2>&1; then
            N="$(grep -cE '^ *[0-9]+/[0-9]+ Test' "${TMPDIR_GATE}/ctest.log" || true)"
            # ★★ 只判"全绿"是不够的，还要判"测试集有没有被静默缩减"。
            #
            #   这条来自一次实际发生过的静默退化：本目录原先与 build_native.sh
            #   共用，而那个脚本默认 -DLVGLCJ_BUILD_TESTS=OFF，于是"重建库"
            #   会把这个目录里的测试目标移除。本脚本自己会再配置回来
            #   （所以门禁一直是绿的），但**若在两者之间跑一次 ctest，
            #   就会在测试集变小的情况下照样报"全绿"** ——
            #   护栏静默变弱，比护栏报红危险得多。
            #
            #   ★ 探针必须用**构建目标列表**，不能用 ctest 的数量。
            #     实测：把本目录配置成 -DLVGLCJ_BUILD_TESTS=OFF 之后，
            #     `ctest -N` 仍然报 10 个测试，而 `--target test_style_widgets`
            #     已经变成 "No rule to make target"。
            #     也就是说 ctest 看到的清单在这件事上**不灵敏** ——
            #     只拿它做护栏，等于装了一个不会响的警报器。
            #     （首版就是这么写的，写完立刻用 tests=OFF 复现验证，才发现它不响。）
            #
            #   期望值 10 与下面 ASan 段逐个列举的清单一致。
            #   匹配用**行首前缀** `... `（gmake 的目标列表格式），不要用行尾 ——
            #   按行尾匹配时实测只数到 9（某一行的结尾形式不同），
            #   于是护栏变成误报。护栏误报和护栏不响一样糟：
            #   前者会让人开始忽略它。
            TGT="$(cmake --build "${BUILD_DIR}" --target help 2>/dev/null | grep -cE '^\.\.\. test_' || true)"
            if [[ "${N}" != "10" || "${TGT}" -lt "10" ]]; then
                bad "C 单测数量异常：ctest 报 ${N} 个、构建目标 ${TGT} 个（期望 10）—— 测试集可能被静默缩减"
                grep -E '^ *[0-9]+/[0-9]+ Test' "${TMPDIR_GATE}/ctest.log" | head -12
            else
                ok "C 单测全绿（${N} 个可执行）"
            fi
        else
            bad "C 单测有失败"
            tail -25 "${TMPDIR_GATE}/ctest.log"
        fi
    else
        bad "C 侧构建失败"
        grep -iE 'error' "${TMPDIR_GATE}/cmake.log" | head -15
    fi

    # ======================================================== 6. ASan
    head_ "6/6 ASan：工具自证 + 零报告（§9.5 / §11.1）"
    if cmake -S native -B "${ASAN_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug \
             -DLVGLCJ_BUILD_TESTS=ON -DLVGLCJ_BUILD_PROBES=ON -DLVGLCJ_ASAN=ON \
             > "${TMPDIR_GATE}/asan_cmake.log" 2>&1 &&
       cmake --build "${ASAN_BUILD_DIR}" -j "$(nproc)" >> "${TMPDIR_GATE}/asan_cmake.log" 2>&1; then

        # ---- 6a. 工具自证：先证明 ASan **真的在工作**，再谈「零报告」
        #      否则「零报告」可能只是因为工具根本没生效，那是假的安心。
        SELFTEST="${ASAN_BUILD_DIR}/bin/probe_v4_asan"
        if [[ -x "${SELFTEST}" ]]; then
            ASAN_OPTIONS=detect_leaks=1 timeout 120 "${SELFTEST}" leak \
                > "${TMPDIR_GATE}/v4.log" 2>&1
            RC=$?
            if [[ ${RC} -ne 0 ]] && grep -q 'LeakSanitizer: detected memory leaks' "${TMPDIR_GATE}/v4.log"; then
                ok "ASan 自证：人为泄漏被 LeakSanitizer 报出（工具有效）"
            elif grep -q 'SKIP' "${TMPDIR_GATE}/v4.log"; then
                bad "ASan 自证：探针未以 ASan 编译（自证无效，门禁不可信）"
            else
                bad "ASan 自证失败：人为泄漏未被报出（rc=${RC}）"
                tail -5 "${TMPDIR_GATE}/v4.log"
            fi
        else
            bad "找不到 ASan 自证探针 ${SELFTEST}"
        fi

        # ---- 6b. 零报告：C 单测逐个跑，任何一个出现 ASan/LSan 报告即失败
        ASAN_BAD=0
        ASAN_RAN=0
        for t in test_handle_table test_queue test_display_indev test_null_backend \
                 test_sdl2_backend test_style_widgets test_anim test_leak \
                 test_fullscreen_refresh test_soak_asan; do
            BIN="${ASAN_BUILD_DIR}/bin/${t}"
            [[ -x "${BIN}" ]] || continue
            ASAN_RAN=$((ASAN_RAN + 1))
            OUT="$(ASAN_OPTIONS=detect_leaks=1 SDL_VIDEODRIVER=dummy timeout 900 "${BIN}" 2>&1)"
            RC=$?
            NREP="$(printf '%s' "${OUT}" | grep -cE 'ERROR: (AddressSanitizer|LeakSanitizer)' || true)"
            if [[ "${RC}" != "0" || "${NREP}" != "0" ]]; then
                ASAN_BAD=$((ASAN_BAD + 1))
                printf '         %s: rc=%s 报告=%s\n' "${t}" "${RC}" "${NREP}"
                printf '%s' "${OUT}" | grep -E 'ERROR: (AddressSanitizer|LeakSanitizer)|SUMMARY:' | head -4
            fi
        done
        if [[ "${ASAN_RAN}" == "0" ]]; then
            bad "ASan 构建里没有可运行的测试"
        elif [[ "${ASAN_BAD}" == "0" ]]; then
            ok "ASan 零报告（${ASAN_RAN} 个 C 单测全部干净）"
        else
            bad "ASan 有报告（${ASAN_BAD}/${ASAN_RAN} 个测试）"
        fi
    else
        bad "ASan 构建失败"
        grep -iE 'error' "${TMPDIR_GATE}/asan_cmake.log" | head -15
    fi
fi

# ============================================================ 汇总
# ============================================================================
# 7/7 ABI ↔ L1 绑定一致性（scripts/gen_bindings.py --check）
# ============================================================================
# ★ 为什么它值得进门禁：一个样式属性要在**四处**保持一致 ——
#     桥接头声明 / C 实现 / ffi_bridge.cj 的 foreign 声明 / style.cj 的 L1 包装，
#   而没有任何编译期机制保证它们同步。
#   实测后果：C 侧 53 个 style setter，L1 只包了 26 个 ——
#   「某个属性能不能用」曾取决于谁顺手写了哪个，而不是取决于设计。
#   这条检查把「ABI 有而 L1 没有」的单向漂移变成硬失败。
echo
echo "=== 7/7 ABI ↔ L1 绑定一致性 ==="
if python3 "${ROOT_DIR}/scripts/gen_bindings.py" --check > "${TMPDIR_GATE}/bindings.log" 2>&1; then
    ok "ABI 的每个 style setter 都有一致的 L1 包装"
else
    echo "  [ FAIL ] 绑定漂移：ABI 有而 L1 缺，或写法与生成规则不符"
    sed 's/^/    /' "${TMPDIR_GATE}/bindings.log" | head -20
    bad "绑定漂移（补齐：python3 scripts/gen_bindings.py --emit-missing）"
fi

printf '\n\033[1m========== 门禁汇总 ==========\033[0m\n'
printf '  通过 %d 项' "${#PASSED[@]}"
[[ ${#FAILED[@]} -gt 0 ]] && printf '，\033[31m失败 %d 项\033[0m' "${#FAILED[@]}"
printf '\n'
if [[ ${#FAILED[@]} -gt 0 ]]; then
    for f in "${FAILED[@]}"; do printf '  \033[31m✗\033[0m %s\n' "$f"; done
    exit 1
fi
printf '  \033[32m门禁全部通过\033[0m\n'
