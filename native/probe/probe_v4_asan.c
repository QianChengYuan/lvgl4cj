/*
 * probe_v4_asan.c —— V4：ASan 有效性自证（设计文档 §9.5 第 4 点）
 *
 * 为什么需要「自证」：
 *   CI 门禁要求「ASan 零报告」。但如果 ASan 根本没生效（例如编译时漏了
 *   -fsanitize=address，或被仓颉运行时的干扰掩盖），那么「零报告」就是假的安心。
 *   所以必须先人为制造一个明确的内存问题，确认 ASan 能报出来。
 *
 * 用法：
 *   probe_v4_asan leak    期望 LeakSanitizer 在退出时报告（退出码非 0）
 *   probe_v4_asan overflow 期望 AddressSanitizer 立即报告
 *
 * 注意：本探针**必须**用 -DLVGLCJ_ASAN=ON 构建才有效；
 *       未启用 ASan 时它会正常退出（并在输出里提示这一点）。
 */
#include "lvglcj_probe.h"

#include <stdio.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define LVGLCJ_ASAN_BUILTIN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LVGLCJ_ASAN_BUILTIN 1
#endif
#endif

#ifndef LVGLCJ_ASAN_BUILTIN
#define LVGLCJ_ASAN_BUILTIN 0
#endif

int main(int argc, char **argv)
{
    const char *kind = (argc > 1) ? argv[1] : "leak";

    printf("=== V4: ASan 有效性自证 ===\n");
    printf("[V4] 本可执行文件是否编译期启用了 ASan : %s\n",
           LVGLCJ_ASAN_BUILTIN ? "YES" : "NO");

    if (!LVGLCJ_ASAN_BUILTIN) {
        printf("[V4] 结论：SKIP —— 本二进制未启用 ASan，无法做自证。\n");
        printf("[V4]       请用 bash scripts/build_native.sh --probes --asan 重新构建。\n");
        return 0;
    }

    if (strcmp(kind, "leak") == 0) {
        printf("[V4] 触发类型：堆内存泄漏（故意不 free）\n");
        int32_t rc = lvglcj_probe_asan_trigger(0);
        printf("[V4] 触发函数返回 : %d\n", rc);
        printf("[V4] 期望：进程退出时 LeakSanitizer 报出泄漏，退出码非 0\n");
        return 0;
    }

    if (strcmp(kind, "overflow") == 0) {
        printf("[V4] 触发类型：堆缓冲区越界写\n");
        printf("[V4] 期望：AddressSanitizer 立即报出 heap-buffer-overflow\n");
        int32_t rc = lvglcj_probe_asan_trigger(1);
        printf("[V4] 触发函数返回 : %d（正常情况下不应执行到这里）\n", rc);
        return 0;
    }

    printf("[V4] 未知类型：%s（可选 leak / overflow）\n", kind);
    return 2;
}
