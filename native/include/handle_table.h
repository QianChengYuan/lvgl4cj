/*
 * handle_table.h —— 四态句柄表（设计文档 §3.1）
 *
 * 为什么用「自增 ID」而不是指针地址作句柄（ADR-001）：
 *   地址会被复用。对象 A 删除后地址被对象 B 复用，此时旧句柄查表会
 *   命中 B 的地址 → 误判存活 → use-after-free。自增 ID 不会复用。
 *
 * 四态：
 *   UNINIT      未初始化            —— 无表项，ptr_of 返回 NULL
 *   ALIVE       有效                —— 表项存在，ptr 有效
 *   INVALIDATED 原生对象已删除       —— **表项仍存在**（保留删除栈便于诊断），ptr_of 返回 NULL
 *   RELEASED    句柄表项已回收       —— 表项移除，ptr_of 返回 NULL
 *
 * INVALIDATED 与 RELEASED 的区别是本设计的关键：前者让仓颉侧能给出
 * 明确报错并保留调试信息，后者才真正释放表项内存。
 *
 * 幂等性契约（§3.1.3）：
 *   close()   在 ALIVE→INVALIDATED；在 INVALIDATED/RELEASED 无操作
 *   release() 在任何态→RELEASED；重复调用无操作
 *   业务 API  非 ALIVE → 返回 LVGLCJ_ERR_INVALID_HANDLE
 */
#ifndef LVGLCJ_HANDLE_TABLE_H
#define LVGLCJ_HANDLE_TABLE_H

#include <stdint.h>
#include "lvglcj_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 无效句柄的保留值；自增 ID 从 1 开始，0 与负值永不参与分配 */
#define LVGLCJ_HANDLE_NULL ((int64_t)0)

/* INVALIDATED 时保留的诊断深度（删除栈） */
#define LVGLCJ_DELETE_STACK_MAX 8

/*
 * 保留的 INVALIDATED 表项上限（有界回收，见 handle_table.c 的实现说明）。
 *
 * 为什么需要有界：父对象被删时子句柄进入 INVALIDATED 且**保留**表项，
 * 保留是为了能报出「句柄已失效（原生对象已删除）」并留着删除栈供定位。
 * 但「保留」若无上限，任何「建容器 → 删容器」的循环都会让表项单调增长
 * （实测一次「1 父 + 10 子」的删除留下 10 条，即 10 条/轮），
 * 于是 §11.1 的 soak 判据「句柄收敛」直接不成立。
 *
 * 取值依据：单条表项约 312 字节（删除栈 8×32 占大头），
 *   1024 条 ≈ 320KB，足够覆盖「最近若干次删除」的诊断需求。
 * 内存敏感的嵌入式目标可下调；调试期想保留更多可上调。
 */
#ifndef LVGLCJ_INVALIDATED_KEEP_MAX
#define LVGLCJ_INVALIDATED_KEEP_MAX 1024
#endif
/* 句柄名称最大长度（含结尾 '\0'） */
#define LVGLCJ_HANDLE_NAME_MAX 32

typedef enum {
    LVGLCJ_HSTATE_UNINIT = 0,
    LVGLCJ_HSTATE_ALIVE = 1,
    LVGLCJ_HSTATE_INVALIDATED = 2,
    LVGLCJ_HSTATE_RELEASED = 3
} lvglcj_handle_state_t;

/* ------------------------------------------------------------ 生命周期 */
int32_t lvglcj_handle_table_init(void);
void    lvglcj_handle_table_destroy(void);

/*
 * 注册一个原生指针，返回新句柄。
 * 仅用于「原生对象刚创建、尚无句柄」的场景（§3.1.2 的 create / handle_of 入口）。
 * 若该 ptr 已注册且状态为 ALIVE，直接返回既有句柄（保持一对一）。
 */
int64_t lvglcj_handle_register(void *ptr, const char *name);

/* ------------------------------------------------------------ 查询 */
void   *lvglcj_ptr_of(int64_t h);            /* 非 ALIVE 一律返回 NULL */
int64_t lvglcj_handle_of(void *ptr);         /* 未注册返回 LVGLCJ_HANDLE_NULL */

int32_t lvglcj_handle_alive(int64_t h);      /* 1 = ALIVE，0 = 其他 */
int32_t lvglcj_handle_state(int64_t h);      /* lvglcj_handle_state_t */

/*
 * 业务 API 的统一入口守卫：
 *   非 ALIVE 时记录错误（INVALID_HANDLE 或 PENDING_DELETE）并返回对应负码；
 *   ALIVE 时返回 LVGLCJ_OK。
 * 便捷宏见文件末尾。
 */
int32_t lvglcj_handle_require(int64_t h, const char *func);

/* ------------------------------------------------------------ 状态转移 */
void lvglcj_handle_invalidate(int64_t h);    /* 幂等；ALIVE→INVALIDATED，其他态无操作 */
void lvglcj_handle_release(int64_t h);       /* 幂等；任何态→RELEASED（释放表项内存） */

void    lvglcj_handle_mark_pending_delete(int64_t h);  /* 延迟删除期间标记 */
int32_t lvglcj_handle_pending_delete(int64_t h);

/* INVALIDATED 时记录删除栈，便于定位「谁删了我」（§3.1 保留调试信息） */
void lvglcj_handle_push_delete_frame(int64_t h, const char *func);
void lvglcj_handle_set_name(int64_t h, const char *name);
/*
 * 读取表项名字（树形 dump 用）。
 * 写入 buf 的内容以 '\0' 结尾；句柄无表项或无名字时 buf 置空串。
 * 返回实际写入的字符数（不含 '\0'）。
 *
 * ★ 为什么要拷贝出来而不是返回内部指针：dump 的用途之一是**在对象被删除之后**
 *   回溯当时的树形，那时表项可能已经释放 —— 返回内部指针会变成悬空读取。
 *   拷贝一份让 dump 的生命周期与句柄表解耦。
 */
int32_t lvglcj_handle_get_name(int64_t h, char *buf, int32_t buflen);

/* ------------------------------------------------------- 可观测性 / 泄漏检测 */
/*
 * state = -1 表示统计全部；返回表项数量。
 * §7.3：handle_count(ALIVE) 与 LVGL 内部 obj_count 长期应保持一致或差值恒定，
 *       差值持续增大 = 句柄泄漏。
 */
int32_t lvglcj_handle_count(int32_t state);

/* 导出当前表项快照；返回写入条数。用于 dump / 泄漏分析。 */
int32_t lvglcj_handle_dump(int64_t *handles, int32_t *states, int32_t max);

/* ---------------------------------------------------------------- 便捷宏 */
#define LVGLCJ_HANDLE_GUARD(h, func)                       \
    do {                                                   \
        int32_t _rc = lvglcj_handle_require((h), (func));  \
        if (_rc != LVGLCJ_OK) return _rc;                  \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_HANDLE_TABLE_H */
