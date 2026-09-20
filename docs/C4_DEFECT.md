# C4 缺陷报告：在「C → 仓颉」回调帧内构造异常导致 SIGSEGV

> 状态：**根因已定位**（平台限制，非本项目桥接层缺陷）
> 影响：P0 门禁「回调抛异常不导致进程崩溃」需**修订**
> 日期：2026-09-20
> 平台：WSL2 Ubuntu 24.04 / 仓颉 SDK 1.1.0 (`cjnative`) / LVGL v9.2.2 / gcc 13.3

---

## 1. 摘要

在一个**由 C 调用进仓颉**的回调帧内构造异常对象时，
仓颉运行时会在采集异常栈回溯的过程中崩溃（SIGSEGV）。

崩溃点不在我们的代码里，而在运行时自身：

```
MapleRuntime::FrameInfo::ResolveProcInfo()
  ← MapleRuntime::StackInfo::AnalyseAndSetFrameType()
    ← MapleRuntime::PrintStackInfo::FillInStackTrace()
      ← CJ_MCC_FillInStackTrace()
        ← Exception::<init>(String)
          ← LvglException::<init>(LvglError, String)
            ← 用户回调闭包体（throw LvglException(...)）
              ← dispatchClosure  ← CJ_MCC_N2CStub  ← lvglcj_call_closure (C)
```

**触发条件**：在「C → 仓颉」回调帧**之内构造**异常对象。
**规避方式**：把构造移出回调帧，或改用返回码 / `onError` 上报失败（已验证有效）。

---

## 2. 现象

`cjpm test` 中单一用例以 `ERROR: Crashed with exit code 11` 结束，耗时 2.4–3.3 s；
其余用例全部通过。直接运行测试二进制同样崩溃（与 `cjpm` 无关）：

```
Handle signal: 11.
Thread "lvgl4cj" catched unhandled SIGSEGV (Segmentation fault) from runtime frame.
signal pc: 0x6456f4, addr: 0xfffffffc37074850
```

注意 `addr` 的高 32 位是 `0xfffffffc`，而正常堆/代码地址不会长这样 —— 这是一条**被破坏的指针**。

---

## 3. 最小复现

在「C → 仓颉」回调内构造异常，且栈上存在测试框架的多层包裹帧时稳定复现。

```cangjie
let cid = registerClosure { _ =>
    throw LvglException(LvglError.OutOfMemory, "boom")   // ← 构造发生在此处
}
let _ = unsafe { lvglcj_call_closure(cid, 1) }   // 第 1 次：构造成功、异常被吞并
let _ = unsafe { lvglcj_call_closure(cid, 1) }   // 第 2 次：构造时崩溃
```

测试工程内的靶子位于 `src/closure_test.cj::testC4ReproOriginalBody`（以 `@Skip` 保留）。
**去掉 `@Skip` 即可稳定复现**（`cjpm test` → exit code 11）。

---

## 4. 关键证据（gdb 回溯）

```
#0  0x00000000006456f4 MapleRuntime::FrameInfo::ResolveProcInfo()
#1  0x000000000063fd4f MapleRuntime::StackInfo::AnalyseAndSetFrameType(UnwindContext&)
#2  0x0000000000644e4c MapleRuntime::PrintStackInfo::FillInStackTrace()
#3  0x00000000006104a1 MapleRuntime::StackManager::RecordLiteFrameInfos(vector<unsigned long>&, unsigned long)
#4  0x000000000060733e MCC_FillInStackTraceImpl()
#5  0x000000000061f4a5 CJ_MCC_FillInStackTrace()
#6  0x0000555555c2a574 _CNat9Exception6<init>HRNat6StringE                ← Exception::<init>
#7  0x00005555556eca82 _CN7lvgl4cj13LvglException6<init>HNNY_9LvglErrorERNat6StringE
#8  0x00005555556dd5b6 ClosureTableTests.testC4ReproOriginalBody（闭包体）
#9  0x00005555556e210c ...$E$g
#10 0x00005555556ee355 _CC$Cw$CiF0ulE$g
#11 0x00005555556f0185 _CC$Cw$CiF0ulE$i
#12 0x00005555556f1591 lvgl4cj::dispatchClosure
#13 0x00005555556ef5e2 lvgl4cj::gDispatcher$real
#14 0x000000000061ebea CJ_MCC_N2CStub                    ← C → 仓颉 跨界桩
#15 0x00005555556ef599 lvgl4cj::gDispatcher
#16 0x00005555556f2f6c lvglcj_call_closure at native/src/dispatch.c:65
#17 0x000000000061eee3 CJ_MCC_C2NStub                    ← 仓颉 → C 跨界桩
#18 0x00005555556f0c49 lvgl4cj:lvglcj_call_closure
#19 0x00005555556e3369 ClosureTableTests.testC4ReproOriginalBody

rip = 0x6456f4 <ResolveProcInfo()+20>
rax = 0xfffffffc556ee85d
rcx = 0xfffffffc556ee854
```

