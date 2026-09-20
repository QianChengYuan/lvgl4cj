# P0 探针实测结论（V1–V7）

> 本文是**实测记录**，不是推测。所有结论都可由 `bash scripts/run_probe.sh` 复现。
> 设计文档 §12 的立场是「下一步不是写文档，是写探针」；本文即为那一步的产出。

## 0. 环境基线

| 项 | 值 |
| --- | --- |
| 平台 | WSL2 / Ubuntu 24.04 (noble), x86_64 |
| 编译器 | gcc 13.3.0；cmake 3.28.3；make 4.3 |
| 仓颉 SDK | **1.1.0**（`Cangjie Compiler: 1.1.0 (cjnative)`，target `x86_64-unknown-linux-gnu`） |
| `CANGJIE_HOME` | `~/cangjie/cangjie_1.1.0/cangjie`（`cjc` 在 `bin/`，`cjpm`/`cjfmt`/`cjlint`/`cjdb` 在 `tools/bin/`） |
| LVGL | **v9.2.2**（gitee 镜像，commit `c98ab243621a2a948674da5339c15da88832f928`） |
| SDL2 | 2.30.0（`libsdl2-dev`，apt/aliyun 镜像） |
| 构建目录 | `$HOME/lvgl4cj-build`（WSL 原生 FS）；ASan 单独用 `$HOME/lvgl4cj-build-asan` |

> 注：github.com 在本机不可达，LVGL 走 gitee 镜像拉取（`scripts/fetch_lvgl.sh` 已固化）。

---

## 1. 结论汇总

| 探针 | 问题 | 实测结论 | 判定 |
| --- | --- | --- | --- |
| **V1** | C 创建的 OS 线程能否执行仓颉代码？ | **能**。2000/2000 次调用全部成功，回调内堆分配正常（0 失败），C 线程 TID 1049 ≠ 主线程 TID 1044 | ✅ **方案 A 可行** |
| **V2-a** | 捕获闭包 → C 函数指针？ | **不能**。编译失败：`CFunc Lambda 不能捕获变量` | ✅ 预期失败 → **分支 B 唯一** |
| **V2-b** | C 侧长期持有 + GC 保活 + pin API？ | **不适用**（V2-a 已不通过，无需评估） | ⏭️ 跳过 |
| **V3** | 仓颉轻量级线程会迁移 OS 线程吗？ | **会**。2000 次 sleep(1ms) 迭代中观察到 **2 个**不同 OS TID（1554 / 1559） | ✅ 证实 M:N 迁移 |
| **V4** | ASan 与仓颉 GC / 工具本身有效吗？ | `LeakSanitizer` 精确报出人为的 1234 字节泄漏，栈追踪定位到触发点，退出码 1 | ✅ **工具有效** |
| **V5** | `cjpm.toml` 链接 C 库可行吗？ | 可行。`link-option = "-Llibs -llvgl4cj_bridge -llvgl ..."`，`cjpm build` + `cjpm run` 均成功 | ✅ 可行 |
| **V6** | SDL2 双线程同步 + 超时可行吗？ | 三项全过：正常往返 68ms；超时路径 rc=-1 / 100ms（**不挂死**）；关闭即时解锁 rc=0 / **1ms** | ✅ 可行 |
| **V7** | `deleted_cb` 三路径是否都触发？ | path1 自然结束 / path2 手动删除 / path3 对象删除(var=obj) **均触发**；path4(var=ctx) **不触发且动画残留** | ✅ Patch P1 成立 |
| V7-补充 | ADR-015 的代价有多严重？ | `var != obj` 时对象删除**既不停动画也不触发 deleted_cb**（`running 1->1`），动画会持续残留 | ⚠️ 必须手动补偿 |

---

## 2. 架构决策归档（依据实测）

| ADR / 风险 | 原状态 | 实测后归档 |
| --- | --- | --- |
| **ADR-013**（方案 A vs C） | Pending | **归档为方案 A**：C 侧 `pthread_create` 独立 OS 线程跑 LVGL 主循环（V1 通过）。性能指标按设计文档 §11.2「方案 A」档执行 |
| **ADR-014**（回调桥接分支） | 默认 B | **归档为分支 B 且为唯一可行路径**（V2-a 语言层不通过） |
| **ADR-015**（anim var = ctx） | 已冻结 | **保持不变，但代价已被实测量化**：必须由 DELETE 钩子手动调 `stop_all_anims_of_obj` 补偿；且 `lvglcj_anim_delete` 需要 `anim → ctx` 映射才能构造删除键 |
| **ADR-017**（anim_ctx 由内部 deleted_cb 唯一释放） | 已冻结 | **成立**（V7 三路径全触发） |
| **ADR-018**（pin 表必须调运行时 API） | 已冻结 | **归档为未采用**（pin 表随分支 A 一起废弃） |
| **R16**（分支 A 的 GC 保活失败） | 高 | **关闭**：分支 A 不实现，风险消失 |
| **R18**（anim_ctx 自然结束泄漏） | 高 | **关闭**：Patch P1 成立，无需 anim 句柄表兜底对账 |
| **R3**（ASan 与仓颉 GC 冲突） | 中 | **降级**：C 侧 ASan 已验证有效；仓颉侧不启用 ASan（§9.5 分层策略），未观察到冲突 |
| **R17**（flush_wait 与 postAndWait 死锁） | 高 | **缓解已验证**：V6 的「超时 + 关闭即时解锁」两条路径实测有效 |

---

## 3. ★ 本次实测发现的硬约束（设计文档未覆盖，影响实现）

这些约束是**实测踩出来的**，不是推测；每条都会直接改变实现方式。

### 3.1 仓颉语言层

| # | 约束 | 影响与对策 |
| --- | --- | --- |
| L1 | **`foreign` 声明不能带访问修饰符**：`public foreign func` 与 `protected foreign func` 均报 `modifiers conflict` | `foreign` 的可见性上限是默认的 `internal`（当前包及子包）。**必须放在所有使用者的公共祖先包**，故本项目把 FFI 声明放在**根包** `src/ffi_bridge.cj`（而非设计文档 §四 的 `src/ffi/bridge.cj`） |
| L2 | **`foreign` 声明跨模块不可见** | 独立模块（`probe/*`、`examples/*`）**必须自行声明**所需的 C 符号。这与设计立场一致：L1 只对外暴露包装好的公开 API，裸 FFI 不出模块 |
| L3 | **`CFunc` Lambda 不能捕获变量** | 分支 B 的根本原因。跨语言回调必须走「仓颉侧闭包表 + 顶层不捕获的 dispatcher」 |
| L4 | 整型字面量后缀必须显式宽度：`478319724u32`；**裸 `u` 非法** | 影响 `gen_conf_const.py` 的产出 |
| L5 | `mut` 是保留字，不能作标识符 | 命名注意 |
| L6 | `HashSet` 用 `add` 而非 `put`（设计文档 §3.6.4 示例此处有误） | 探针已修正 |
| L7 | 逻辑运算符是 `&&` / `||`，不是 `and` / `or` | — |
| L8 | `cjpm` 产出的可执行文件默认名为 **`main`**，不是包名 | 脚本里按 `target/release/bin/main` 定位 |
| L9 | Ubuntu 的 `~/.bashrc` 对非交互 shell **提前 return**（`case $- in *i*) ...`） | 非交互调用（IDE、脚本）下 `cjsc/cjpm` 不在 PATH → 必须有 `scripts/env.sh` 显式导出 |
| **L10** | **`match` 中 `case <具名 const> =>` 会被解释为「绑定模式」**（声明一个新变量，匹配任意值），第一个 case 永远命中，且编译器**不报错** | ★ 高危：会让「错误码映射」静默地全部返回第一个分支。本项目实测踩到（`lvglErrorFromCode(-1)` 返回了 `Ok`）。**解决办法：引用具名常量时必须用 `if/else-if`；只有字面量才是可靠的常量模式** |
| **L11** | **每个 `foreign` 声明都会在链接期产生符号引用**，即使从未被调用 | 声明必须与 C 库中真实存在的符号**严格一致**。本项目据此抓到 `lvglcj_set_log_cb` 与 `lvglcj_log_set_cb` 的名字漂移，以及 `lvglcj_drain_deferred` 声明早于实现。→ 建议 CI 增加「(foreign 声明集合) ⊆ (库导出符号集合)」校验 |
| L12 | `@Derive[...]` 需要显式 `import std.deriving.*`（测试模式下 `unittest` 会自动导入，但 `deriving` 不会） | 自定义枚举要在 `@Assert`/`@Expect` 中直接比较，须先派生 `Equatable` |
| L13 | `@Assert` **不支持字符串消息参数**（仅 `@Assert(a,b)` / `@Assert(cond)` / 带 `delta:`），且两侧类型必须完全一致 | `sizeOf<T>()` 返回 `UIntNative`，与 `Int64` 常量比较需显式转换 |

### 3.2 LVGL v9.2.2 层

