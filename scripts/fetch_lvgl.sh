#!/usr/bin/env bash
# ============================================================================
# fetch_lvgl.sh —— 拉取并锁定 LVGL 源码
#
# 为什么用 gitee 镜像：本机 WSL 无法访问 github.com（连接超时），
# gitee 镜像可达。LVGL 为 MIT 许可（第三方位保留 LICENCE.txt 原样）。
#
# 用法：
#   bash scripts/fetch_lvgl.sh                 # 默认 v9.2.2
#   LVGLCJ_LVGL_TAG=v9.3.0 bash scripts/fetch_lvgl.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

LVGL_TAG="${LVGLCJ_LVGL_TAG:-v9.2.2}"
LVGL_REPO="${LVGLCJ_LVGL_REPO:-https://gitee.com/mirrors/lvgl.git}"
DEST="${ROOT_DIR}/third_party/lvgl"
HASH_FILE="${ROOT_DIR}/third_party/LVGL_VERSION.txt"

if [[ -d "${DEST}/.git" ]]; then
    CURRENT_TAG="$(git -C "${DEST}" describe --tags --exact-match 2>/dev/null || echo unknown)"
    if [[ "${CURRENT_TAG}" == "${LVGL_TAG}" ]]; then
        echo "[fetch_lvgl] 已存在 ${LVGL_TAG}，跳过"
        exit 0
    fi
    echo "[fetch_lvgl] 当前为 ${CURRENT_TAG}，需要 ${LVGL_TAG}，重新拉取"
    rm -rf "${DEST}"
fi

mkdir -p "${ROOT_DIR}/third_party"

echo "[fetch_lvgl] 从 ${LVGL_REPO} 拉取 ${LVGL_TAG}（--depth 1）"
git clone --depth 1 --branch "${LVGL_TAG}" "${LVGL_REPO}" "${DEST}"

# 记录版本与 commit 哈希：设计文档 §14.1 要求保留版本哈希用于 SBOM 追溯
COMMIT="$(git -C "${DEST}" rev-parse HEAD)"
{
    echo "lvgl_tag    = ${LVGL_TAG}"
    echo "lvgl_commit = ${COMMIT}"
    echo "lvgl_repo   = ${LVGL_REPO}"
    echo "fetched_at  = $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${HASH_FILE}"

if [[ ! -f "${DEST}/LICENCE.txt" ]]; then
    echo "[fetch_lvgl] 警告：未找到 LICENCE.txt，请核对第三方许可（§14.1）" >&2
fi

echo "[fetch_lvgl] 完成：${LVGL_TAG} @ ${COMMIT}"
cat "${HASH_FILE}"