**读法**：`rcx / rax` 的**低 32 位 `0x556ee854` / `0x556ee85d` 落在测试二进制的代码段地址范围内**
（对照 #10 = `0x...6ee355`、#12 = `0x...6f1591`），说明这是**一个真实返回地址**，
但**高 32 位被覆盖成了 `0xfffffffc`**。运行时拿这个非法 PC 去查符号信息，
在 `ResolveProcInfo()+20` 处解引用越界而崩溃。

即：**故障是由「栈上保存的返回地址高 32 位被破坏」+「运行时按非法 PC 解引用」共同造成的。**

---

## 5. 已实测证伪的假设

排查过程中逐条做了隔离实验，以下假设**均被证伪**（保留以防重复走弯路）：

| # | 假设 | 实验方法 | 结果 |
| --- | --- | --- | --- |
| H1 | 同一 cid 抛异常后再次分发是根因 | 独立程序 `probe/c4_closure_throw` 场景 `same_cid`：同一 cid 连抛 3 次 | **证伪**，全部正常吞并返回 `-4` |
| H2 | 连续两次抛出异常是根因 | 独立程序场景 `diff_cids`：两个不同 cid 各抛一次 | **证伪**，正常 |
| H3 | 外层 try/catch 包裹（复刻测试框架的用例包裹）是根因 | 独立程序场景 `outer_try` | **证伪**，正常 |
| H4 | 抛出的是「预先构造好的对象」还是「现场构造」是根因 | 独立程序场景 `prebuilt_throw` | **部分证伪**：独立程序中两者都正常，故它单独不足以触发 |
| H5 | 仓颉轻量级线程栈过小（`cjStackSize` 默认 128KB）导致回溯读越界 | `cjStackSize=8388608 cjpm test` | **证伪**，仍然崩溃 |
| H6 | 仓颉栈**深度**本身是触发条件 | 独立程序场景 `deep_stack`：先递归 40 / 200 / 800 层再 dispatch | **证伪**，三档深度全部正常 |

**结论**：六个假设全部排除 ⇒ 触发条件需要「在回调帧内构造异常」**叠加**「测试框架特有的栈形态」
（涉及 `UnitTestCase.create` 的 lambda 包裹、框架的接口调用帧等多层结构），
而**单纯的调用深度不足以触发**。这也解释了为什么独立程序在所有场景下都无法复现，
而测试框架内稳定复现。

**对严重程度的推论（谨慎表述）**：由于普通形态的独立程序（含 800 层递归）均无法触发，
**真实应用的触发风险低于最初估计**；但「未能在普通形态复现」**不等于**「普通应用安全」——
因此仍按**已知平台限制 + 禁止该用法**处理，而不是降级为「只在测试里出现的怪现象」。

---

## 6. 为什么可以排除「我们自己的缺陷」

`probe/c4_closure_throw` 是一个脱离测试框架的独立可执行程序，
通过 `lvgl4cj` 模块的**公开 API**（`registerClosure` / `bindCurrentThreadAsLvgl`）
走**完全相同的** `dispatchClosure → gDispatcher → lvglcj_call_closure` 路径，五个场景全部通过：

| 场景 | 内容 | 结果 |
| --- | --- | --- |
| `same_cid` | 同一 cid 连续抛 3 次 | 全部 `-4`，计数正确，干净退出 |
| `diff_cids` | 不同 cid 各抛一次 | 正常 |
| `normal_after` | 抛一次后再分发正常闭包 | 正常（`rc=0`） |
| `outer_try` | 外层 try/catch 包裹 | 正常 |
| `prebuilt_throw` | 异常在回调外构造、回调内只 throw | 正常 |

因此：**闭包表、分发器、以及「异常在边界被吞并并转为返回码」这套逻辑是正确的**，
崩溃由运行时在特定栈形态下采集栈回溯时产生。

---

## 7. 规避方式（已验证）

### 7.1 推荐做法：回调内不构造异常

回调中用**返回码 / `onError`** 上报失败，完全不构造异常。这也是 L1 API 应有的设计取向：
`onError` 机制本来就存在，正是为「回调内出错」准备的，比让用户 `throw` 更可控。