| # | 约束 | 影响与对策 |
| --- | --- | --- |
| G1 | **对象必须有 display**：无 display 时 `lv_screen_active()` 返回 NULL，`lv_obj_create(NULL)` 触发 `Asserted ... obj != NULL` 并终止进程 | **headless 也必须先建 display**。§3.5 之外补一条硬前置；`backend_null` 不是可选优化而是必需 |
| G2 | **`lv_anim_t` 是公开结构体**（`var` / `user_data` 可直接访问），但**没有 `lv_anim_get_var()`** | Patch P1 的 deleted_cb **直接读 `a->var`** 取回 ctx。实现上**同时写 `a->user_data`** 做冗余，使 deleted_cb 不依赖 var 是否被清空 —— 比设计文档更稳健 |
| G3 | **`lv_anim_delete(void* var, lv_anim_exec_xcb_t exec_cb)` 按 (var, exec_cb) 键删除** | `var = ctx` 后，**必须先能找回 ctx 才能删动画** → 桥接层必须维护 `anim_handle → ctx` 映射（这原本是 Patch P1 的兜底方案，现成为必需基础设施） |
| G4 | **`LV_PROFILER_INCLUDE` 路径陷阱**：模板按「lvgl 目录在工程根下」写成 `"lvgl/src/misc/lv_profiler_builtin.h"`；用 `LV_CONF_PATH` 时该路径解析不到 | `gen_lv_conf.sh` 定点改为 `"src/misc/lv_profiler_builtin.h"`，并加了自检 |
| G5 | **`lv_conf_internal.h` 的回退默认值不可靠**：如 `LV_USE_DRAW_SW` 的 `#ifndef` 回退值是 **0**（等于没有软件渲染器） | **不能**用「最小覆盖式 `lv_conf.h`」。采用官方推荐的**模板 + 定点覆盖**，并在 `gen_lv_conf.sh` 里对每个覆盖项做命中自检 |
| G6 | `lv_display_flush_wait_cb_t` 是 `void (*)(lv_display_t *)`，**无超时参数**（§3.4.5 的 100ms 超时需在回调内部实现） | 与设计一致，已在 V6 验证 |
| **G7** | **`lv_event_set_user_data` 与 `lv_event_stop_trickling` 在 v9.2 都不存在**；且分支 B **已经用 `user_data` 装载 `closure_id`**，二者不可能共存 | ★ 设计文档 §5.6 的「param 放句柄 → 用 `lvglcj_event_get_user_data` 取回」**不可实现**。载荷必须走 **param**，故改为对称的两个入口 `lvglcj_event_get_param_handle` / `lvglcj_event_get_param_scalar`；`stop_trickling` 依真实语义改名为 `stop_processing`（对应 `lv_event_stop_processing`） |
| **G8** | **C 静态库只在被引用时才拉入目标文件** | ★ 「头文件声明了但未实现（或实现名字写错）」可以一路编译 + 链接通过，直到某个调用方真的引用它才在链接期爆炸（本项目实测：`event.c` 的两个隐式声明就是这么漏网的）。**对策**：`scripts/build_native.sh` 末尾新增 **ABI 符号存在性自检**，输出「声明集合 − 定义集合」的差集；该清单同时是一份精确的子系统进度表 |

---

## 4. 对设计文档的勘误清单

| 位置 | 原文 | 实测修正 |
| --- | --- | --- |
| §9.1 | `[ffi.c] path = [...]` / `clink = [...]` | 不是 cjpm 语法。正确做法：`[package] link-option = "-Llibs -llvgl4cj_bridge -llvgl ..."` + `[target.<triple>]`（照抄本机可用的 `CJQT6/cjpm.toml`） |
| §3.2.4 | 分支 A（闭包指针 + pin 表）作为备选 | **语言层即不成立**（L3），已归档 |
| §5.1 | `lvglcj_set_log_cb(int32_t cid)` | 闭包表签名是 `(Int64) -> Unit`，**装不下日志字符串**；改为与错误回调一致的函数指针注册（见 `lvglcj_error.h`） |
| §3.6.4 | 探针示例 `tids.put(...)` | `HashSet` 无 `put`，应为 `add`（L6） |
| §3.10.2 | 「`ctx = lv_anim_get_var(a)`」 | 该函数不存在（G2）。直接读公开成员 `a->var`，并用 `a->user_data` 冗余 |
| §3.10.3 | 兜底方案「anim 句柄表」标注为仅在 V7 失败时启用 | V7 通过（Patch P1 成立），但 **G3 使 `anim_handle → ctx` 映射成为必需**（删除动画需要它） |
| §5.6 | 「param 双路径 + `lvglcj_event_set_user_data` / `lvglcj_event_get_user_data`」 | **不可实现**（G7）：`user_data` 被分支 B 占用于 closure_id，且 v9.2 无运行期设置接口。改为 `lvglcj_obj_send_event`（句柄路径）+ `lvglcj_obj_send_event_scalar`（标量路径）两个显式入口，读取侧对称提供 `get_param_handle` / `get_param_scalar` |
| §5.6 | `lvglcj_event_stop_trickling` | v9.2 无此 API；依真实语义改名为 `lvglcj_event_stop_processing`（对应 `lv_event_stop_processing`） |
| §3.3.2 | 「clean 标志使子失效、父不失效」 | clean 标志用**保存-恢复**实现（支持嵌套），但需澄清：钩子在**任何路径下都不会失效父对象**，因此「clean 后父仍存活」是自然结果；标志的实际用途是给删除栈打标注（区分 `obj_delete` / `obj_clean`），便于 dump 时定位「谁删了我」 |
| §四 | `src/ffi/bridge.cj` | 改为 `src/ffi_bridge.cj`（根包），原因见 L1 |

---

## 5. 复现方式

```bash
# 一次性准备（装 SDL2、拉 LVGL、生成 lv_conf.h）
sudo apt-get install -y libsdl2-dev
bash scripts/fetch_lvgl.sh
bash scripts/gen_lv_conf.sh

# 构建桥接层 + 探针 + 生成仓颉常量
bash scripts/build_native.sh --probes

# 跑全部探针并输出汇总表
bash scripts/run_probe.sh

# 只跑个别探针
bash scripts/run_probe.sh v1 v7
```

---

## 6. C4 缺陷：在回调帧内构造异常导致 SIGSEGV

### 6.1 结论（根因已定位，完整报告见 `docs/C4_DEFECT.md`）

**根因**：崩溃发生在**仓颉 1.1.0 运行时的异常栈回溯采集**路径上 ——
`Exception::<init>` → `FillInStackTrace()` → `StackInfo::AnalyseAndSetFrameType()` → `FrameInfo::ResolveProcInfo()`。
栈上某个返回地址的高 32 位被破坏为 `0xfffffffc`（低 32 位仍是合法代码地址），
运行时据这个非法 PC 去解引用而 SIGSEGV。

**触发条件**：在**「C → 仓颉」回调帧之内构造异常对象** ——
即用户回调里直接写 `throw LvglException(...)`（`throw` 会先构造，构造时采集栈回溯）。

**影响面**：这是**平台限制**，不是本项目桥接层逻辑的缺陷。
我们的闭包表 / 分发器 / 异常边界捕获经独立程序五场景验证全部正确。
但因此 **P0 门禁「回调抛异常不导致进程崩溃」在仓颉 1.1.0 上无法完全达成**，
必须改为：「回调内的失败通过返回码或 `onError` 上报；框架保证异常**传播**不跨越 C 边界；
在回调帧内**构造**异常属已知平台限制，需在用户指引中禁用。」

**已验证的规避方式**（规避后测试套件 28 PASSED / 0 ERROR，两次运行稳定）：
把异常对象的构造移到回调帧**之外**，回调内只 `throw` 已构造好的对象；
更推荐的做法是在回调中用返回码或 `onError` 上报失败，完全不构造异常。

### 6.2 历史排查记录（以下候选假设**均已实测证伪**，保留以供复核）

**现象**：`cjpm test` 中单一用例 `ERROR: Crashed with exit code 11`，
耗时约 2.4–3.3 s 后崩溃；其余 27 个用例全部通过。

**最小复现（当时的写法）**：

```cangjie
let cid = registerClosure { _ => throw LvglException(OutOfMemory, "boom") }
let _ = unsafe { lvglcj_call_closure(cid, 1) }   // 第 1 次抛异常 → 被吞并 → 返回 -4
let _ = unsafe { lvglcj_call_closure(cid, 1) }   // 第 2 次 → 崩溃
```

**已排除**：
- 不是「抛异常」本身 —— 单次抛出被正确吞并，返回 `-4`，`closureSwallowedExceptionCount` 与
  C 侧 `CALLBACK_THROWN` 计数一致（`testClosureExceptionIsSwallowed`、
  `testExceptionCountMatchesCCounter` 均通过）
- 不是线程亲和性 —— 该问题曾表现为**非确定性**（同一用例两次运行结果不同），
  加 `bindCurrentThreadAsLvgl()`（每用例重新声明 LVGL 线程归属）后
  **非确定性消失**，但此崩溃**变为稳定复现**。这是两个独立问题，前者已修。

**两个候选触发条件（尚未区分）**：
- (a) **同一 cid 在抛异常后再次被分发**
- (b) **连续两次抛出异常**（每次都会走一遍 `eprintln` + 错误返回路径）

把用例改成「不同 cid、只抛一次」后 28/28 通过 —— 但这次改动**同时变了两个变量**，
因此只能说明「该组合无害」，**不能判定 (a) 与 (b) 谁是元凶**。

**下一步隔离方法**（各写一条独立用例，一次只变一个变量）：

| 用例 | 设计 | 若失败则结论 |
| --- | --- | --- |
| A | 同一 cid，抛一次 → 再分发**正常返回**的闭包 | (a) 指向「异常后同一 cid 重入」 |
| B | **不同 cid**，各自抛一次（共两次异常） | (b) 指向「连续抛出」 |
| C | 同一 cid，抛一次 → 再分发**同一个抛异常的闭包**（即原用例） | 复现基线 |

**为什么必须查清而不是绕过**：生产环境中「用户回调抛异常」是**必然发生**的
（`onError` 机制就是为它设计的），而同一个回调会在下一次事件到来时**再次被调用**。
若这一组合真的会崩溃，那就是 P0 门禁指标「回调抛异常不导致进程崩溃」的**直接违反**（§11.1）。
在查清并修复之前，不应据此宣称该门禁已达成。

**建议的排查工具**：`cjdb`（仓颉调试器）定位崩溃点，或用
`ASan 构建 + 逐个用例运行`对比栈信息（ASan 对仓颉运行时可能不适用，见 §9.5 分层策略）。

---

## 7. 下一步

V1–V7 已全部出结论，按设计文档 §12 的依赖图，**可以开始写业务代码**：

