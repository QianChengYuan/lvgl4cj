/*
 * font.c —— 字体子系统（设计文档 §5.10）
 *
 * ==================== 两类字体，生命周期完全不同 ====================
 *   1) **内置字体**：编译进库的静态 const 对象（如 lv_font_simsun_16_cjk）。
 *      ★ 绝不能被释放 —— 对它调用 free/destroy 是未定义行为。
 *        因此本文件维护一份「哪些句柄指向内置字体」的记录，
 *        font_delete 命中它时**明确拒绝**并说明原因，而不是放行到崩溃。
 *   2) **加载字体**：lv_binfont_create 从文件读出来的堆对象，必须 destroy。
 *
 * 为什么内置字体也要句柄化：壳层（仓颉）只能通过句柄引用原生对象
 * （ADR-001），而 style_set_text_font 收的就是句柄。句柄表对「同一指针重复登记
 * 返回同一句柄」的特性，正好让「每次取内置字体都拿到同一个句柄」成立，
 * 因此不需要额外的缓存逻辑。
 *
 * P0 的背景：LVGL 内置的 Montserrat 只有拉丁字形，中文会触发
 * "glyph dsc. not found" 且界面上什么都不显示。所以必须在 lv_conf 里
 * 开启 LV_FONT_SIMSUN_16_CJK，并在界面中显式使用它 —— 这正是本文件存在的直接原因。
 */
#include "lvglcj_internal.h"

#include <string.h>

/*
 * 判断一个字体指针是否为**内置（静态 const）字体**。
 *
 * ★ 这里用「与已知内置字体对象比指针」，而**不是**「把内置字体的句柄记到一个数组里」。
 *
 *   早先的实现是后者：一个 `LVGLCJ_BUILTIN_FONT_MAX`（8）大小的句柄数组，
 *   满了就**静默不再记录**（原注释写的是「超出时只记录不报错」）。
 *   它有两个问题，第一个是实打实的 bug：
 *
 *   1. ★ 句柄 id 单调递增且**永不复用**（ADR-001），而 lvglcj_deinit()/init()
 *      可以反复发生。于是同一对静态字体在每次初始化后都会拿到**新句柄**，
 *      数组很快被**上一轮生命周期留下的陈旧句柄**占满；
 *      此后新登记的内置字体句柄不再被识别，lvglcj_font_delete() 就会
 *      放行到 lv_binfont_destroy() 去释放**静态 const 对象** —— 未定义行为。
 *
 *      实测触发方式：仓颉侧连续跑多个「init → 取内置字体 → deinit」的用例，
 *      约 4 轮之后数组即被用尽，第 9 个用例在 close() 内置字体时**挂死**。
 *      单独跑该用例则通过 —— 典型的「只在长序列里复现」。
 *      根子是「容量 8」这个数字本身没有依据，而溢出路径又是静默的。
 *
 *   2. 这个集合本来就是**编译期已知**的（就那么几个 &lv_font_xxx）。
 *      用运行时簿记去描述它，等于凭空引入一个会满、会过期、会静默失效的状态。
 *      改成比指针后，这两类失效都从根上不存在了 —— 也**去掉了溢出路径**。
 */
static int lvglcj_font_is_builtin_ptr(const lv_font_t *f)
{
    if (f == NULL) {
        return 0;
    }
    /* LV_FONT_DEFAULT 在 lv_conf 里就是 &lv_font_montserrat_14（指针） */
    if (f == LV_FONT_DEFAULT) {
        return 1;
    }
#if LV_FONT_SIMSUN_16_CJK
    if (f == &lv_font_simsun_16_cjk) {
        return 1;
    }
#endif
    return 0;
}

/* ------------------------------------------------------------ 内置字体 */

