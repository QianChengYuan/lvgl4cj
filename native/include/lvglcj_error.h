/*
 * lvglcj_error.h —— lvgl4cj 统一错误模型（设计文档 §3.12 + 附录 B）
 *
 * 约定（冻结，不得更改数值，测试与文档均依赖）：
 *   0    = 成功
 *   正数 = 特殊成功标记（目前仅 +1 OK_DEFERRED）
 *   负数 = 错误
 *
 * 说明：错误码数值与 lvgl4cj_error.h 同构于仓颉侧 src/error.cj 的 LvglError，
 *       两侧必须逐项一致；test/ffi_contract_test.cj 会做一致性断言。
 */
#ifndef LVGLCJ_ERROR_H
#define LVGLCJ_ERROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 状态码 */
typedef enum {
    LVGLCJ_OK = 0,                     /* 成功 */
    LVGLCJ_OK_DEFERRED = 1,            /* 成功但已延迟（非错误） */

    LVGLCJ_ERR_INVALID_HANDLE = -1,    /* 句柄失效（INVALIDATED / RELEASED） */
    LVGLCJ_ERR_NOT_INITIALIZED = -2,   /* 未调用 lvglcj_init */
    LVGLCJ_ERR_INVALID_CONFIG = -3,    /* lv_conf 哈希不匹配 */
    LVGLCJ_ERR_CALLBACK_THREW = -4,    /* 仓颉回调抛异常（已在边界吞掉并记录） */
    LVGLCJ_ERR_OUT_OF_MEMORY = -5,     /* LVGL 池耗尽或 malloc 失败 */
    LVGLCJ_ERR_WRONG_THREAD = -6,      /* 跨线程调用 LVGL API */
    LVGLCJ_ERR_DEADLOCK_RISK = -7,     /* 回调内同步等待 / 等待图成环 */
    LVGLCJ_ERR_QUEUE_FULL = -8,        /* 任务队列满（入队失败） */
    LVGLCJ_ERR_BACKEND_FAILURE = -9,   /* 后端失败（含 flush_wait 超时） */
    LVGLCJ_ERR_INVALID_ARGUMENT = -10, /* 参数非法 */
    LVGLCJ_ERR_CLOSURE_EXHAUSTED = -11,/* 闭包 ID 耗尽 */
    LVGLCJ_ERR_PENDING_DELETE = -12,   /* 对象处于待删除状态 */
    LVGLCJ_ERR_NOT_SUPPORTED = -13,    /* 该 API 在当前构建配置下不可用 */
    LVGLCJ_ERR_VERSION_MISMATCH = -14, /* LVGL 版本不匹配 */
    LVGLCJ_ERR_DEFERRED_LOOP = -15,    /* 延迟删除循环超限（连续 3 帧触发强制清空） */

    /* ---- 以下两项：分支 A（闭包指针 + pin 表）已归档（CFunc Lambda 不能捕获变量） ---- */
    LVGLCJ_ERR_PIN_EXHAUSTED = -16,    /* [已归档] pin 表满，本实现不产生此码 */
    LVGLCJ_ERR_ANIM_CTX_LOST = -17     /* 动画上下文丢失（ctx 已释放但 trampoline 仍被调用） */
} lvglcj_status_t;

/* ---------------------------------------------------------------- 日志级别 */
typedef enum {
    LVGLCJ_LOG_TRACE = 0,
    LVGLCJ_LOG_INFO = 1,
    LVGLCJ_LOG_WARN = 2,
    LVGLCJ_LOG_ERROR = 3,
    LVGLCJ_LOG_USER = 4,
    LVGLCJ_LOG_NONE = 5
} lvglcj_log_level_t;

/* ------------------------------------------------------------- 错误上下文 */
typedef struct {
    int32_t code;            /* lvglcj_status_t */
    int64_t handle;          /* 相关句柄，无则 0 */
    int32_t closure_id;      /* 相关闭包，无则 0 */
    const char *func;        /* 产生错误的 C 侧函数名（静态字符串，勿释放） */
    const char *msg;         /* 额外说明，可为 NULL（静态字符串或临时栈缓冲） */
} lvglcj_error_t;

/* 全局错误回调：由仓颉侧注册；C 侧只保存函数指针，不引用任何仓颉符号 */
typedef void (*lvglcj_error_cb_t)(const lvglcj_error_t *err);

/* 日志回调：level 为 lvglcj_log_level_t；msg 仅在回调期间有效 */
typedef void (*lvglcj_log_cb_t)(int32_t level, const char *msg);

/* ------------------------------------------------------------------ API */
int32_t lvglcj_error_set_cb(lvglcj_error_cb_t cb);
int32_t lvglcj_log_set_cb(lvglcj_log_cb_t cb);

/*
 * 记录一个错误：写入内部计数 + 调用全局错误回调（若有）。
 * 单一出口，便于 §7.3 的速率限制与统一日志格式。
 */
void lvglcj_record_error(int32_t code, int64_t handle, int32_t closure_id,
                         const char *func, const char *msg);

/* 仅日志，不产生错误 */
void lvglcj_log(int32_t level, const char *func, const char *msg);

const char *lvglcj_strerror(int32_t code);

/* -------------------------------------------------- 可观测性（测试/诊断用） */
int32_t lvglcj_error_count(int32_t code);   /* 指定错误码累计次数；code=0 表示总次数 */
void    lvglcj_error_reset_counts(void);
int32_t lvglcj_last_error(lvglcj_error_t *out); /* 取最近一次错误快照（含 msg 拷贝） */

#ifdef __cplusplus
}
#endif

#endif /* LVGLCJ_ERROR_H */
