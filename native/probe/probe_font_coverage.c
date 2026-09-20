/*
 * probe_font_coverage —— 内置字体的**字形覆盖**实测
 *
 * ============================================================================
 * 【为什么需要这个探针：一次被正确措辞纠正的严重误述】
 * ============================================================================
 *
 * LVGL 官方对 `LV_FONT_SIMSUN_16_CJK` 的原文是
 *     "16 px font with normal range plus 1000 of the most common **CJK radicals**"
 * （lv_conf_template.h / lv_conf_internal.h / cmsis-pack / docs/overview/font.rst
 *   四处一致，且 ime_pinyin.rst 也重复了 "more than 1,000 most common CJK radicals"）
 *
 * 即：它是**部首/字根**表，不是常用汉字表。
 *
 * 这个区别不是措辞问题，而是会直接导致界面出错的事实：
 * 字库里含有大量**繁体与日文异体**（問 / 調 / 窓），
 * 而对应的**简体**形式（问 / 调 / 窗）缺失。
 * 于是一句「界面含中文就用这个字体」会让最常见的词静默少字 ——
 * 而且**不报错**（LVGL 只在日志里打一行 glyph not found），
 * 画面上就是缺一块，非常容易漏过。
 *
 * 本项目早前在 src/font.cj、src/ffi_bridge.cj、docs/P0_RESULTS.md 三处
 * 都把它写成了「常用汉字」—— 本探针就是这三处更正所依据的实测。
 *
 * ============================================================================
 * 【判据设计】
 * ============================================================================
 * 不采样、不推断，直接问字体：`lv_font_get_glyph_dsc()` 的 letter 参数
 * 就是 Unicode 码位，返回 false 即「该字无法渲染」。
 *
 * 第 0 节是**自检**：先用几个确定存在的字（ASCII 'A'、'一'）验证调用约定正确。
 * 若自检失败，则后续所有「缺失」结论都无效（可能只是探针问错了方式）——
 * 这是必须的，否则一次 API 误用会被误读成「字库几乎是空的」。
 */

#include "lvgl.h"
#include "lvglcj_bridge.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* 单字是否可渲染 */
static int has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, codepoint, 0) ? 1 : 0;
}

/* 打印一组码位，返回缺失个数 */
static int report_group(const lv_font_t *font, const char *title,
                        const uint32_t *codes, const char *const *names, int n)
{
    printf("\n-- %s --\n", title);
    int missing = 0;
    for (int i = 0; i < n; i++) {
        int ok = has_glyph(font, codes[i]);
        if (!ok) {
            missing++;
        }
        printf("  %-8s U+%04X : %s\n", names[i], codes[i],
               ok ? "存在" : "★ 缺失");
    }
    printf("  → 缺失 %d / %d\n", missing, n);
    return missing;
}

