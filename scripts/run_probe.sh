#!/usr/bin/env bash
# ============================================================================
# run_probe.sh —— 一键运行 V1–V7 全部探针并汇总结论（设计文档 §12 第 1 周）
#
# 每个探针都带超时：V1 的失败模式之一就是「段错误/挂死」，
# 而**能观测到挂死本身就是结论**，所以绝不允许编排脚本自己被拖住。
#
# 用法：
#   bash scripts/run_probe.sh              # 跑全部
#   bash scripts/run_probe.sh v1 v7        # 只跑指定的
# ============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/env.sh"

ROOT_DIR="${LVGLCJ_ROOT}"
BUILD_DIR="${LVGLCJ_BUILD_DIR}"
ASAN_BUILD_DIR="${LVGLCJ_ASAN_BUILD_DIR:-$HOME/lvgl4cj-build-asan}"
TIMEOUT_S=120

WANT=("$@")
want() {
    [[ ${#WANT[@]} -eq 0 ]] && return 0
    for w in "${WANT[@]}"; do [[ "$w" == "$1" ]] && return 0; done
    return 1
}

declare -a SUMMARY
record() { SUMMARY+=("$1|$2|$3"); }

hr() { printf '%s\n' "------------------------------------------------------------"; }

# ============================================================ V1
probe_v1() {
    hr; echo ">>> V1：C 创建的 OS 线程能否执行仓颉代码（方案 A/C 分叉闸门）"; hr
    ( cd "${ROOT_DIR}/probe/v1_thread_attach" && cjpm build >/dev/null 2>&1 ) \
        || { echo "构建失败"; record V1 "构建失败" "FAIL"; return; }

    local out rc
    out="$(cd "${ROOT_DIR}/probe/v1_thread_attach" && timeout ${TIMEOUT_S} ./target/release/bin/main 2>&1)"
    rc=$?
    echo "${out}"

    if [[ ${rc} -eq 124 ]]; then
        record V1 "超时挂死 → 方案 A 不可行，退回方案 C" "FAIL"
    elif [[ ${rc} -ne 0 ]]; then
        record V1 "异常退出 rc=${rc} → 方案 A 不可行，退回方案 C" "FAIL"
    elif grep -q "方案 A 可行" <<<"${out}"; then
        record V1 "方案 A 可行（C 侧独立 OS 线程跑主循环）" "PASS"
    else
        record V1 "方案 A 不可行 → 退回方案 C（泵模式）" "DONE"
    fi
}

# ============================================================ V2
probe_v2() {
    hr; echo ">>> V2-a：捕获闭包 → C 函数指针（预期编译失败，归档分支 A）"; hr
    local tmp; tmp="$(mktemp -d)"
    local comp_out
    if comp_out="$(cjc "${ROOT_DIR}/probe/v2_capture/capture_fail.cj" -o "${tmp}/v2.out" 2>&1)"; then
        echo "⚠️ 竟然编译通过了！"
        echo "${comp_out}" | head -5
        if grep -qi "captur" <<<"${comp_out}"; then
            record V2 "编译通过 → 需重新评估分支 A" "UNEXPECTED"
        else
            record V2 "编译通过（可能未真正捕获）→ 需人工复核" "UNEXPECTED"
        fi
    else
        echo "编译失败（符合预期），关键错误行："
        echo "${comp_out}" | grep -iE "captur|error" | head -6
        local reason="编译失败（CFunc Lambda 不能捕获变量）"
        grep -qi "captur" <<<"${comp_out}" || reason="编译失败（错误信息未显式提及捕获，建议人工确认）"
        record V2 "${reason} → 分支 B 为唯一路径" "PASS(预期失败)"
    fi
    rm -rf "${tmp}"
    echo
    echo "V2-b：因 V2-a 不通过，GC 保活/pin API 议题**不适用**，跳过。"
}

# ============================================================ V3
probe_v3() {
    hr; echo ">>> V3：仓颉轻量级线程的 OS 亲和性（方案 B 是否可行）"; hr
    ( cd "${ROOT_DIR}/probe/v3_affinity" && cjpm build >/dev/null 2>&1 ) \
        || { echo "构建失败"; record V3 "构建失败" "FAIL"; return; }

    local out
    out="$(cd "${ROOT_DIR}/probe/v3_affinity" && timeout ${TIMEOUT_S} ./target/release/bin/main 2>&1)"
    echo "${out}"

    local n
    n="$(grep -oE '不同 OS 线程数 : [0-9]+' <<<"${out}" | grep -oE '[0-9]+' | head -1)"
    if [[ -z "${n}" ]]; then
        record V3 "无法解析结果" "FAIL"
    elif [[ "${n}" -le 1 ]]; then
        record V3 "亲和性稳定（${n} 个 OS 线程）" "PASS"
    else
        record V3 "发生 M:N 迁移（${n} 个 OS 线程）→ 方案 B 不可行" "DONE"
    fi
}

# ============================================================ V4
probe_v4() {
    hr; echo ">>> V4：ASan 有效性自证（先证明工具有效，再谈零报告）"; hr

    if [[ ! -x "${ASAN_BUILD_DIR}/bin/probe_v4_asan" ]]; then
        echo "未找到 ASan 构建，正在构建到 ${ASAN_BUILD_DIR} ..."
        LVGLCJ_BUILD_DIR="${ASAN_BUILD_DIR}" \
            bash "${SCRIPT_DIR}/build_native.sh" --probes --asan >/dev/null 2>&1 \
            || { record V4 "ASan 构建失败" "FAIL"; return; }
    fi

    local leak_out rc
    leak_out="$("${ASAN_BUILD_DIR}/bin/probe_v4_asan" leak 2>&1)"; rc=$?
    if [[ ${rc} -ne 0 ]] && grep -q "LeakSanitizer" <<<"${leak_out}"; then
        echo "leak 用例：LeakSanitizer 已报出泄漏（工具有效）"
        record V4 "LeakSanitizer 生效（人为泄漏被报出）" "PASS"
    elif grep -q "SKIP" <<<"${leak_out}"; then
        record V4 "二进制未启用 ASan，SKIP" "SKIP"
    else
        record V4 "LeakSanitizer 未报出人为泄漏 → 内存检测不可信" "FAIL"
    fi
}

# ============================================================ V5
probe_v5() {
    hr; echo ">>> V5：cjpm 构建与 C 库链接（已在 t0 打通，此处回归验证）"; hr
    ( cd "${ROOT_DIR}/examples/hello_cj" && cjpm build >/dev/null 2>&1 )
    if [[ $? -eq 0 ]]; then
        echo "hello_cj 构建成功（link-option 模式可用）"
        record V5 "cjpm build + link-option 链接 C 库可用" "PASS"
    else
        record V5 "cjpm 构建失败" "FAIL"
    fi
}

# ============================================================ V6
probe_v6() {
    hr; echo ">>> V6：SDL2 双线程渲染同步 + 超时 + 窗口关闭即时解锁"; hr
    local bin="${BUILD_DIR}/bin/probe_v6_sdl_thread"
    [[ -x "${bin}" ]] || { record V6 "未构建（缺 --probes）" "SKIP"; return; }

    local out
    out="$(timeout ${TIMEOUT_S} "${bin}" 2>&1)"; echo "${out}"
    if grep -q "A(正常往返)=PASS  B(超时生效)=PASS  C(关闭即时解锁)=PASS" <<<"${out}"; then
        record V6 "三项全通过（往返 / 超时 / 关闭即时解锁）" "PASS"
    elif grep -q "SKIP" <<<"${out}"; then
        record V6 "无显示环境，SKIP" "SKIP"
    else
        record V6 "存在失败子项" "FAIL"
    fi
}

# ============================================================ V7
probe_v7() {
    hr; echo ">>> V7：lv_anim deleted_cb 三路径（Patch P1 前提）"; hr
    local bin="${BUILD_DIR}/bin/probe_v7_anim_deleted"
    [[ -x "${bin}" ]] || { record V7 "未构建（缺 --probes）" "SKIP"; return; }

    local out
    out="$(timeout ${TIMEOUT_S} "${bin}" 2>&1)"; echo "${out}"
    if grep -q "Patch P1" <<<"${out}" && grep -q "成立" <<<"${out}"; then
        record V7 "三路径均触发 deleted_cb → Patch P1 成立" "PASS"
        grep -q "obj_delete_fired_deleted_cb=NO" <<<"${out}" && \
            record "V7-补充" "var!=obj 时对象删除不停动画 → 需 DELETE 钩子手动补偿" "DONE"
    else
        record V7 "存在不触发路径 → 需 anim 句柄表兜底" "FAIL"
    fi
}

# ============================================================ 主流程
echo "============================================================"
echo "  lvgl4cj P0 探针 V1–V7"
echo "  构建目录 : ${BUILD_DIR}"
echo "  时间     : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "============================================================"

want v1 && probe_v1
want v2 && probe_v2
want v3 && probe_v3
want v4 && probe_v4
want v5 && probe_v5
want v6 && probe_v6
want v7 && probe_v7

echo
echo "============================================================"
echo "  结论汇总"
echo "============================================================"
printf '%-10s %-58s %s\n' "探针" "结论" "判定"
for row in "${SUMMARY[@]}"; do
    IFS='|' read -r k v s <<<"${row}"
    printf '%-10s %-58s %s\n' "${k}" "${v}" "${s}"
done
echo "============================================================"
echo "详细说明见 docs/P0_RESULTS.md"