1. **t3** 生命周期与回调：DELETE 钩子四步顺序、`clean` 保存-恢复标志、trampoline + 闭包表、延迟删除队列
2. **t4** 对象树 / display（C 侧缓冲分配，且**必须提供 headless display**，见 G1）/ indev / 任务队列 / 线程装配（方案 A 已定）
3. **t5** SDL2 后端（V6 已验证机制，按 §8.1.2 实现）
4. **t6** `hello_cj` 全链路 + 八条断言
5. **t7** anim_ctx（含必需的 `anim_handle → ctx` 映射，见 G3）

---

## 8. t4 落地记录（display / indev / 任务队列 / 线程双轨）

### 8.1 交付与验证

| 产出 | 说明 | 验证 |
| --- | --- | --- |
| `native/src/queue.c` | 有界任务队列 + 等待图死锁检测（§3.9 / §3.8.4） | `test_queue`：57 项 |
| `native/src/display.c` | 四态句柄 + **C 侧缓冲分配** + T4 flush/flush_wait trampoline | `test_display_indev`：70 项 |
| `native/src/indev.c` | indev + group + T5 输入回填 | 同上 |
| `backend/null/null_backend.c` | **headless display（G1 必需项）** + 确定性推帧 | `test_null_backend`：20 项 |
| `src/queue.cj` / `src/display.cj` / `src/runtime.cj` | L1 任务投递、display 包装、`LvglRuntime`（方案 A/C 双轨） | `cjpm test` 新增 8 例 |

合计：C 侧 180 项检查、仓颉侧 37 例（36 通过 / 1 跳过 = C4 靶子），全部 0 失败。

**t4 完成度的客观证据**：`scripts/build_native.sh` 的 ABI 符号自检（G8）报告剩余
**94 个「已声明未实现」符号，全部属于 t6（控件/样式）、t7（动画）、t8（调试）**；
t4 的子系统（display / indev / group / queue / thread）**一个都不在清单里**。

### 8.2 本阶段新发现的契约问题（已修正）

| # | 问题 | 影响 | 处理 |
| --- | --- | --- | --- |
| N1 | `lvglcj_indev_type_t` 把 **BUTTON/ENCODER 写反**（我们 3=ENCODER/4=BUTTON，LVGL 3=BUTTON/4=ENCODER） | 编码器被当按钮处理，**不报错**、行为诡异 | 逐值对齐 LVGL（不再需要映射表），并在头注释写明「新增类型先去 `lv_indev.h` 核对，不要凭记忆续写」 |
| N2 | `lvglcj_timer_handler()` 返回的是 `lv_timer_handler()` 的「距下次到期毫秒数」 | 违反 ABI「正数仅 +1 为特殊标记」的约定；返回 30 会被当特殊标记、返回 0 会被当成功，**两种解读都错且静默** | 改为返回错误码；下次间隔经新增的 `lvglcj_timer_next_ms()` 单独查询 |
| N3 | 删除类 API 的幂等性职责不统一（`obj_delete` 严格，`display_delete` 起初写成宽容） | 仓颉侧 `close()` 需按子系统分支实现，迟早漏一个 | 在 `lvglcj_bridge.h` §5.2 明确**分层规则**：C 侧一律严格返回 `INVALID_HANDLE`，幂等由仓颉 L1 `close()` 承担（把 `INVALID_HANDLE` 视为成功），三个子系统统一 |
| N4 | headless 场景下 flush 观测与「不绕过 trampoline」冲突 | 若 headless 后端自己挂 `lv_display_set_flush_cb` 会绕过 T4，导致「headless 测过」推不出「SDL2 也对」 | null 后端**不注册 flush 回调**：display.c 的 trampoline 在无 cid 时自动 `flush_ready`；需要观测时由仓颉侧照常注册 cid，**测试因此仍覆盖 T4 全链路** |

### 8.3 语言层约束（对用户 API 有直接影响）

**仓颉禁止「捕获可变局部变量的 lambda」逃逸**：

```cangjie
var n = 0
rt.post { n = n + 1 }        // ✗ 编译失败：
                             //   lambda capturing mutable variables needs to be called directly

let n = ArrayList<Int32>()   // ✓ 捕获「let 绑定的可变容器」
rt.post { n.add(1) }
```

`post { }` 的闭包必然逃逸（要入队、稍后在另一帧执行），因此用户**无法**用
「捕获 `var` 局部变量」的方式回传结果。该约束已写入 `LvglRuntime.post` 的文档注释。
（仓颉侧其它易错点：无 `object` 声明、无 `toInt64()` 成员方法、枚举默认不支持 `==`
需 `@Derive[Equatable]`、`ArrayList` 用 `add` 而非 `append`、默认参数须写成 `p!: T = v`。）

### 8.4 两套线程方案的等价性保证（t4）

方案 A（C 侧 OS 线程）与方案 C（调用方驱动）的**帧内顺序完全一致**——
`延迟删除 → 任务队列 drain → lv_timer_handler`，由 `thread.c` 与 `pump()` 共用同一份实现。
因此「headless / 泵模式下达成的时序结论」对方案 A 具有参考价值，
而不是两套彼此无关的语义。另：`startPumped()` / `startHeadless()` 会显式
`lvglcj_rebind_current_thread()` —— 因为 `lvglcj_init()` 幂等不会重绑线程，
而 M:N 下「这次调用的线程」很可能不是「首次登记的那个」（V3 结论的直接后果）。

---

## 9. t5 落地记录（SDL2 桌面后端）

### 9.1 交付与验证

| 产出 | 说明 | 验证 |
| --- | --- | --- |
| `backend/sdl2/sdl2_backend.c` | 窗口/渲染器/纹理、手写 flush/read、有界等待、即时解锁 | `test_sdl2_backend`：31 项 |
| `native/src/display.c` 新增 **flush sink** | C 函数指针钩子，让后端拿到 `(area, px_map)` 而不把缓冲交给仓颉 | 同上 |
| `src/sdl2.cj` | L1 薄包装（错误翻译 + 不暴露像素缓冲） | `cjpm test` 新增 2 例（**真实执行**，非跳过） |

**实测时序数据**（SDL_VIDEODRIVER=dummy，软件渲染）：

| 路径 | 期望 | 实测 |
| --- | --- | --- |
| 跨线程正常往返 | ≪ 100ms | **2 ms** |
| 渲染方不响应 | 约 100ms 后返回，**不挂死** | **103 ms** |
| 一次超时后恢复 | 往返恢复正常 | **0 ms** |
| 窗口关闭后 flush | **立即**返回 | **0 ms** |

后两项即 P0 断言 7「窗口关闭时 flush_wait_cb 立即返回，不挂死」的机制证据。

### 9.2 三个必须写下来的实现决定

| # | 决定 | 理由 |
| --- | --- | --- |
| S1 | **flush 按「当前线程是否渲染线程」分流**：是 → 内联渲染；否 → 投递事件 + 有界等待 | §8.1.2 的流程（投递 → 主线程渲染 → 信号量）**只在双线程下成立**。若渲染线程就是 LVGL 线程（方案 C、以及绝大多数自动化测试），flush 投递完请求再等待，而唯一能处理该请求的线程正卡在等待里 → 必然超时，**每帧白等 100ms**，帧率塌成 10 FPS。V6 探针用两条线程专测，因此没有暴露这个缺口 |
| S2 | 像素格式映射不上时**明确拒绝**（`SDL_PIXELFORMAT_UNKNOWN`），不兜底猜测 | 猜错的后果是花屏，而花屏**不报错、不崩溃**，只会被误认为是渲染逻辑写错。因此 RGB888 有意列为不支持：LVGL 的 RGB888 与 SDL 的 RGB24 字节序可能不同，要支持须先实测字节序 |
| S3 | 每次等待都有界，且**无论成功还是超时都必须 `flush_ready`** | 超时不放行会让 LVGL 永久停在「等 flush 完成」→ 界面卡死。丢一帧 + 记 `BACKEND_FAILURE` 远好于卡死 |

### 9.3 与设计文档的偏差

| 项 | 设计文档 | 实现 | 原因 |
| --- | --- | --- | --- |
| flush 的像素获取 | 未说明仓颉侧如何取得 `px_map` | 新增 **C 侧 sink 钩子**，仓颉侧完全不接触像素 | 若把 `px_map` 交给仓颉侧会违反 ADR-002（GC 会移动缓冲）；sink 让后端在 C 侧直接拿到 `(area, px_map, stride)` |
| flush 与 flush_wait 的职责 | flush 投递、flush_wait 等待 | flush 自身完成「上传 + 有界等待 + 放行」；flush_wait 只在有在途请求时等待 | 避免两处都等待导致每帧双倍等待；flush_wait 仍保留并实现，以满足 §8.1.2 与断言 7 |
| 渲染线程身份 | 未提及 | 登记给 §3.8.4 等待图判据 3 | 渲染线程同步等待 LVGL 任务必然与 flush 等待成环，应在**进入等待前**拒绝而非等死锁 |

---

## 10. t6 落地记录（hello_cj 与八条断言）

### 10.1 交付与断言状态

| 产出 | 说明 |
| --- | --- |
| `native/src/style.c` | 53 个 setter + `obj_add_style/remove_style/get_style_prop` |
| `native/src/widgets.c` | label / button（与 obj 同样登记句柄并挂 DELETE 钩子） |
| `native/src/font.c` | 内置字体句柄化 + binfont 加载；**拒绝释放内置字体** |
| `src/{object,style,event,widgets,indev,font}.cj` | L1 对象/样式/事件/控件/输入/字体 |
| `examples/hello_cj` | 800×480 深色窗口 + 居中圆角按钮；`--smoke` 支持有界自动化运行 |

