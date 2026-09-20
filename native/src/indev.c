/*
 * indev.c —— 输入设备与焦点组（设计文档 §5.4 / §3.2.5 T5）
 *
 * 最关键的一处设计选择：**输入数据怎么回填**（T5）。
 *
 *   LVGL 要求 read_cb 把「本次读取的按压点/按键」写进 lv_indev_data_t。
 *   我们没有把该结构体的指针交给仓颉（理由见 lvglcj_bridge.h 中
 *   lvglcj_indev_set_point 上方的长注释），而是采用「暂存 + setter」：
 *
 *       trampoline 入口：把 data 指针存入线程局部 t_read_ctx
 *                        ↓ 调用仓颉闭包（cid, indev 句柄）
 *       仓颉侧：lvglcj_indev_set_point(x, y, pressed)   ← 写的是 t_read_ctx
 *                        ↓ 闭包返回
 *       trampoline 出口：清空 t_read_ctx
 *
 *   这样「LVGL 的布局知识」全部留在 C 侧，仓颉侧只表达语义。
 *
 * 为什么用线程局部而不是全局：LVGL 是单线程的，但仓颉测试与工具可能在
 * 多个 OS 线程上分别驱动 LVGL（V3 探针已证明测试框架会跨 OS 线程迁移用例）。
 * 用全局指针会在这类场景下互相踩；线程局部天然隔离，也没有锁开销。
 *
 * 另一处必须显式处理的默认值：**没有注册 read 回调时必须写 RELEASED**。
 *   lv_indev_data_t 由 LVGL 逐次传入，其初始内容不应被假定为「未按下」；
 *   若我们什么都不写就调用闭包/返回，读到的是上一轮的残留状态，
 *   表现为「松开鼠标后按钮仍持续处于按下态」。因此这里显式复位。
 */
#include "lvglcj_internal.h"

#include <string.h>

/* --------------------------------------------------- T5 输入数据暂存（线程局部） */
static _Thread_local lv_indev_data_t *t_read_ctx = NULL;

/*
 * 取当前正在读取的 data。
 * 已用「是否处于读取中」的显式判定代替「指针是否为空」：
 * t_read_ctx 为 NULL 就是「不在 read 回调内」。
 */
static lv_indev_data_t *read_ctx(const char *func)
{
    if (t_read_ctx == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, func,
                            "只能在 indev_read 回调内回填输入数据");
        return NULL;
    }
    return t_read_ctx;
}