int64_t lvglcj_font_builtin(int32_t which)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    const lv_font_t *f = NULL;
    switch (which) {
        case LVGLCJ_FONT_DEFAULT:
            /* LV_FONT_DEFAULT 由 lv_conf 的 LV_FONT_DEFAULT 决定（当前为 Montserrat 14） */
            f = LV_FONT_DEFAULT;
            break;
        case LVGLCJ_FONT_CJK_16:
#if LV_FONT_SIMSUN_16_CJK
            f = &lv_font_simsun_16_cjk;
#else
            /*
             * 明确报错而不是回退到默认字体：回退会让「中文不显示」变成一个
             * 需要从画面倒推的问题，而这里可以直接指出「字体没编译进来」。
             */
            lvglcj_record_error(LVGLCJ_ERR_NOT_SUPPORTED, 0, 0, __func__,
                                "LV_FONT_SIMSUN_16_CJK 未开启，请检查 scripts/gen_lv_conf.sh 并重建");
            return LVGLCJ_HANDLE_NULL;
#endif
            break;
        default:
            lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                                "未知的内置字体编号");
            return LVGLCJ_HANDLE_NULL;
    }

    if (f == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, 0, 0, __func__,
                            "内置字体指针为空");
        return LVGLCJ_HANDLE_NULL;
    }

    /*
     * 同一指针重复登记返回同一句柄（句柄表一对一），因此这里天然幂等。
     *
     * ★ 不再需要「把句柄登记进内置字体表」：是否为内置字体由
     *   lvglcj_font_is_builtin_ptr() 按指针判定，与句柄 id 无关，
     *   因此 init/deinit 反复发生也不会失效。
     */
    return lvglcj_handle_register((void *)(uintptr_t)f, "lv_font_t(builtin)");
}

/* ------------------------------------------------------------ 字形覆盖查询 */

int32_t lvglcj_font_has_glyph(int64_t font, int32_t codepoint)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(font, __func__);

    if (codepoint < 0) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, font, 0, __func__,
                            "码位不能为负");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    const lv_font_t *f = (const lv_font_t *)lvglcj_ptr_of(font);
    if (f == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_HANDLE, font, 0, __func__,
                            "字体指针为空");
        return LVGLCJ_ERR_INVALID_HANDLE;
    }

    /*
     * dsc 是**出参**，必须由我们提供对象（LVGL 只往里写，不会保存该指针）。
     * 用 0 作 letter_next：本查询不关心字距（kerning）—— 字距只影响 dsc 里的
     * advance 值，不影响「有没有字形」这个布尔问题。
     */
    lv_font_glyph_dsc_t dsc;
    if (lv_font_get_glyph_dsc(f, &dsc, (uint32_t)codepoint, 0)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ 加载字体 */

int64_t lvglcj_font_load(const char *path)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return LVGLCJ_HANDLE_NULL;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();

    if (path == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, 0, 0, __func__,
                            "字体路径为空");
        return LVGLCJ_HANDLE_NULL;
    }

    /*
     * lv_binfont_create 要求文件系统已就绪（LV_USE_FS_POSIX + lvglcj_fs_init_posix）。
     * 失败时返回 NULL，并会打 LVGL 自己的日志说明是「打不开」还是「格式不对」。
     */
    lv_font_t *f = lv_binfont_create(path);
    if (f == NULL) {
        lvglcj_record_error(LVGLCJ_ERR_BACKEND_FAILURE, 0, 0, __func__,
                            "字体文件加载失败（检查 lv_fs 是否已初始化、路径是否在 FS 根下）");
        return LVGLCJ_HANDLE_NULL;
    }

    int64_t h = lvglcj_handle_register(f, "lv_font_t(loaded)");
    if (h == LVGLCJ_HANDLE_NULL) {
        /* 登记失败必须把刚加载的字体销毁，否则内存泄漏 */
        lv_binfont_destroy(f);
        return LVGLCJ_HANDLE_NULL;
    }
    return h;
}

int32_t lvglcj_font_delete(int64_t font)
{
    int32_t rc = lvglcj_require_initialized(__func__);
    if (rc != LVGLCJ_OK) {
        return rc;
    }
    LVGLCJ_CHECK_LVGL_THREAD_RET();
    LVGLCJ_HANDLE_GUARD(font, __func__);

    lv_font_t *f = (lv_font_t *)lvglcj_ptr_of(font);

    /*
     * ★ 内置字体是静态 const 对象，释放它是未定义行为（通常直接崩）。
     *   这里必须**明确拒绝**，而不是「看起来能跑就放行」。
     *   判定基于**指针**（集合编译期已知），不依赖运行时簿记 ——
     *   理由见 lvglcj_font_is_builtin_ptr 上方的说明（那里记录了这个判定
     *   曾经因为「容量 8 + 静默溢出」而失效、并导致挂死的完整经过）。
     */
    if (lvglcj_font_is_builtin_ptr(f)) {
        lvglcj_record_error(LVGLCJ_ERR_INVALID_ARGUMENT, font, 0, __func__,
                            "内置字体不可释放（它是编译进库的静态对象）");
        return LVGLCJ_ERR_INVALID_ARGUMENT;
    }

    if (f != NULL) {
        lv_binfont_destroy(f);
    }
    lvglcj_handle_release(font);
    return LVGLCJ_OK;
}