| 断言 | 状态 | 证据 |
| --- | --- | --- |
| ① 主循环稳定不崩 | 部分 | 本仓有界 soak（600 帧，闭包表回基线、错误计数不增长）；完整 10 分钟长跑留给 t8 脚本 |
| ② flush 被调用且画面正确 | ✅ | flush 计数 > 0 + **像素级体检**（distinct>1 非纯色、nonzero>0 非全黑） |
| ③ 点击命中且坐标正确 | ✅ | 走**真实输入链路**（pointer indev 读回调 → 命中测试 → 事件分发），并断言 target 就是按钮 |
| ④ 删父后子句柄失效 | ✅ | C 侧 `test_display_indev` / `test_style_widgets`（含控件）+ 仓颉侧等价用例 |
| ⑤ ASan 零泄漏 | ⬜ | t8 |
| ⑥ 回调中 close 自身 + 同帧删除 | ✅ | 仓颉侧用例（回调内 `btn.close()` → 延迟删除 → 帧内生效） |
| ⑦ 关窗时 flush_wait 立即返回 | ✅ | C 侧 `test_sdl2_backend` 实测 **0ms** |
| ⑧ repeat(1) 动画后 ctx 释放 | ⬜ | t7（动画子系统） |

**总计**：C 侧 6 个单测共 **254 项检查 0 失败**；仓颉侧 **43 通过 / 1 跳过 / 0 失败**，连跑 5 轮稳定。

### 10.2 本阶段发现的真实缺陷（均已修正）

| # | 现象 | 根因 | 修法 |
| --- | --- | --- | --- |
| A4 | 中文完全不显示，日志刷 `glyph dsc. not found for U+6211` | LVGL 内置 Montserrat **只有拉丁字形** | 开启 `LV_FONT_SIMSUN_16_CJK` + `font.c` 句柄化内置字体；**未开启时明确报错而非静默回退**。★ 但该字库**不是常用汉字表、有缺字** —— 本条当时的描述有误，见 §14 |
| A5 | 日志被 `tracing_mark_write` 淹没，真实 WARN 不可见 | `LV_USE_PROFILER=1` 时内置 profiler 对每次布局/绘制写一行 trace mark | 默认关闭 profiler（它是调试辅助，需配 trace 消费者）。★ 另发现 `LV_USE_PROFILER` 与 `LV_USE_PROFILER_BUILTIN` **耦合**：开前者关后者会导致 LVGL 自身编译失败（`LV_PROFILER_BUILTIN_END undeclared`） |
| A6 | 退出时 `display 删除返回非成功`，**界面看着正常但 display 与缓冲未释放** | 方案 A 下 `startThreaded()` 把 LVGL 线程身份交给了 C 循环线程；该线程 join 后，主线程再删 display 就**不是 LVGL 线程** | `LvglRuntime.stop()` 在停循环后把身份**交回调用方**（此时已无并发者，语义等价于方案 C 的「谁驱动谁是 LVGL 线程」） |
| A7 | 断言⑥用例偶发 `WrongThread`（5 轮中 1 轮） | **M:N 迁移**（V3）：逻辑主循环没变，但底层 OS 线程在任意一次阻塞调用后被换掉，而用户无法预知发生在哪一行 | 库级兜底：泵模式下在 `pump()` 与所有 L1 创建/关闭入口调用 `ensureLvglOwnership()` 重新确认归属。**不能推给用户** —— 迁移不是用户能规避的 |
| A8 | 点击完全没反应 / `containsPoint` 为 false | ① LVGL 的坐标在 `lv_timer_handler` 里才结算，刚 `setSize` 完 `width()` 仍为 0；② indev 读取周期约 30ms，按下/松开时长太短会一次都没读到 | 测试先推进若干帧让布局结算再读几何；按下/松开时长覆盖 indev 读取周期 |

### 10.3 v9 与 v8 的三处 API 差异（照 v8 写会静默出错）

| 项 | v8 | v9 | 写错的后果 |
| --- | --- | --- | --- |
| style setter 的 selector | `lv_style_set_bg_color(s, state, color)` | `lv_style_set_bg_color(s, color)`；selector 只在 `lv_obj_add_style` 给出 | 设置了「不存在的状态」→ **样式完全没效果且不报错** |
| `lv_obj_remove_style` 返回 | `bool`（是否移除成功） | `void` | 无法区分「移除了」与「本就没挂」；本层不假装有返回值 |
| `lv_label_long_mode_t` 末项 | — | `CLIP` 排在**最后**（不是 `SCROLL_CIRCULAR`） | 上界写错会把合法的 `CLIP` 误判为非法参数 |

### 10.4 本阶段新增的仓颉语言约束（已写入对应代码注释）

| 约束 | 说明 |
| --- | --- |
| **命名参数必须按名传** | `p!` 声明的参数不能按位置传，否则报 `missing argument prefix 'p:'` |
| `prop` 是关键字 | 不能作参数名（`styleProp(propId: Int32)` 因此改名） |
| `String` ≠ `CString` | 不隐式转换，需 `LibC.mallocCString` 并**自行释放**（已封装 `withCString/withCString64`） |
| 逃逸闭包不能捕获 `var` | `post { n = n + 1 }` 编译失败；须用 `let` 绑定的可变容器 |
| 无 `object` 声明 | 常量用顶层 `public const`（如 `LV_COLOR_FORMAT_*`） |
| 枚举默认不支持 `==` | 需 `@Derive[Equatable]` |

### 10.5 一处 ABI 设计收窄（t6）

`lvglcj_display_sample_buf` 用于「画面非纯色/非全黑」的断言，改为**打包返回值**
（高 32 位 = 不同像素数，低 32 位 = 非零像素数，负数=出错），而不是两个出参 ——
出参要求仓颉侧构造 `CPointer` 并写指针，容易引入指针算法错误；而两个值都是非负计数，
打包后「负数=出错」无歧义。

---

## 11. t7 落地记录（anim_ctx / timer / 泄漏对账）

### 11.1 交付与验证

| 产出 | 说明 | 验证 |
| --- | --- | --- |
| `native/src/anim.c` | 13 个 anim ABI + `anim_ctx` 完整生命周期 + T3 trampoline | `test_anim`：**69 项** |
| `src/anim.cj` | `LvAnim` DSL（target/values/duration/path/repeat/playback/onExec/onStart/onDeleted） | `cjpm test` 新增 5 例 |
| `lvglcj_anim_path_t` | 补齐契约缺口：`set_path` 的取值此前**未定义** | C 测试覆盖 7 种曲线的编号校验 |

**断言⑧「`repeat(1)` 动画跑完后动画上下文被释放」—— 已在 C 与仓颉两侧独立验证。**

汇总：C 侧 7 个单测共 **323 项检查 0 失败**；仓颉侧 **48 通过 / 1 跳过 / 0 失败**，两轮稳定。
ABI 未实现符号 **27 → 14**（余下 7 个 canvas 属 P1，7 个 debug/fs/mem/perf 属 t8）。

### 11.2 三条释放路径与那条「不会走回调」的路径

`anim_ctx` 由 C 侧 internal deleted_cb **唯一释放**（ADR-017 / Patch P1），
V7 实测的三条路径都会触发它：

| 路径 | 触发方式 | 实测 |
| --- | --- | --- |
| path1 自然结束 | `repeat(1)` 跑完 | ✅ ctx 归零、三个闭包均被注销 |
| path2 手动删除 | `LvAnim.close()` | ✅ 同上，且 `onDeleted` 被通知一次 |
| path3 目标对象被删 | `obj.close()` → DELETE 钩子步骤 (c) | ✅ 两个无限循环动画都被停掉 |

★ **但还有第四条路径不会走 deleted_cb**：「**start 之前就删除**」——
动画从未进入 LVGL 的管理，回调永远不会被调用。
若只按「deleted_cb 是唯一释放点」实现，这条路径会**稳定泄漏一个 ctx**。
因此 `lvglcj_anim_delete` 对「未启动」单独处理，自己负责释放 ctx 与注销闭包。
单测 §5 专门覆盖了它。

### 11.3 ADR-015 的代价补偿（已验证生效）

`var = ctx` 使 LVGL「按 `var == obj` 自动停动画」失效（V7：`running 1->1` 残留）。
补偿点在 `lifecycle.c` 的 DELETE 钩子步骤 (c)：`lvglcj_stop_all_anims_of_obj`。
单测 §3 用**两个无限循环动画**（`repeat(0)`）验证：若补偿缺失，它们会一直残留；
实测删除对象后 `anim_count_running()` 与 `anim_ctx_count()` **同时归零**。

### 11.4 把三处「静默失效」变成明确报错

| 场景 | 若不拦会怎样 | 处理 |
| --- | --- | --- |
| start 之后再 `set_*` | `lv_anim_start` 会**拷贝**模板，此后改本地模板毫无效果 —— 「设了但没生效」，不报错 | 置 `started` 标志，之后再 set 返回 `INVALID_ARGUMENT` |
| 未设 exec_cb 就 start | 动画「跑着但什么都不做」，且删除键 `(var, exec_cb)` 失去意义 | start 前校验，缺失即拒绝 |
| 非法 path 编号 | 契约里 `set_path` 的取值原本**未定义**，调用方只能猜 | 补齐 `lvglcj_anim_path_t`（0..6 映射到 `lv_anim_path_*`），越界即拒绝 |

### 11.5 泄漏对账的正确判据（写进代码注释）

`lvglcj_anim_ctx_count()` 与 `lvglcj_anim_count_running()` 的关系**不是恒等**：

- **稳态（无动画在跑）时两者必须都为 0** ← 这才是「零泄漏」的判据；
- 动画进行中 `ctx_count >= running`，因为 **delay 尚未到期的动画不计入 running，
  但它的 ctx 已经存在**。

因此对账断言写成「稳态双零」，而不是「两者恒等」——后者会得到一个
「平时对、某些时刻莫名其妙不对」的脆弱断言。

### 11.6 跨语言注销协议的验证方法

断言「ctx 归零」只能证明 **C 侧**释放干净。若仓颉侧的 exec/start/deleted 闭包
还留在闭包表里，那只是把泄漏换了个地方。因此仓颉侧用例同时断言：

```
let baseClosures = closureCount()   // 动画创建之前
... 动画跑完 ...
@Assert(closureCount(), baseClosures)   // ★ 闭包表也回到基线
```