int32_t lvglcj_indev_set_point(int32_t x, int32_t y, int32_t pressed)
{
    lv_indev_data_t *d = read_ctx(__func__);
    if (d == NULL) {
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }
    d->point.x = (lv_coord_t)x;
    d->point.y = (lv_coord_t)y;
    d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_set_key(uint32_t key, int32_t pressed)
{
    lv_indev_data_t *d = read_ctx(__func__);
    if (d == NULL) {
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }
    d->key = key;
    d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_set_enc_diff(int32_t diff)
{
    lv_indev_data_t *d = read_ctx(__func__);
    if (d == NULL) {
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }
    d->enc_diff = (int16_t)diff;
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_set_continue(int32_t cont)
{
    lv_indev_data_t *d = read_ctx(__func__);
    if (d == NULL) {
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }
    d->continue_reading = (cont != 0);
    return LVGLCJ_OK;
}

/* --------------------------------------------------------------- 上下文表 */

#define LVGLCJ_INDEV_MAX 8
#define LVGLCJ_GROUP_MAX 8

typedef struct {
    int32_t used;
    int64_t handle;
    int32_t type;
    int32_t read_cid;
} lvglcj_indev_t;

static lvglcj_indev_t g_indev[LVGLCJ_INDEV_MAX];

typedef struct {
    int32_t used;
    int64_t handle;
} lvglcj_group_t;

static lvglcj_group_t g_group[LVGLCJ_GROUP_MAX];

static lvglcj_indev_t *indev_of(int64_t h)
{
    if (h == LVGLCJ_HANDLE_NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < LVGLCJ_INDEV_MAX; ++i) {
        if (g_indev[i].used && g_indev[i].handle == h) {
            return &g_indev[i];
        }
    }
    return NULL;
}

static lvglcj_group_t *group_of(int64_t h)
{
    if (h == LVGLCJ_HANDLE_NULL) {
        return NULL;
    }
    for (int32_t i = 0; i < LVGLCJ_GROUP_MAX; ++i) {
        if (g_group[i].used && g_group[i].handle == h) {
            return &g_group[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- T5 trampoline */

void lvglcj_indev_read_trampoline(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (data == NULL) {
        return;
    }

    /*
     * ★ 显式复位：不能假定 LVGL 传入的内容是干净的。
     *   漏掉这一步的症状是「松开后仍为按下」或「按键粘滞」。
     */
    data->point.x = 0;
    data->point.y = 0;
    data->key = 0;
    data->btn_id = 0;
    data->enc_diff = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;

    lvglcj_indev_t *iv = NULL;
    int64_t handle = lvglcj_handle_of(indev);
    if (handle != LVGLCJ_HANDLE_NULL) {
        iv = indev_of(handle);
    }

    if (iv == NULL || iv->read_cid == LVGLCJ_CID_NONE) {
        /* 没有用户读取回调：保持上面复位出来的 RELEASED 状态即可 */
        return;
    }

    /*
     * 以「保存-恢复」方式装填暂存指针：即使将来出现嵌套读取
     * （例如回调内又主动触发一次 lv_indev_read），退出时也能还原到上一层。
     */
    lv_indev_data_t *saved = t_read_ctx;
    t_read_ctx = data;

    (void)lvglcj_call_closure(iv->read_cid, handle);

    t_read_ctx = saved;
}

/* ------------------------------------------------------------ indev 生命周期 */

int64_t lvglcj_indev_create(int32_t type)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* 逐值对齐 LVGL（见 lvglcj_bridge.h 的枚举注释），因此这里无需映射 */
    if (type < LVGLCJ_INDEV_NONE || type > LVGLCJ_INDEV_ENCODER) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "未知的输入设备类型");
        return LVGLCJ_HANDLE_NULL;
    }

    lvglcj_indev_t *slot = NULL;
    for (int32_t i = 0; i < LVGLCJ_INDEV_MAX; ++i) {
        if (!g_indev[i].used) {
            memset(&g_indev[i], 0, sizeof(g_indev[i]));
            g_indev[i].used = 1;
            g_indev[i].read_cid = LVGLCJ_CID_NONE;
            slot = &g_indev[i];
            break;
        }
    }
    if (slot == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "indev 上下文槽位已满（P0 上限 8）");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_indev_t *iv = lv_indev_create();
    if (iv == NULL) {
        slot->used = 0;
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "lv_indev_create 失败");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_indev_set_type(iv, (lv_indev_type_t)type);

    int64_t handle = lvglcj_handle_register(iv, "lv_indev_t");
    if (handle == LVGLCJ_HANDLE_NULL) {
        lv_indev_delete(iv);
        slot->used = 0;
        return LVGLCJ_HANDLE_NULL;
    }

    slot->handle = handle;
    slot->type = type;
    return handle;
}

int32_t lvglcj_indev_set_read_cb(int64_t indev, int32_t cid)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(indev, __func__);

    lvglcj_indev_t *iv = indev_of(indev);
    if (iv == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, indev, cid, __func__,
                            "该句柄不是经本层创建的 indev");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    if (iv->read_cid != LVGLCJ_CID_NONE && iv->read_cid != cid) {
        lvglcj_closure_unregister(iv->read_cid);
    }
    iv->read_cid = cid;

    /*
     * 即使 cid == 0 也要挂 trampoline：它负责把 data 复位成 RELEASED。
     * 不挂的话残留状态会生效（见文件头说明）。
     */
    lv_indev_set_read_cb((lv_indev_t *)lvglcj_ptr_of(indev),
                         lvglcj_indev_read_trampoline);
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_set_display(int64_t indev, int64_t disp)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(indev, __func__);

    lv_display_t *dp = (lv_display_t *)lvglcj_ptr_of(disp);
    if (dp == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, disp, 0, __func__,
                            "display 句柄无效");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_indev_set_display((lv_indev_t *)lvglcj_ptr_of(indev), dp);
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_set_group(int64_t indev, int64_t group)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(indev, __func__);

    lv_group_t *g = (lv_group_t *)lvglcj_ptr_of(group);
    if (g == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, group, 0, __func__,
                            "group 句柄无效");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }
    lv_indev_set_group((lv_indev_t *)lvglcj_ptr_of(indev), g);
    return LVGLCJ_OK;
}

int32_t lvglcj_indev_add_event(int64_t indev, int32_t code, int32_t cid)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(indev, __func__);

    /* §3.7.3：绘制类事件不支持 */
    if (code == LV_EVENT_DRAW_MAIN || code == LV_EVENT_DRAW_MAIN_BEGIN ||
        code == LV_EVENT_DRAW_MAIN_END || code == LV_EVENT_DRAW_POST ||
        code == LV_EVENT_DRAW_POST_BEGIN || code == LV_EVENT_DRAW_POST_END) {
        lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, indev, cid, __func__,
                            "LV_EVENT_DRAW_* 不支持");
        return LVGLCJ_ERR_NOT_SUPPORTED;
    }

    lv_indev_add_event_cb((lv_indev_t *)lvglcj_ptr_of(indev),
                          lvglcj_event_trampoline,
                          (lv_event_code_t)code, lvglcj_cid_to_ptr(cid));
    /* 与 display 同理：该 API 不返回 dsc，用 NULL 登记，删除时统一 drop */
    return lvglcj_reg_event_add(indev, NULL, cid);
}