### 7.2 过渡做法：把构造移出回调帧

```cangjie
// 在回调帧**之外**构造（此时栈上还没有桥接层的 C 帧）
let boom = LvglException(LvglError.InvalidArgument, "预构造")
let cid = registerClosure { _ => throw boom }     // 回调内只 throw 已有对象
```

**验证结果**：采用此写法后，`cjpm test` = **28 PASSED / 1 SKIPPED / 0 ERROR**，两次运行结果一致。

### 7.3 不要做的事

- 不要在用户回调里写 `throw LvglException(...)`（现场构造）
- 不要因为「偶尔不崩」就当作可用 —— 崩溃依赖栈形态，属间歇性、上线后极难定位

---

## 8. 对 P0 门禁与设计文档的影响

**P0 验收标准**中原表述为「回调抛异常不导致进程崩溃」。基于本缺陷，该条必须修订为：

> 回调内的失败通过**返回码或 `onError`** 上报；
> 框架保证异常**传播**不跨越 C 边界（`dispatchClosure` 的捕获边界已实现并有测试覆盖）；
> 在「C → 仓颉」回调帧内**构造**异常属**已知平台限制**，
> 需写入用户指引的禁止事项，并在升级仓颉 SDK 后重新评估。

同时应补入设计文档 §D.5「禁止事项」清单一条：

> **禁止在跨语言回调内构造异常对象**（`throw` 会触发构造）。
> 原因：仓颉 1.1.0 运行时在混合栈上采集异常栈回溯存在缺陷，见 C4 缺陷报告。

---

## 9. 复现与验证

```bash
# 1) 复现（测试框架内，稳定崩溃）
#    编辑 src/closure_test.cj，去掉 testC4ReproOriginalBody 上的 @Skip
cd <工程根> && cjpm test        # → ERROR: Crashed with exit code 11

# 2) 独立程序侧的反证（五场景全部通过）
cd probe/c4_closure_throw
for s in same_cid diff_cids normal_after outer_try prebuilt_throw; do
    ./target/release/bin/main $s
done

# 3) 取栈回溯
gdb -batch -ex "handle SIGSEGV stop" -ex run -ex "bt 25" \
    -ex "info registers rip rsp rax rbx rcx" \
    --args target/release/unittest_bin/lvgl4cj
```

> 注：仓颉自带调试器 `cjdb` 在本机不可用 ——
> 它是指向 LLDB 的符号链接，依赖 `libpython3.11.so.1.0`，而 Ubuntu 24.04 只有 python3.12。
> 因此改用 `gdb`（`sudo apt-get install -y --no-install-recommends gdb`，
> 直接装会因 `libc6-dbg` 版本 404 失败，需先 `apt-get update`，或加 `--no-install-recommends`）。

---

## 10. 后续行动

| # | 行动 | 优先级 |
| --- | --- | --- |
| 1 | 把「回调内禁止构造异常」写入设计文档 §D.5 禁止事项与用户指引 | 高 |
| 2 | 修订 P0 门禁第 6 条表述（见 §8） | 高 |
| 3 | L1 API 提供 `onError` 优先的错误上报路径，降低用户 `throw` 的动机 | 高 |
| 4 | 升级仓颉 SDK 后，去掉 `testC4ReproOriginalBody` 的 `@Skip` 直接回归 | 中 |
| 5 | 向仓颉 SDK 反馈：混合（C/仓颉）栈上 `FillInStackTrace` 产生非法 PC | 中 |
| 6 | 排查是否存在不构造异常即可绕过栈回溯的 API（如预分配异常池） | 低 |

---

## 附：本报告未做的事（避免误导）

- **没有修复该缺陷**。它是仓颉运行时内部问题，本项目无法从外部修复。
- **没有证明「只有测试框架下才崩」**。已实测「深度 800 层的普通调用链」不触发（H6），
  但仍无法断言普通应用一定安全 —— 测试框架只是**目前唯一能稳定复现的催化条件**。
  因此按「已知限制 + 禁止用法」处理，而不是按「只在测试里出现」处理。
- **没有定位到触发所需的精确栈形态**。六个候选假设均已证伪，剩余差异（框架的 lambda 包裹帧、
  接口调用帧的具体组合）尚未逐帧比对；若要彻底定性，需要在框架内逐层裁剪调用链。
  考虑到已有可用的规避方式且不阻塞 P0 推进，此项列为后续行动（见 §10.5）。
- §5 中五个假设的证伪实验均在 `probe/c4_closure_throw` 与 `cjStackSize` 环境变量下完成，步骤可复现。