这一条同时验证了 C 侧 `LVGLCJ_ARG_UNREGISTER` 协议与仓颉侧 `dispatchClosure`
的注销分支配合正确 —— 是「两侧任一改动都会立刻暴露」的那种断言。

---

## 12. t8 落地记录（ASan 零泄漏 / cjlint+cjfmt 门禁 / 基线文档）

### 12.1 交付

| 产出 | 说明 |
| --- | --- |
| `scripts/gate.sh` | 统一门禁：cjfmt / cjlint / cjpm build / cjpm test / ctest / ASan（含工具自证）。**7 项全绿** |
| `config/cjlint_rule_list.json` | 项目规则清单（54 条），含两条文件命名规则的排除理由 |
| `test/native/test_leak.c` | 泄漏门禁：1 万次 create/delete 的 ALIVE 增量、差值恒定、级联保留量有界性 |
| `native/src/debug.c` | 可观测性 ABI：`obj_count` / `mem_monitor` / `perf_sample` / `dump_tree` / `fs_init_posix` |
| `docs/benchmarks/2026-09-20_P0_gate.md` | §11.5 要求的基线记录（环境 + 原始数据 + 复现命令） |

**ABI 未实现符号：27 → 8**（余下 8 个全部是 Canvas，属 P1）。

### 12.2 门禁抓出并修掉的 4 个缺陷

门禁的价值是**发现问题**，不只是贴绿灯。t8 期间实测抓出：

| # | 问题 | 发现方式 | 性质 |
| --- | --- | --- | --- |
| 1 | `lvglcj_style_delete` 释放仍被存活对象引用的样式 | ASan `heap-use-after-free` | **静默内存破坏**：不一定崩，可能只是尺寸/颜色错乱 |
| 2 | flush 测试缓冲按 area 紧凑尺寸分配，却把 display 的 stride 交给 SDL 按行读 | ASan `global-buffer-overflow` | 越界读 128 字节 |
| 3 | 父对象删除时 `INVALIDATED` 子表项**无上限增长**（10 条/轮、完全线性） | `test_leak` 曲线 | 直接违反 §11.1 soak 判据「句柄收敛」 |
| 4 | `ht_release_unlocked` 对 `INVALIDATED` 表项也摘反查表，而该地址已被新对象复用 | 引入 #3 的回收后由门禁②抓出 | **既有代码的潜伏缺陷**：误删新对象的反查项 → 重复句柄 → ALIVE 单调增长 |

其中 #1 与 #4 尤其值得记下：**两者在普通构建下都不会立刻出错**。
#1 表现为「尺寸/颜色偶尔不对」，#4 表现为「ALIVE 计数缓慢漂移」——
都是最难从现象反推原因的形态，也正是「必须有自动化内存/泄漏门禁」的理由。

### 12.3 三处「静默失效」被改成明确报错

延续 C4 与 t7 的同一条原则（可报的错绝不静默）：

| 场景 | 原行为 | 现行为 |
| --- | --- | --- |
| 释放仍被引用的样式 | 静默释放 → 后续 UAF | 拒绝释放，报出「仍被 N 个对象引用」 |
| `obj_add_style` 登记失败 | —— | **回滚**已生效的 add，避免「LVGL 认为挂着、守卫认为没挂」的分歧 |
| `lvglcj_fs_init_posix(root)` 传非 "." 的 root | —— | 返回 `NOT_SUPPORTED` 并说明「运行时换根需自定义驱动（P2）」 |

最后一条是刻意的：`LV_FS_POSIX_PATH` 是编译期常量，LVGL 没有运行时换根的接口。
假装支持只会让用户面对「文件读不到但也不报错」的谜题。

### 12.4 ASan 与仓颉运行时硬不兼容 —— 对 §9.5 原方案的有据修正

§9.5 原计划让 ASan 覆盖混合进程 + 用 suppression 屏蔽仓颉运行时符号。实测走不通：

```
AddressSanitizer: CHECK failed: sanitizer_thread_arg_retval.cpp:56 "((t)) != (0)"
    #4 pthread_join                  (ASan 拦截器)
    #5 CJ_ScheduleAllNonDefaultExit  (libcangjie-runtime.so)
```

仓颉运行时的线程由自身机制创建，不在 ASan 的线程簿记里，
退出时 `pthread_join` 的一致性检查必然失败并中止进程。
**`detect_leaks=0` 也避不开**（该簿记不受泄漏检测开关控制）。

**修正后的边界**：ASan 只跑纯 C 进程；混合进程用 valgrind（§9.5 第 5 点的既定回退）。
这个边界不是妥协 —— 本层所有手工内存管理都在 C 侧（四态句柄表、闭包表、
延迟删除队列、anim_ctx、任务队列、显示缓冲、样式使用者表），C 单测会走遍它们。

valgrind 侧实测（`hello_cj --smoke`）：98 123 条报告，但**栈中含 `native/src`
或 `backend/` 的为 0 条** —— 全部来自仓颉运行时的 GC 保守扫描。
判定命令见 `docs/benchmarks/2026-09-20_P0_gate.md` §3.4。

★ 这同时**修正了 V4 探针结论的适用范围**：V4 证明的是「LeakSanitizer 能报出
人为泄漏」（工具有效），而不是「ASan 可用于混合进程」。前者成立，后者不成立。

### 12.5 cjfmt / cjlint 的收敛（77 → 0）

`cjfmt` 没有 `--check` 模式，因此门禁里的「零 diff」用**格式化到临时目录再比对**实现
（注意 `-o` 会再套一层与源目录同名的子目录）。当前 **0 个文件待格式化**。

`cjlint` 从 **77 条收敛到 0 条**，逐类处理如下：

| 规则 | 条数 | 处理 |
| --- | --- | --- |
| `G.PKG.01` 通配导入 | 26 | **修**：逐文件换成显式导入（并借机发现 `p0_assert_test` / `sdl2_test` 的两处导入**本就未使用**，直接删除） |
| `G.ERR.01` 未在注释中说明异常 | 24 | **修**：为 22 个公开 API 补 `@throws[LvglException] ...`；余 2 处是 `@Derive` 宏展开的误报 |
| `G.FUN.01` 函数过长 | 2 | **修**：`lvglErrorFromCode` 改用查表；用例抽出 3 个辅助函数（同时把「推进多少帧」变成显式参数） |
| `G.NAM.05` 不可变全局命名 | 4 | **修**：改为全大写（`G_CLOSURE_MUTEX` 等） |
| `G.EXP.03` 短路表达式右操作数含副作用 | 2（**error 级**） | **修**：`containsPoint` 先把几何值取到局部变量 —— 既是规则要求，也消除了每次命中测试的重复 FFI 调用 |
| `G.VAR.02` 作用域最小化 | 4 | 1 处**修**（就地映射）；3 处**行级屏蔽**并写明理由（顶层 `CFunc` 必须是稳定对象、常量表只初始化一次、进程级泵模式标记） |
| `G.OPR.01` / `G.ERR.01`（宏展开） | 20 + 2 | **行级屏蔽**：`@Derive[Equatable]` 生成的 `==`/`!=` 非手写代码，规则必然误报 |
| `G.NAM.02` / `G.NAM.01` 文件命名 | 13 + 1 | **规则级排除**（理由见 `config/cjlint_rule_list.json`）：按子系统分文件、与 C 侧同名是刻意选择；多级包名在单级目录下物理不可满足 |

★ 一个值得记下的坑：`cjlint` 通过而 `cjc` 不通过的例子确实出现了 ——
`import std.sync.{Mutex, synchronized}` 里 `synchronized` 是**语言关键字**（本就无需导入），
cjlint 未报，编译器报 `found keyword 'synchronized'`。
**所以「静态检查零告警」不能替代「编译通过」，两者都要在门禁里。**

★ 另一个：`cjlint -c` 的参数语义是「**包含** `config/` 的目录」，且会要求该目录下有
**全部**规则配置文件（缺一个就 `open json file failed`）。因此 `gate.sh` 在临时目录里
把 SDK 配置整体拷一份、再用本项目的规则清单覆盖 —— 仓库只保留「我们的取舍」这一个文件，
不必 vendor 工具配置，也不会随 SDK 升级留下过期副本。

### 12.6 泄漏对账的两个口径（写进代码注释，避免将来误判）

1. **句柄层面**：`ALIVE` 增量必须为 0；被**直接删除**的对象表项回收（`UNINIT`），
   因**父对象被删**而失效的子句柄保留表项（`INVALIDATED`）——后者是刻意的，
   为的是能报出「原生对象已删除」并保留删除栈。
2. **保留量必须有界**：既然 INVALIDATED 表项保留，就必须有上限。
   实测 2 万条累计失效而在册量峰值 1 023（上限 1 024），平台化成立。

### 12.7 未做与已知限制

- 未做 24h soak、未做多平台矩阵（仅 ubuntu-x64）、未测性能指标（§11.2 需目标平台）。
- `perf_sample` 的 FPS/CPU 是**本层自己埋点**计算的（`LVGL` 官方 perf monitor 无公开取数 API），
  口径已在 `native/src/debug.c` 文件头写明：`cpu_percent` 是「主循环忙碌比」而非进程 CPU 占用率。
- `obj_count` 遍历 active screen 子树 + 默认 display 的三个 layer 并去重；
  **非默认 display 的 layer 无法通过公开 API 取得**，多 display 场景下会少算（已知口径限制，已注释）。

---

## 13. t8 后续（性能指标 / soak / 平台矩阵）

### 13.1 交付