/* 统计区间内可渲染的码位数 */
static uint32_t count_range(const lv_font_t *font, uint32_t lo, uint32_t hi)
{
    uint32_t n = 0;
    for (uint32_t cp = lo; cp <= hi; cp++) {
        if (has_glyph(font, cp)) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ 目标字符集 */

/* 社区报告缺失的简体字（本探针要确认的正是这一组） */
static const uint32_t k_simplified[] = {
    0x95EE, /* 问 */ 0x5385, /* 厅 */ 0x706F, /* 灯 */
    0x8C03, /* 调 */ 0x7A97, /* 窗 */ 0x5E18, /* 帘 */
};
static const char *const k_simplified_names[] = {
    "问", "厅", "灯", "调", "窗", "帘"
};

/* 上面对应的繁体 / 日文异体 —— 用于验证「有繁体而无简体」这个模式 */
static const uint32_t k_traditional[] = {
    0x554F, /* 問 */ 0x5EF3, /* 廳 */ 0x71C8, /* 燈 */
    0x8ABF, /* 調 */ 0x7A93, /* 窓(日) */ 0x7C3E, /* 簾 */
};
static const char *const k_traditional_names[] = {
    "問", "廳", "燈", "調", "窓", "簾"
};

/* 本项目自己的文档、注释与示例里真正出现过的字
   （font.cj 的 "你好"、P0_RESULTS 的 "显示"、perf_test 的 "中文字体大小" 等）*/
static const uint32_t k_ours[] = {
    0x4F60, /* 你 */ 0x597D, /* 好 */ 0x4E16, /* 世 */ 0x754C, /* 界 */
    0x4E2D, /* 中 */ 0x6587, /* 文 */ 0x6211, /* 我 */ 0x663E, /* 显 */
    0x793A, /* 示 */ 0x5B57, /* 字 */ 0x4F53, /* 体 */ 0x5927, /* 大 */
    0x5C0F, /* 小 */
};
static const char *const k_ours_names[] = {
    "你", "好", "世", "界", "中", "文", "我", "显", "示", "字", "体", "大", "小"
};

/*
 * ★ 示例**实际渲染到界面**的文案用字。
 *
 * 与上一组（文档散文用字）的区别是判定强度不同：
 * 文档里缺一个字只是阅读体验，而这里缺一个字就是**画面上少一块** ——
 * 所以这一组必须是**硬断言**，不能只打印。
 *
 * 来源（grep setText 得到，非手工抄写）：
 *   examples/hello_cj/src/main.cj:88   "点我"
 *   examples/hello_cj/src/main.cj:102  "点了 ${n} 次"
 *   src/p0_assert_test.cj:181          "点了 1 次"
 */
static const uint32_t k_rendered[] = {
    0x70B9, /* 点 */ 0x6211, /* 我 */ 0x4E86, /* 了 */ 0x6B21, /* 次 */
};
static const char *const k_rendered_names[] = { "点", "我", "了", "次" };

/* 部首/字根样本：若字库真由「部首表」而来，这一组应当几乎全在 */
static const uint32_t k_radicals[] = {
    0x4E00, /* 一 */ 0x4E28, /* 丨 */ 0x4E36, /* 丶 */ 0x4E3F, /* 丿 */
    0x4E59, /* 乙 */ 0x4E85, /* 亅 */ 0x4E8C, /* 二 */ 0x4EA0, /* 亠 */
    0x4EBA, /* 人 */ 0x513F, /* 儿 */ 0x5165, /* 入 */ 0x516B, /* 八 */
    0x5182, /* 冂 */ 0x5196, /* 冖 */ 0x51AB, /* 冫 */ 0x51E0, /* 几 */
    0x5200, /* 刀 */ 0x529B, /* 力 */ 0x52F9, /* 勹 */ 0x5315, /* 匕 */
    0x531A, /* 匚 */ 0x5338, /* 匸 */ 0x5341, /* 十 */ 0x535C, /* 卜 */
    0x5369, /* 卩 */ 0x5382, /* 厂 */ 0x53B6, /* 厶 */ 0x53C8, /* 又 */
};
static const char *const k_radicals_names[] = {
    "一", "丨", "丶", "丿", "乙", "亅", "二", "亠", "人", "儿",
    "入", "八", "冂", "冖", "冫", "几", "刀", "力", "勹", "匕",
    "匚", "匸", "十", "卜", "卩", "厂", "厶", "又"
};

/* ------------------------------------------------------------------ codes 模式 */

/* 把码位按 UTF-8 写出来，好让人一眼看出是哪个字 */
static void print_utf8(uint32_t cp)
{
    if (cp < 0x80) {
        putchar((int)cp);
    } else if (cp < 0x800) {
        putchar((int)(0xC0 | (cp >> 6)));
        putchar((int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        putchar((int)(0xE0 | (cp >> 12)));
        putchar((int)(0x80 | ((cp >> 6) & 0x3F)));
        putchar((int)(0x80 | (cp & 0x3F)));
    } else {
        putchar((int)(0xF0 | (cp >> 18)));
        putchar((int)(0x80 | ((cp >> 12) & 0x3F)));
        putchar((int)(0x80 | ((cp >> 6) & 0x3F)));
        putchar((int)(0x80 | (cp & 0x3F)));
    }
}

/*
 * codes 模式：从 stdin 读十六进制码位（空白/换行分隔），报出字库里**缺失**的那些。
 *
 * 退出码：0 = 全部可渲染；5 = 有缺失；1 = 初始化失败
 *
 * 供 scripts/cjk_audit.py 调用。分工的理由：
 *   · 「扫源码、抽出到底用了哪些字」是文本处理，放在脚本里改起来最快；
 *   · 「这些字字库有没有」必须问 C（仓颉侧要起运行时，做一次性审计太重）。
 * 两边用「十六进制码位」这个最小接口对接，比传字符串更少歧义。
 */
static int check_codes_from_stdin(void)
{
    const lv_font_t *f = &lv_font_simsun_16_cjk;
    unsigned int cp = 0;
    int total = 0;
    int missing = 0;

    while (scanf("%x", &cp) == 1) {
        total++;
        if (!has_glyph(f, (uint32_t)cp)) {
            if (missing == 0) {
                printf("★ 以下字符在该字库里**没有字形**（界面会静默少一块）：\n");
            }
            printf("    ");
            print_utf8((uint32_t)cp);
            printf("   U+%04X\n", cp);
            missing++;
        }
    }

    if (total == 0) {
        printf("（stdin 未读到码位；期望十六进制，如 4F60 597D）\n");
        return 1;
    }

    printf("\n码位总数 %d，缺失 %d，覆盖 %.1f%%\n",
           total, missing, 100.0 * (double)(total - missing) / (double)total);
    return (missing == 0) ? 0 : 5;
}

/* ------------------------------------------------------------------ 入口 */

int main(int argc, char **argv)
{
    /* 关缓冲：探针的输出是证据，不能在崩溃或截断时丢失 */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (lvglcj_init() != LVGLCJ_OK) {
        printf("lvglcj_init 失败\n");
        return 1;
    }

    /* codes 模式：只回答「这批码位能不能渲染」，不做本文件的固定核对 */
    if (argc > 1 && strcmp(argv[1], "codes") == 0) {
        int rc = check_codes_from_stdin();
        lvglcj_deinit();
        return rc;
    }

    printf("=== probe_font_coverage：内置字体字形覆盖实测 ===\n");
    printf("LVGL %s\n", lv_version_info());

    const lv_font_t *f_cjk = &lv_font_simsun_16_cjk;
    const lv_font_t *f_def = &lv_font_montserrat_14;

    /* ------------------------------ 0. 自检 ------------------------------ */
    printf("\n-- 0. ★ 自检（失败则后续结论全部无效）--\n");
    int sane = 1;
    {
        struct {
            const char *what;
            const lv_font_t *f;
            uint32_t cp;
        } t[] = {
            { "ASCII 'A' @ SimSun16", f_cjk, 0x41 },
            { "'一' U+4E00 @ SimSun16", f_cjk, 0x4E00 },
            { "ASCII 'A' @ Montserrat14", f_def, 0x41 },
        };
        for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            int ok = has_glyph(t[i].f, t[i].cp);
            printf("  %-28s : %s\n", t[i].what, ok ? "存在" : "★ 缺失");
            if (!ok) {
                sane = 0;
            }
        }
        /* 对照：Montserrat 不应有中文 —— 若它有，说明我们拿错了字体指针 */
        int latin_only = !has_glyph(f_def, 0x4E00);
        printf("  %-28s : %s\n", "'一' U+4E00 @ Montserrat14",
               latin_only ? "缺失（符合预期）" : "★ 存在（字体指针可能拿错）");
        if (!latin_only) {
            sane = 0;
        }
    }
    if (!sane) {
        printf("\n★★ 自检失败：探针的调用约定有问题，以下结论不可采信 ★★\n");
        lvglcj_deinit();
        return 2;
    }

    /* ------------------------------ 1..4. 分组核对 ------------------------------ */
    int miss_simp = report_group(f_cjk, "1. 社区报告缺失的简体字",
                                 k_simplified, k_simplified_names, 6);
    /* ★ 必须接住返回值：首版漏接，导致结论区把它当成「缺失 0」打印出来 ——
     *   一个纯粹由「忘记接收返回值」造成的、看起来很合理的错误数字。 */
    int miss_trad = report_group(f_cjk, "2. 对应的繁体 / 日文异体（用来验证「有繁无简」这个模式）",
                                 k_traditional, k_traditional_names, 6);
    int miss_ours = report_group(f_cjk, "3. ★ 本项目文档/注释/示例里真正用到的字",
                                 k_ours, k_ours_names, 13);
    int miss_rad = report_group(f_cjk, "4. 部首/字根样本（若字库源于部首表，应几乎全在）",
                                k_radicals, k_radicals_names, 28);

    /* 示例实际渲染用字：硬断言（见上面 k_rendered 的说明） */
    int miss_rendered = report_group(f_cjk, "3b. ★★ 示例实际渲染到界面的用字（必须全在）",
                                     k_rendered, k_rendered_names, 4);

    /* ------------------------------ 5. 区间统计 ------------------------------ */
    printf("\n-- 5. 区间统计（回答「到底有多少个 CJK 字」）--\n");
    uint32_t n_cjk = count_range(f_cjk, 0x4E00, 0x9FFF);
    uint32_t n_kana = count_range(f_cjk, 0x3040, 0x30FF);
    uint32_t n_ascii = count_range(f_cjk, 0x20, 0x7E);
    printf("  U+4E00..U+9FFF（CJK 统一表意文字，共 20992 码位）：含 %u 个\n", n_cjk);
    printf("  U+3040..U+30FF（平假名 + 片假名，共 192 码位）    ：含 %u 个\n", n_kana);
    printf("  U+0020..U+007E（ASCII 可打印，共 95 码位）        ：含 %u 个\n", n_ascii);

    /* ------------------------------ 6. 结论 ------------------------------ */
    printf("\n=== 结论 ===\n");
    printf("  · 简体目标缺 %d/6；对应繁体/日文异体缺 %d/6（問/調/窓 存在）\n",
           miss_simp, miss_trad);
    printf("  · 本项目自身用字缺失：%d / 13\n", miss_ours);
    printf("  · ★ 部首样本缺失：%d / 28 —— 连「它就是部首表」这个字面读法也不成立\n",
           miss_rad);
    printf("  · CJK 统一表意文字实测 %u 个，而假名 %u/192（几乎全覆盖）\n", n_cjk, n_kana);
    printf("\n  → 它是一份**手工拼合的混合清单**：简体常用字 + 繁体字 + 日文假名/异体。\n");
    printf("    既不是完整的部首表，也不是常用字表。\n");
    printf("    官方 \"1000 of the most common CJK radicals\" 是**历史描述**，\n");
    printf("    不能当作选字依据 —— 真实成分要用本探针这样的实测来看。\n");
    printf("  → 实用结论：不存在「这个字库支持中文」这种判断，\n");
    printf("    只能对**每一段真正要显示的文本**逐个字符核对（见第 3 节）。\n");

    /*
     * ------------------------------ 7. 结论锁定 ------------------------------
     *
     * 这里断言的是**我们文档与注释所依据的实测事实**，而不是「字体应该怎样」。
     *
     * 为什么要锁住一个「缺陷」：src/font.cj / src/ffi_bridge.cj / docs/P0_RESULTS.md
     * 三处都写了「界面含中文就用 Cjk16」。这个说法之所以被保留（而非改成"不能用"），
     * 前提正是**常见的示例用字恰好命中**（你好/世界/中文 都在）。
     * 一旦 LVGL 换了字库，这个前提就可能失效 —— 那些注释会从"略有不准确"变成
     * "会误导使用者"。锁住它，任何字库变更都会在这里立刻失败，强制复核那三处文字。
     */
    printf("\n-- 7. ★ 结论锁定（失败说明 LVGL 换了字库，必须复核三处文档）--\n");
    int lock_ok = 1;
    struct {
        const char *what;
        uint32_t cp;
        int want; /* 1 = 存在，0 = 缺失 */
    } lock[] = {
        { "问 仍缺失（简体缺口）", 0x95EE, 0 },
        { "调 仍缺失（简体缺口）", 0x8C03, 0 },
        { "窗 仍缺失（简体缺口）", 0x7A97, 0 },
        { "問 仍存在（繁体在册）", 0x554F, 1 },
        { "調 仍存在（繁体在册）", 0x8ABF, 1 },
        { "窓 仍存在（日文异体）", 0x7A93, 1 },
        { "我 仍存在（P0 断言 A4 依赖它）", 0x6211, 1 },
    };
    for (size_t i = 0; i < sizeof(lock) / sizeof(lock[0]); i++) {
        int got = has_glyph(f_cjk, lock[i].cp) ? 1 : 0;
        int ok = (got == lock[i].want);
        printf("  %-36s : %s\n", lock[i].what, ok ? "符合" : "★ 已变化");
        if (!ok) {
            lock_ok = 0;
        }
    }
    /* 数量级也要锁：只要它还在千余量级，「不是常用字表」这个结论就成立 */
    int count_ok = (n_cjk >= 900 && n_cjk <= 1500);
    printf("  %-36s : %s（%u）\n", "CJK 字数仍在千余量级",
           count_ok ? "符合" : "★ 已变化", n_cjk);
    if (!count_ok) {
        lock_ok = 0;
    }

    lvglcj_deinit();

    /*
     * ★ 示例渲染用字单独判定，且**必须硬失败**。
     *
     * 与「文档散文用字」不同：文档里缺一个字只是阅读体验，
     * 而示例里缺一个字表示**界面本身就是残缺的** —— 画面上少一块。
     * 这个缺陷 LVGL 不报错、CI 不报错、单测也测不到（文本设置了、句柄也有效），
     * 只有人眼看截图才会发现。所以它必须在这里挡下来。
     */
    if (miss_rendered != 0) {
        printf("\n★★ 示例实际渲染的文案有 %d 个字无法渲染：界面会缺少字形。\n"
               "   请改文案，或换成覆盖完整的字体（LvFont.load）。★★\n", miss_rendered);
        return 4;
    }

    if (!lock_ok) {
        printf("\n  ★★ 实测事实已变化：请复核 src/font.cj、src/ffi_bridge.cj、"
               "docs/P0_RESULTS.md 中关于内置 CJK 字库的描述 ★★\n");
        return 3;
    }
    return 0;
}