int32_t lvglcj_indev_delete(int64_t indev)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* ★ 严格校验：非 ALIVE 一律 INVALID_HANDLE（幂等由仓颉侧 close() 负责） */
    LVGLCJ_HANDLE_GUARD(indev, __func__);

    lv_indev_t *iv = (lv_indev_t *)lvglcj_ptr_of(indev);
    lvglcj_indev_t *s = indev_of(indev);

    /* (a) 先注销闭包：删除后不该再有回调进来 */
    if (s != NULL && s->read_cid != LVGLCJ_CID_NONE) {
        lvglcj_closure_unregister(s->read_cid);
        s->read_cid = LVGLCJ_CID_NONE;
    }
    {
        int32_t cids[16];
        int32_t n = lvglcj_reg_event_cids_of(indev, cids, 16);
        for (int32_t i = 0; i < n; ++i) {
            lvglcj_closure_unregister(cids[i]);
        }
        lvglcj_reg_event_drop(indev);
    }

    /* (b) 删原生对象 */
    lv_indev_delete(iv);

    /* (c) 回收上下文与句柄表项 */
    if (s != NULL) {
        s->used = 0;
    }
    lvglcj_handle_release(indev);
    return LVGLCJ_OK;
}

/* ---------------------------------------------------------------- group */

int64_t lvglcj_group_create(void)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    lvglcj_group_t *slot = NULL;
    for (int32_t i = 0; i < LVGLCJ_GROUP_MAX; ++i) {
        if (!g_group[i].used) {
            memset(&g_group[i], 0, sizeof(g_group[i]));
            g_group[i].used = 1;
            slot = &g_group[i];
            break;
        }
    }
    if (slot == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "group 上下文槽位已满（P0 上限 8）");
        return LVGLCJ_HANDLE_NULL;
    }

    lv_group_t *g = lv_group_create();
    if (g == NULL) {
        slot->used = 0;
        lvglcj_record_error(LVGLCJ_ERR_OUT_OF_MEMORY, 0, 0, __func__,
                            "lv_group_create 失败");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t handle = lvglcj_handle_register(g, "lv_group_t");
    if (handle == LVGLCJ_HANDLE_NULL) {
        lv_group_delete(g);
        slot->used = 0;
        return LVGLCJ_HANDLE_NULL;
    }
    slot->handle = handle;
    return handle;
}

int32_t lvglcj_group_delete(int64_t g)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /* ★ 严格校验：非 ALIVE 一律 INVALID_HANDLE（幂等由仓颉侧 close() 负责） */
    LVGLCJ_HANDLE_GUARD(g, __func__);

    lv_group_t *gp = (lv_group_t *)lvglcj_ptr_of(g);
    lvglcj_group_t *s = group_of(g);
    lv_group_delete(gp);
    if (s != NULL) {
        s->used = 0;
    }
    lvglcj_handle_release(g);
    return LVGLCJ_OK;
}

int32_t lvglcj_group_add_obj(int64_t g, int64_t obj)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(g, __func__);
    LVGLCJ_HANDLE_GUARD(obj, __func__);

    lv_group_add_obj((lv_group_t *)lvglcj_ptr_of(g), (lv_obj_t *)lvglcj_ptr_of(obj));
    return LVGLCJ_OK;
}

int32_t lvglcj_group_remove_obj(int64_t obj)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(obj, __func__);

    /* LVGL 的语义就是从「所属组」里移除，因此入参是 obj 而不是 group */
    lv_group_remove_obj((lv_obj_t *)lvglcj_ptr_of(obj));
    return LVGLCJ_OK;
}

int32_t lvglcj_group_focus_obj(int64_t obj)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(obj, __func__);

    lv_group_focus_obj((lv_obj_t *)lvglcj_ptr_of(obj));
    return LVGLCJ_OK;
}

int64_t lvglcj_group_get_focused(int64_t g)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    if (lvglcj_handle_require(g, __func__) != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }

    lv_obj_t *o = lv_group_get_focused((lv_group_t *)lvglcj_ptr_of(g));
    if (o == NULL) {
        return LVGLCJ_HANDLE_NULL;
    }
    /*
     * 未注册的对象返回 0（§3.1.5：LVGL 内部自建对象不向仓颉暴露句柄）。
     * 调用方据此判断「焦点对象不可见」，而不是拿到一个会误用的假句柄。
     */
    return lvglcj_handle_of(o);
}

int32_t lvglcj_group_focus_next(int64_t g)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(g, __func__);

    lv_group_focus_next((lv_group_t *)lvglcj_ptr_of(g));
    return LVGLCJ_OK;
}

int32_t lvglcj_group_focus_prev(int64_t g)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(g, __func__);

    lv_group_focus_prev((lv_group_t *)lvglcj_ptr_of(g));
    return LVGLCJ_OK;
}

int32_t lvglcj_group_set_default(int64_t g)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    /*
     * 允许传 0 表示「取消默认组」（LVGL 接受 NULL）——
     * 这与其它 API 把 0 视为非法句柄不同，是刻意保留的能力，
     * 因为「把默认组设回无」在切换界面层级时是合法需求。
     */
    lv_group_t *gp = NULL;
    if (g != LVGLCJ_HANDLE_NULL) {
        gp = (lv_group_t *)lvglcj_ptr_of(g);
        if (gp == NULL) {
            lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, g, 0, __func__,
                                "group 句柄无效");
            return LVGLCJ_ERR_INVALID_HANDLE;
        }
    }
    lv_group_set_default(gp);
    return LVGLCJ_OK;
}