| 产出 | 说明 |
| --- | --- |
| `src/debug.cj` | `LvDebug`（§7.3 的仓颉侧包装）：`objCount` / `memMonitor` / `perfSample` / `dumpTree` / `fsInitPosix` |
| `src/clock.cj` | 单调微秒时钟（性能测量时基，与 C 侧同源） |
| `src/bench.cj` | `LvBench`：性能测量与压力负载。**以数据返回而非打印** |
| `src/perf_test.cj` | §11.2 的**回归护栏**（宽松上界，纳入 `cjpm test`） |
| `examples/bench_cj` | 命令行采集工具：`perf` / `startup` / `soak --seconds N` |
| `scripts/perf.sh` | 指标采集 + 冷启动外部计时 + 门槛对照（`--strict` 供目标平台验收） |
| `scripts/soak.sh` | soak 驱动：**进程外** RSS 采样 + 四项判据 + 报告 |
| `test/native/test_fullscreen_refresh.c` | 全屏往复刷新的纯 C 复现（让 ASan 能覆盖混合进程跑不到的路径） |
| `native/cmake/aarch64-linux-gnu.cmake` | 交叉编译工具链（可移植性回归） |
| `.github/workflows/ci.yml` | 平台矩阵（并显式标注各平台的能力边界） |
| `docs/benchmarks/2026-09-21_perf_soak_platform.md` | 基线：性能原始数据 + soak 曲线 + 平台矩阵 |

ABI 未实现符号 **8 → 8**（余下全是 Canvas，属 P1；t8 补齐的是 debug/fs/mem/perf 一族）。

### 13.2 本轮抓出并修复的 3 个缺陷

**① flush 行距误用 display stride —— heap-buffer-overflow（最严重的一个）**

LVGL 在 PARTIAL 模式下对**每个子块**调 `layer_reshape_draw_buf(layer, LV_STRIDE_AUTO)`，
即用**子区域宽度**重排缓冲。于是区域比屏窄时行是紧凑排列的，
缓冲能容纳的行数也变多：800 宽的屏、700 宽的区域，按屏宽算的 **48 行**缓冲
实际承载了 **54 行**。而 flush 路径一律把 display 的 stride 交给 sink，
读到第 48 行就越过缓冲末尾。

**这个缺陷改写了前面的一次「修复」**：t8 早前看到 `test_sdl2_backend` 的 8KB 缓冲
在 ASan 下报越界，就把测试缓冲放大到全屏尺寸去迁就 —— 那其实是在**掩盖缺陷**。
按正确行距，8KB（64×64 紧凑）本来就是对的。现已改回并用断言钉住行距语义。

定位过程值得记下：混合进程跑不了 ASan，于是把场景搬进纯 C 用例；
**首版复现「没崩」是假阴性** —— 紧循环 pump 下 600 轮只产生 10 次 flush，
刷新定时器根本没到期，压根没走到出问题的代码。加 2ms 帧节奏后立刻报出。
教训：**「复现不出来」必须先确认复现真的驱动到了目标代码**，
否则会得出「C 层干净」的错误结论。

**② NULL 解引用回归 —— 由 arm64 先跑到**

新增的行距计算无条件解引用 `area`，而 `test_display_indev` 以 `area = NULL`
直接调 trampoline。**这不是 arm64 的问题**，是 x86 侧加完代码后漏跑全量套件，
被 arm64 先跑到。修复后 arm64 8/8、x86 9/9。

**③ SDL2 后端 deinit 漏复位会话计数器**

`g_flush_count` / `g_present_count` / `g_wait_timeout_count` 是「本次 start 之后
发生了多少次」的语义，生命周期应与窗口绑定，却在 deinit 里没被复位。
性能基准先跑了一遍 SDL2（init → 采样 → close），随后的 `sdl2_test`
看到上一次会话的累计值而失败，并**级联**出第二处失败
（前者失败后提前返回、窗口未关闭，后者命中 `sdl2_init` 的幂等分支而不再校验格式）。

### 13.3 性能与 soak 的结论（详见基线文档）

- 9 项指标全部落在 §11.2 门槛内，但**两项需要重新校准**：
  · FFI 开销实测 1.74 – 1.96 μs，门槛 2 μs —— 余量仅 2 – 20%；
  · FPS 30 – 31 而门槛 ≥30 —— 这是 `LV_DEF_REFR_PERIOD = 33ms` 的**周期上限**，
    与渲染能力无关（CPU 忙碌比仅 10 – 20%）。要更高 FPS 必须下调刷新周期。
- soak（120s / 300s 缩放版）：**计数漂移全为 0**（alive/obj/closures/anim_ctx），
  无崩溃。**24h 未运行** —— RSS 是 GC 振荡量（300s 内升到 30 MB 又回落到 18.5 MB），
  短跑不足以判定收敛，`soak.sh` 会主动对 <1h 的运行打 `[NOTE]` 说明这一点。
- 平台矩阵：ubuntu-x64 **368 项**全绿；ubuntu-arm64（qemu）**332 项**全绿；
  aarch64 交叉编译零告警；macos-arm64 未验证。

### 13.4 四处「静默」的新增堵口

| 场景 | 原行为 | 现行为 |
| --- | --- | --- |
| flush 区域超出绘制缓冲 | 按错误行距越界读内存 | 拦住并报出 w/h/pitch/bpp/需求/缓冲的实际值 |
| `area == NULL`（仅直接调 trampoline 可达） | 解引用崩溃 | 退回 display stride 并跳过检查 |
| dumpTree 返回值 | 被当作错误码（「展开出 6 个节点」→ `UnknownCode(6)`） | 只判负值；**计数与错误码是两类返回值** |
| SDL2 第二次 init/close | 计数器残留 | deinit 复位，计数器与窗口同生命周期 |

---

## 14. 更正：内置 CJK 字库**不是**常用汉字表

### 14.1 被更正的三处表述

| 位置 | 原标题（错） | 更正后 |
| --- | --- | --- |
| `src/font.cj` 文件头 | 「含中文的界面**必须**显式使用 CJK 字体」 | 拆成两个坑：Montserrat 无中文（坑 1）+ **Cjk16 不是"中文可用"的开关**（坑 2） |
| `src/font.cj` 枚举注释 | 「SIMSUN 16 常用汉字（1000 字），界面含中文时用它」 | 官方称 radicals；实测是混合清单且有缺字，逐字列出 |
| `src/ffi_bridge.cj` | 「1 = SIMSUN 16 常用汉字」 | 同上，并指向探针 |
| `docs/P0_RESULTS.md` §10.2 A4 | 「自带 1000 常用汉字，无需外部工具链」 | 撤下「常用汉字」，指向本节 |

### 14.2 官方原文（四处一致）

```
lv_conf_template.h:510        #define LV_FONT_SIMSUN_16_CJK  0  /*1000 most common CJK radicals*/
lv_conf_internal.h:1579       （同上）
cmsis-pack/lv_conf_cmsis.h:506（同上）
docs/overview/font.rst:83     "16 px font with normal range plus 1000 of the most
                               common CJK radicals"
docs/others/ime_pinyin.rst:34 "currently only has more than 1,000 most common CJK radicals"
```

注意：**这句原文就写在本项目自己生成的 `native/include/lv_conf.h:510` 里**
（该文件由 `scripts/gen_lv_conf.sh` 从上游模板生成，注释原样保留）。
也就是说，正确措辞一直躺在仓库里，而我们的三处注释写成了「常用汉字」——
**这不是信息缺失，是没有去核对手边就有的原文。**

### 14.3 实测（`native/probe/probe_font_coverage.c`）

不靠措辞推断，直接用 `lv_font_get_glyph_dsc()`（`letter` 参数即 Unicode 码位）逐字询问：

| 组 | 结果 |
| --- | --- |
| 社区报告的缺字（简体） | **问 厅 灯 调 窗 帘 —— 6/6 全部缺失** |
| 对应的繁体/日文异体 | 問 調 窓 **在册**；廳 燈 簾 缺失（3/6 缺失）→ 「有繁无简」不是绝对规律 |
| 本项目自身用字 | **显 缺失**（1/13）；你好世 界中文我字大体小 均在册 |
| 部首样本 | **缺 19/28** —— 连「它就是部首表」这个字面读法也不成立 |
| CJK 统一表意文字总数 | **1118 个**（U+4E00..U+9FFF 共 20992 码位） |
| 平假名 + 片假名 | **173 / 192（几乎全覆盖）** |
| ASCII 可打印 | 95 / 95 |

**修正后的准确描述**：它是一份**手工拼合的混合清单** ——
简体常用字 + 繁体字 + 日文假名/异体混在一起。
官方 "1000 most common CJK radicals" 是**历史描述**，不能当选字依据。
（上游 `--symbols` 参数确实是逐字手写的一大串，含大量中文常用字与日文假名，
这也解释了为什么「部首」这个词与实际成分不符。）

### 14.4 为什么这个错误比「措辞不准」严重

「用 Cjk16 就能显示中文」会让使用者建立**错误的安全感**：
缺字时 LVGL 只在日志打一行 `glyph dsc. not found for U+XXXX`，
画面上直接少一块，**不报错、不抛异常、不影响其他字符**。
于是「输入 → 界面」这条链路上，一段文案里少一个字，可能到验收甚至上线后才发现。

正确的心智模型是：**不存在"这个字库支不支持中文"这种判断**，
只能对每一段真正要显示的文本逐个字符核对。

### 14.5 防复发

探针第 0 节是**自检**：先用 ASCII 'A'、'一' 验证调用约定正确，
并对照 Montserrat **不应**有中文（若它有，说明字体指针拿错了）——
自检失败时后续「缺失」结论一律作废（否则一次 API 误用会被误读成「字库几乎是空的」）。

第 7 节是**结论锁定**：断言「问/调/窗 仍缺失、問/調/窓 仍存在、我 仍存在、
CJK 字数仍在千余量级」。一旦 LVGL 换字库，这里立刻失败，
强制复核上述被更正的四处文字 —— 把「文档准确性」变成一条可执行的断言。

### 14.6 应对手段（已实施，见 §15）

当初这里写的是「缺一个能让使用者自查的 API……本轮只提出、未实施」。
现已实施为三层，详见下一节。

---

## 15. 字形覆盖的应对：库能力 + 开发期自查

