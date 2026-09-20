/*
 * test_handle_table.c —— C 侧单测：四态句柄表（设计文档 §10.3 handle_test）
 *
 * 覆盖：
 *   · 四态转移正确；close()/invalidate() 重复调用安全
 *   · release() 幂等，且回收表项内存
 *   · 句柄 ID **不复用**（ADR-001 的核心）
 *   · 空/负/不存在句柄全部安全返回，不崩溃
 *   · pending delete 标记与 require 的错误码
 *   · 5000 条压测下映射正确（覆盖哈希扩容与墓碑整理）
 *
 * 用 CTest 注册：bash scripts/build_native.sh --tests
 */
#include "handle_table.h"
#include "lvglcj_error.h"

#include <stdio.h>

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        g_total++;                                    \
        if (cond) {                                   \
            printf("  [ok]   %s\n", (msg));           \
        } else {                                      \
            printf("  [FAIL] %s\n", (msg));           \
            g_fail++;                                 \
        }                                             \
    } while (0)

#define N_STRESS 5000

static int        g_stress_vals[N_STRESS];
static int64_t    g_stress_handles[N_STRESS];

int main(void)
{
    printf("=== test_handle_table：四态句柄表（§3.1）===\n");

    lvglcj_error_reset_counts();
    if (lvglcj_handle_table_init() != LVGLCJ_OK) {
        printf("  [FAIL] 句柄表初始化\n");
        return 1;
    }

    /* ---------------------------------------------------------- 注册与查询 */
    static int obj_a = 1, obj_b = 2, obj_c = 3;

    int64_t ha = lvglcj_handle_register(&obj_a, "obj_a");
    int64_t hb = lvglcj_handle_register(&obj_b, "obj_b");

    CHECK(ha > 0 && hb > 0 && ha != hb, "register 返回唯一非零句柄");
    CHECK(lvglcj_ptr_of(ha) == &obj_a, "ptr_of 取回正确指针");
    CHECK(lvglcj_handle_of(&obj_a) == ha, "handle_of 反查正确");
    CHECK(lvglcj_handle_of(&obj_c) == LVGLCJ_HANDLE_NULL, "未注册指针返回 0");
    CHECK(lvglcj_handle_register(&obj_a, "obj_a") == ha, "重复注册同一指针返回既有句柄（一对一）");
    CHECK(lvglcj_handle_alive(ha) == 1, "新句柄为 ALIVE");
    CHECK(lvglcj_handle_state(ha) == LVGLCJ_HSTATE_ALIVE, "state == ALIVE");
    CHECK(lvglcj_handle_require(ha, "test") == LVGLCJ_OK, "require(ALIVE) 通过");

    /* ------------------------------------------------------------ 失效 */
    lvglcj_handle_invalidate(ha);
    CHECK(lvglcj_handle_state(ha) == LVGLCJ_HSTATE_INVALIDATED, "invalidate → INVALIDATED");
    CHECK(lvglcj_handle_alive(ha) == 0, "INVALIDATED 不是 ALIVE");
    CHECK(lvglcj_ptr_of(ha) == NULL, "INVALIDATED 时 ptr_of 返回 NULL（防止 use-after-free）");
    CHECK(lvglcj_handle_require(ha, "test") == LVGLCJ_ERR_INVALID_HANDLE,
          "require(INVALIDATED) 返回 ERR_INVALID_HANDLE");
    CHECK(lvglcj_handle_of(&obj_a) == LVGLCJ_HANDLE_NULL, "失效后反查表已摘除");
    CHECK(lvglcj_error_count(LVGLCJ_ERR_INVALID_HANDLE) >= 1, "错误计数已记录");

    /* -------------------------------------------------------- 幂等性 */
    lvglcj_handle_invalidate(ha);
    CHECK(lvglcj_handle_state(ha) == LVGLCJ_HSTATE_INVALIDATED, "重复 invalidate 幂等");
    lvglcj_handle_invalidate(999999);
    CHECK(1, "对不存在句柄 invalidate 不崩溃");

    /* ------------------------------------------------------------ 回收 */
    lvglcj_handle_release(ha);
    CHECK(lvglcj_handle_state(ha) == LVGLCJ_HSTATE_UNINIT, "release 后表项被回收");
    lvglcj_handle_release(ha);
    CHECK(1, "重复 release 幂等且不崩溃");
    CHECK(lvglcj_ptr_of(ha) == NULL, "已回收句柄的 ptr_of 为 NULL");

    /* ------------------------------------------------------ ID 不复用 */
    int64_t hc = lvglcj_handle_register(&obj_c, "obj_c");
    CHECK(hc != ha, "句柄 ID 不复用（ADR-001：防地址复用导致误判存活）");
    CHECK(hc > hb, "句柄 ID 单调递增");

    /* ------------------------------------------------------------ 计数 */
    CHECK(lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE) == 2, "ALIVE 计数 == 2（obj_b + obj_c）");

    /* -------------------------------------------------------- 待删除态 */
    lvglcj_handle_mark_pending_delete(hb);
    CHECK(lvglcj_handle_pending_delete(hb) == 1, "pending 标记生效");
    CHECK(lvglcj_handle_require(hb, "test") == LVGLCJ_ERR_PENDING_DELETE,
          "pending 时 require 返回 ERR_PENDING_DELETE（§3.8.3 回调期句柄锁定）");

    /* ------------------------------------------------- 边界与异常输入 */
    CHECK(lvglcj_handle_alive(0) == 0, "句柄 0 不是 ALIVE");
    CHECK(lvglcj_ptr_of(0) == NULL, "句柄 0 的 ptr_of 为 NULL");
    CHECK(lvglcj_ptr_of(-1) == NULL, "负句柄安全返回 NULL");
    CHECK(lvglcj_handle_require(888888, "test") == LVGLCJ_ERR_INVALID_HANDLE,
          "不存在的句柄返回 ERR_INVALID_HANDLE");
    CHECK(lvglcj_handle_register(NULL, "null") == LVGLCJ_HANDLE_NULL, "拒绝为 NULL 指针注册");

    /* ------------------------------------------------------------ 压测 */
    /* 覆盖哈希扩容 + 墓碑整理，验证映射在大规模下仍然正确 */
    for (int i = 0; i < N_STRESS; i++) {
        g_stress_vals[i] = i;
        g_stress_handles[i] = lvglcj_handle_register(&g_stress_vals[i], "stress");
    }
    int all_ok = 1;
    for (int i = 0; i < N_STRESS; i++) {
        if (g_stress_handles[i] <= 0 || lvglcj_ptr_of(g_stress_handles[i]) != &g_stress_vals[i]) {
            all_ok = 0;
            break;
        }
    }
    CHECK(all_ok, "5000 条压测下「句柄↔指针」映射全部正确（含扩容）");

    all_ok = 1;
    for (int i = 0; i < N_STRESS; i++) {
        if (lvglcj_handle_of(&g_stress_vals[i]) != g_stress_handles[i]) {
            all_ok = 0;
            break;
        }
    }
    CHECK(all_ok, "5000 条压测下反查全部正确");

    /* 全部回收后 ALIVE 计数应回到基线 */
    for (int i = 0; i < N_STRESS; i++) {
        lvglcj_handle_invalidate(g_stress_handles[i]);
        lvglcj_handle_release(g_stress_handles[i]);
    }
    CHECK(lvglcj_handle_count(LVGLCJ_HSTATE_ALIVE) == 2,
          "回收后 ALIVE 计数回到基线 2（无句柄泄漏）");
    CHECK(lvglcj_handle_count(-1) == 2, "全部状态统计也回到基线 2（表项内存已回收）");

    lvglcj_handle_table_destroy();

    printf("\n=== %s：%d 项检查，%d 项失败 ===\n",
           (g_fail == 0) ? "PASS" : "FAIL", g_total, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
