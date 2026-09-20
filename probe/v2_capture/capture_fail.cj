/*
 * V2 证据文件 —— 「捕获闭包 → C 函数指针」在仓颉语言层不成立的书面证据
 *
 * ★ 本文件**故意编译失败**，且**不在任何 cjpm 工程的 src/ 下**，
 *   因此不会被正常构建流程编译到。
 *   scripts/run_probe.sh 会用 cjc 单独编译它，并断言编译**失败**。
 *
 * 结论（V2-a = 不通过）：
 *   仓颉的 CFunc Lambda 不能捕获变量，因此设计文档 §3.2.4 的
 *   「分支 A：直接持有闭包指针 + pin 表」在语言层即不成立。
 *
 * 由此归档：
 *   · 分支 B（自建闭包表 + closure_id）是**唯一路径**，不再是「默认设计」
 *   · pin 表、PIN_EXHAUSTED(-16)、风险 R16、ADR-018 标记为未采用
 *   · 删除设计文档自称的「本项目最危险的自欺」路径
 */
package v2_capture_evidence

main() {
    var captured = 0

    /*
     * 下面这一行是本证据的核心：期望编译器报「不能捕获变量」类错误。
     * 如果这一行竟然编译通过，说明仓颉的 CFunc 语义发生了变化，
     * 必须重新评估分支 A —— 那么 run_probe.sh 的断言会失败并报警。
     */
    let cb: CFunc<(Int32) -> Unit> = { v => captured = v }

    unsafe { cb(1) }
    println("${captured}")
}