§14 定性了「内置 CJK 字库有缺字」，但没有给出**使用者该怎么办**。
这一节补上。问题的难点不在"查"，而在下面这件事：

    ★ 用了哪些汉字，是**确定**的（就在源码里）；
      但**没有人能靠阅读把它列全** —— 汉字不是字母表，
      你无法凭记忆枚举自己刚敲过的那几十个字。

所以它必须由机器扫，不能由人保证。据此分三层。

### 15.1 三层手段

| 层 | 手段 | 回答什么 | 什么时候用 |
| --- | --- | --- | --- |
| 库能力 | `LvFont.hasGlyph(cp)` / `LvFont.missingGlyphs(text)` | 「这段文本里有几个字渲染不出来」 | 运行时，尤其是文案**是算出来的**时候 |
| 开发期 | `python3 scripts/cjk_audit.py` | 「我的代码里到底用了哪些汉字，哪些缺」 | 提交前 / CI |
| 根治 | 换覆盖完整的自定义字体（`LvFont.load`） | 从根上消除不确定性 | 正式产品 |

新增 ABI：`int32_t lvglcj_font_has_glyph(int64_t font, int32_t codepoint)`。
★ 它返回的是**三态而非错误码**：`1` 有字形 / `0` 没有字形（**合法答案**）/
负数才是错误码。因此**不能**写 `rc != LVGLCJ_OK` 判断成功 ——
0 同时是 `LVGLCJ_OK` 也是「没字形」。这个坑在契约头里显式写了。

契约上还守住一条：**缺字不是异常**。把「缺字」抛成异常会迫使调用方用
try/catch 处理一个正常情况，反而更容易写出「catch 里什么都不做」的代码。
只有查询本身失败（句柄无效 / 非 LVGL 线程 / 未初始化）才抛。

### 15.2 审计脚本：两个必须做对的分流

首版脚本犯了两个错，都值得记下来，因为它们是这类工具的通病：

**① 必须跳过注释。** 注释里的汉字不会渲染。本仓库注释里就有大量中文，
其中「显」恰好是缺字 —— 不跳过注释会把它算成"界面缺字"。

**② ★ 字面量还必须按用途分流 —— 这条首版漏了，报告直接失去意义。**
首版只跳过注释，于是把 **272 个汉字**全报成"界面用字"，其中一半"缺失"。
可那些字绝大部分来自 `println` / 异常信息 / 日志 ——
**它们输出到控制台，根本不经过字形渲染**。
一份喊狼来了的报告等于没有报告。

修正后按**调用点**分流，报告变成：

```
=== ★ 界面文案用字（进渲染路径，必须全覆盖）：4 个汉字 ===
  了我次点
=== 其它字面量用字（控制台/异常信息，不渲染）: 272 个汉字 ===
  ...（只列界面未用到的）
  ↑ 这些字只出现在 println / 异常信息里，缺字形不影响显示。

=== ★ 字形覆盖核对（LV_FONT_SIMSUN_16_CJK）===
  码位总数 4，缺失 0，覆盖 100.0%
  ✓ 界面文案全部可渲染。
```

分流靠 `UI_CALLS` 白名单（`setText` / `withLabel`），**它是启发式的**，
所以脚本里写明了维护规则：新增任何「接受文案并显示出来」的 API 时必须加进去。
实测就踩到一次 —— `btn.withLabel("点我")` 是控件工厂，也是渲染入口，
漏掉它会让「我」被误归到控制台组。

**残余缺口（词法扫描的边界，已写明在脚本里）**：文案若先赋给变量再传进去
（`let s = "点我"; label.setText(s)`），脚本看不到数据流，不会硬判定它。
这类文案要在运行时用 `missingGlyphs` 核对 —— 两者互补：
**脚本管"直接写出来的文案"，API 管"算出来的文案"。**

### 15.3 ★ 本轮抓出的缺陷：内置字体登记表的「容量 8 + 静默溢出」

新增的 `testBuiltinFontCannotBeReleased` 单独跑**通过**，
放进 9 个用例的序列里就**挂死** —— 典型的「只在长序列里复现」。

根因在 `font.c`：内置字体是用一个**大小 8 的句柄数组**记录的，
**满了就静默不再记录**（原注释写的是「超出时只记录不报错」）。

- 句柄 id 单调递增且**永不复用**（ADR-001），而 `lvglcj_deinit()/init()`
  可以反复发生 → 同一对静态字体每次初始化都拿到**新句柄**；
- 于是数组很快被**上一轮生命周期留下的陈旧句柄**占满；
- 此后新登记的内置字体句柄不再被识别 →
  `lvglcj_font_delete()` 放行到 `lv_binfont_destroy()` 去释放
  **静态 const 对象** → 未定义行为（表现为挂死）。

那个「8」没有任何依据，而溢出路径是**静默**的 —— 正是本项目一直在清理的模式。

**修正**：这个集合本来是**编译期已知**的（就那么几个 `&lv_font_xxx`），
用运行时簿记去描述它，等于凭空引入一个会满、会过期、会静默失效的状态。
改为**按指针判定**（`lvglcj_font_is_builtin_ptr`），两类失效从根上不存在，
溢出路径也一并消失。

### 15.4 验证

| 项 | 结果 |
| --- | --- |
| `src/font_test.cj` | **9 例**（含实测缺字名单锁定 + 缺字不抛异常 + 去重顺序 + 示例文案全覆盖） |
| 门禁 | **7/7** |
| 仓颉测试 | **60 通过 / 1 跳过 / 0 失败**（t7 为 51，本次 +9） |
| C 单测 / ASan | 9 个可执行全绿 / 零报告 |
| 自仓库审计 | 界面文案 4 字（了我次点）**100% 覆盖**；控制台 272 字不参与判定 |

---

## 16. t8 后续：ASan 口径决策 + 随机序列测试抓出的级联清理缺陷

### 16.1 §11.4 与 §9.5 互斥 → 决策为「方案 a」

§11.4 要求 24h 运行「零崩溃、**零 ASan 报告**、内存收敛」，
而 §9.5 已实测确立 **ASan 与仓颉运行时硬不兼容**（混合二进制崩在退出路径
`CJ_ScheduleStop` 的线程簿记，`detect_leaks=0` 也避不开）。
二者互斥，不存在同时满足的实现路径。**决策为方案 a**：

| 承载者 | 时长 | ASan | 覆盖 |
| --- | --- | --- | --- |
| `scripts/soak.sh` | 24h | ❌ | 运行时行为 + RSS/四计数收敛，**含仓颉侧闭包表** |
| `test/native/test_soak_asan.c` | 门禁 8s / 可 `--seconds` | ✅ | **C 层**：句柄表、样式注册表、anim_ctx、display |

覆盖边界（写进了两个文件的头部，不许含糊成「ASan 也测了」）：
纯 C harness **驱动不了回调链路** —— `closure_id` 由仓颉侧分配，
纯 C 进程里没有任何闭包。故它覆盖不到「回调里分配/释放的资源」。
**两者互补，缺一不可。**

`soak.sh` 因此会**拒绝**被 ASan 插桩的二进制（那不是"更严格"，
而是会崩在半路、白白浪费 24 小时），并支持**中断出部分报告**（长跑必需）。

### 16.2 ★ 级联删除的子对象清理被整段跳过（随机序列抓出）

**发现方式**：`src/fuzz_test.cj` 的 `testCascadeDeleteUnregistersChildClosures`
—— 20 轮 × 5 个带回调的子对象，断言闭包归零。实测泄漏 **100/100**（一个不多一个不少）。

**根因**（读 LVGL 源码确认）：`obj_delete_core()` 的顺序是
**先给父对象发 `LV_EVENT_DELETE`，之后才递归删子对象**。
而我们的钩子在父对象上先调 `lvglcj_invalidate_subtree()`，
它把**子对象的 ptr→handle 反查项摘掉**；等子对象自己的 DELETE 钩子稍后触发时，
`lvglcj_handle_of(child)` 返回 NULL，钩子**在第一步就提前返回**，(a)(a2)(c) 三步全被跳过。

**三个后果**（同一个 bug 带出，都不是小事）：

| 步骤 | 后果 |
| --- | --- |
| (a) 注销闭包 | 子对象的事件闭包**全部泄漏** → 闭包表随「建容器→删容器」单调增长 |
| (a2) 解绑样式 | 只被"子对象"引用的样式永远显示"有人在用" → **释放守卫变死锁** |
| (c) 停动画 | 深度 ≥ 2 的后代动画**不会被停** → ADR-015 的补偿失效 |

**修正**：清理改由**父一次做完整棵子树**（`cleanup_subtree`，先深后浅，
每个节点各自做 (a)(a2)(c)(d)）。子对象自己的钩子此后会因句柄已失效而自然跳过 ——
这也让钩子入口的 `handle_of == NULL` 提前返回不再需要。

**★ 为什么已有的确定性 soak 没抓到**：它的树里唯一挂了回调的对象（`btn`）
在删父之前就被**显式删掉**了；而它新建的 `btn2` 没有回调。
也就是说**负载形状恰好绕过了这条路径**。这正是随机序列的价值：
它会撞上人没想过的形状 —— 确定性负载永远只能覆盖"设计者想到的形状"。

### 16.3 一个尚未定论的观测（已记录，未修复）

`handleCount(Alive)` 在随机序列下有一个**有界偏移 +4~+5**：

- **不随步数增长**：100 步与 300 步的观测值完全相同（alive=7）；
- `objCount` 全程不变（对象本就在，只是句柄计数不同）；
- `closureCount` 与 `animCtxCount` 都**严格归零**；
- 已排除项（都做过，都是否定的）：按操作类别逐个隔离 → **每一类都有 +4~+5**，
  连「纯建对象 → 删 → 推帧」（无样式/无动画/无回调）那一类也一样，
  故与样式、动画、闭包**无关**；加预热（20 帧 / 建一个对象再删 / 推帧）
  **不改变基线**（始终 2），故**不是**惰性登记。

有界且不随负载增长 ⇒ **不违反 §11.3「句柄表大小稳态后不再增长」**，
但它的确切来源尚未定论。因此 `fuzz_test.cj` 的判据按 §11.3 的**原话**编码：

1. 偏移不超过小上界；2. ★ **100 步与 300 步之间不再增长**；3. 跑完 3 个 seed 后不递增。

第 2、3 条比"等于基线"更贴近意图 —— 直接测「稳态后还在不在长」，
而任何按操作次数累积的真泄漏都必然越过它。

### 16.4 随机序列的实测规模

| 承载者 | 时长 | 规模 | ASan | 结果 |
| --- | --- | --- | --- | --- |
| `test_soak_asan --fuzz`（纯 C） | 8s × 3 seed | 各约 **100 万步** | ✅ | 0 报告 0 失败 |
| `fuzz_test.cj`（仓颉侧，含闭包） | 门禁内 | 3 seed × 300 步 + 定向用例 | — | 全绿 |

§11.4 的「10⁶ 次事件」在 **8 秒**内即达成（2 小时约 **9 亿次**），
seed 可复现（`--seed N`），失败能精确重放。

### 16.5 顺带补上的一处 L1 缺口

`LV_STATE_*` 状态位此前**只定义了 `DEFAULT`**，意味着调用方拿不到
除默认态以外的任何 selector —— **连「按下时变色」都表达不出来**
（selector 是 `part | state` 的位或）。已补齐全部 13 个状态位
（取值与 `lv_obj.h` 逐项一致）。
这个缺口是被随机序列"要用两个不同 selector"逼出来的 —— 用不到就不会发现。

---

## 17. 注释与事实的一致性清理 + 绑定生成器

### 17.1 ④ 清理过期描述（实际 7 处，比估计的 6 处多）

`§14` 更正了 4 处「Cjk16 = 常用汉字」的误述。本轮做了一次**全库扫描**
（而不是只修已知那几处），又发现 5 类共 7 处，全部修正：

| 位置 | 过期内容 | 为什么危险 |
| --- | --- | --- |
| `native/src/style.c` 注释 | 「字体子系统（font.c）**尚未实现**」 | font.c 早已实现（内置字体 + 加载 + 字形查询），此处只剩句柄校验 |
| `native/src/style.c` 报错信息 | 「字体句柄无效（**font.c 尚未实现**，P0 请用默认字体）」 | 把排查方向指向一个**不存在的原因**；真实原因是句柄已 close 或传错类型 |
| `src/style.cj` 文档注释 | 「含中文的界面**必须**传 Cjk16」 | 与 §14 更正的四处是同一件事（第 5 处）；Cjk16 本身有缺字 |
| `src/p0_assert_test.cj` | 「⑧ → t7（**动画子系统尚未实现**）」 | t7 已完成并落地；典型「计划时写的，完成时忘了回头改」 |
| `src/display.cj` / `src/event.cj` 注释 | 「需要自绘请用 **Canvas** 控件」 | Canvas 排在 **P2**，尚未落地 —— 这是在**推荐一个不存在的 API** |
| `native/src/display.c` / `native/src/event.c` 报错信息 | 同上 | 同上；已加「P2 落地」标注 |

**结论**：过期注释**比没有注释更糟** —— 它会把排查方向指向一个不存在的原因。
复查已确认全库无残留（`grep` 三条模式全部为空）。最后两类尤其值得记：
**「建议使用尚未实现的能力」也是过期描述的一种**，而且更隐蔽 ——
它不是陈述错误，而是让使用者在错误的方向上找答案。

### 17.2 ③ 绑定生成器（`scripts/gen_bindings.py`）

#### 先量化问题

一个样式属性要在**四处**保持一致：桥接头声明 / C 实现 / `ffi_bridge.cj` 的
`foreign` 声明 / `style.cj` 的 L1 包装。没有任何编译期机制保证同步。
实测结果：

```
ABI 提供：53 个 style setter
L1 已包装：26 个
```

**缺的 27 个里有** `padLeft/padRight/padTop/padBottom`（只有 `padAll`）、
`shadowOffsetX`（而 `shadowOffsetY` 有）、`lineWidth`、`borderOpa`、`maxWidth/maxHeight`、
`textAlign`、`arc*`、`image*`、`blendMode`、`layout`、`baseDir`、`clipCorner`、`rotarySensitivity`。

也就是说：「**某个属性能不能用，取决于当初谁顺手写了哪个**，而不是取决于设计」。

#### 生成器

输入选 **C 侧 ABI**（唯一真源），而不是 LVGL 头文件或另维护一张表：

- LVGL v9 的 `lv_style_set_*` 是宏生成的，直接解析头部收益低；
- 而 C 侧 ABI 已经是「按子系统系统化整理过」的那一层，且它本身就是 L1 的唯一依赖
  —— 从它生成，**L1 与 ABI 的一致性自动成立**。

`--check` 模式把一致性变成 CI 判据（已接入门禁第 7 步）：

- 输出「ABI 有而 L1 没有」的单向漂移清单，并核对已有包装的**写法**是否与生成规则一致
  —— 校验时它逐字符匹配了 25 个手写包装，**0 处偏差**，规则得到实证；
- `--emit-missing` 产出增量补齐代码，不触碰已有手写代码。

#### 两个"生成器必须显式处理"的点（都是实测踩出来的）

1. **特例白名单**：生成器一度产出 `textFont(v: Int64)`，而手写版本是
   `textFont(f: LvFont)`。两者构成**同名重载并且能编译通过** ——
   但把更弱的那份（裸 Int64 句柄）也暴露给了使用者。
   这类"能编译的错误"正是生成器最该防的，因此特例用白名单写死在脚本里
   （`SPECIAL_CASE`），并在 `--check` 里单独回报「手写版本在不在」。
2. **解析正则不能假设实参是裸名字**：手写特例里是 `(handle, f.handle)`，
   首版正则只写 `\w+`，于是把它误判成「手写版本也不在 —— 真的漏了」。
   误报比漏报更容易被当成真问题，所以已放宽为 `[\w.]+` 并注明原因。

#### 结果

| 项 | 前 | 后 |
| --- | --- | --- |
| L1 样式包装 | 26 | **53**（+27，全部由生成器产出） |
| 一致性检查 | 无 | `gen_bindings.py --check`，**门禁第 7 步** |
| 门禁 | 7 项 | **8 项** |

★ 补进来的 27 个都是对**已被 `test_style_widgets` 覆盖的 ABI 函数**的薄转发，
因此风险集中在"能否编译"而非"语义是否正确"；`cjpm build` + 门禁全绿已覆盖。
但**它们还没有被独立断言过取值往返** —— 这是后续（P1 样式补到 40+ 属性时）
应该补的：生成器能保证"存在且签名正确"，保证不了"设进去真的生效"。

---

## 18. ④ 让 CI 真的能跑：两个"首跑必红"的原因

把远程仓库接上之后，第一件事不是 push 而是**用干净克隆模拟 CI 会看到什么**。
结果发现两处必红（都不是 YAML 语法问题，语法一直是合法的）：

| # | 原因 | 为什么 CI 会红 |
| --- | --- | --- |
| 1 | `third_party/lvgl/` 被 `.gitignore` 排除（约 200 MB） | 克隆后它**不存在**，而 `native/CMakeLists.txt` 在配置阶段直接 `FATAL_ERROR`。三个 build 作业里**都没有拉取步骤** |
| 2 | `src/generated/conf_const.cj` 同样是生成物且不入库 | 它只在 `build_native.sh --probes` 时才生成；不带该参数时脚本只打印「未生成，跳过」然后**继续**，随后 `cjpm build` 因缺包文件失败 |

第 2 条是典型的**静默跳过**（本项目一直在清理的模式）：脚本给出了提示但不失败，
调用方很容易以为"构建成功"。修法是把要求显式化 —— CI 里写 `--probes`。

**验证方式（本地可做的最强验证）**：`git clone .` 到一个临时目录 —— 那是 CI 的精确视图 ——
然后逐步执行工作流里的命令：

```
① 干净克隆（无 LVGL）→ cmake 配置：CMake Error「未找到 LVGL 源码」   ← 复现了必红原因
② 补上 LVGL 之后      → 配置 0 / 构建 0 / ctest 10 个用例全过（27s）  ← 证明步骤序列正确
```

`fetch_lvgl.sh` 默认从 **gitee 镜像**拉 v9.2.2（可用 `LVGLCJ_LVGL_REPO` 覆盖），
实测该镜像可达。

### 18.1 SDK 凭据为什么要拆成 Variables + Secrets

下载地址带签名参数（如 `objectKey`），属**临时凭据**，而 `vars` 会明文出现在日志里
—— 所以地址必须放 `secrets`（Actions 会自动打码）。
但 GitHub **不允许 `secrets` 出现在 job 级 `if:` 条件里**（那是配置期判断），
因此"是否启用"由 `vars` 里的布尔开关决定：

| 类型 | 名称 | 用途 |
| --- | --- | --- |
| Variables | `ENABLE_CANGJIE_JOB = true` | 开关（可公开，仅表示启不启用） |
| Secrets | `CANGJIE_SDK_URL = <地址>` | 地址本体（打码） |

这是一个**必须拆开的组合**，不是冗余设计。地址过期时只需更新 Secret —— 不必改代码。
SDK 约 385 MB，已加缓存（key 刻意不含 URL：那既泄露又必然失效）。

### 18.2 本环境无法验证的部分（如实记录）

| 项 | 状态 |
| --- | --- |
| `github.com` 可达性 | **不可达**（HTTP 000），因此 `git push` 无法在本环境完成 —— 两个提交已在本地就绪 |
| GitHub Actions 实际执行 | **未验证**（无网络） |
| `ubuntu-24.04-arm` runner | 未验证。该标签需**公开仓库**（私有仓库无免费 arm64 runner）；若仓库为私有需改用自托管或删掉该矩阵项 |
| macOS runner 上的 SDL2 / ctest | 未验证 |
| 仓颉侧作业（SDK 下载 + cjpm 全流程） | 未验证（依赖 385 MB 下载） |
