# lvgl4cj

**LVGL 9.2 的仓颉（Cangjie）安全绑定层。**

C 侧手工管理内存与线程归属，仓颉侧提供**不泄漏、不静默失败**的接口。
它不是「把 C API 逐个翻译过来」——每层接口只在**能同时满足内存安全与明确报错**时才放出来；
做不到的部分宁可缺失，也不提供一个"看起来能用"的版本。

---

## 目录

- [设计立场](#设计立场)
- [快速开始](#快速开始)
- [当前能力](#当前能力)
- [平台支持](#平台支持)
- [质量门禁](#质量门禁)
- [★ 已知边界（请先读这一节）](#-已知边界请先读这一节)
- [目录结构](#目录结构)
- [文档索引](#文档索引)

---

## 设计立场

三条贯穿全部代码的原则，读代码时可以先记住它们：

1. **内存安全由 C 侧兜底，不交给仓颉 GC。** 像素缓冲、句柄表、等待者等一律由 C 侧
   手工管理；**绝不把裸指针交给仓颉**（ADR-002：GC 会移动缓冲）。
2. **线程归属在 C 侧强制检查。** `anim/display/event/font/indev/obj/style/timer/widgets`
   共 9 个模块会校验调用线程，错线程返回 `WrongThread`，而不是崩在 LVGL 内部。
3. **失败必须显式，宁可报错也不静默回退。** 典型例子：未开启 CJK 字体时
   `LvFont.builtin` **明确抛错**，而不是悄悄回退到一个没有中文字形的默认字体。

---

## 快速开始

```bash
# 1. 拉取 LVGL（锁定版本 v9.2.2）并构建 C 库
bash scripts/fetch_lvgl.sh
bash scripts/build_native.sh          # 末尾会打印 ABI 符号自检结果

# 2. 跑示例
cd examples/hello_cj && cjpm run

# 3. 跑全部检查（推荐先跑这个，确认环境没问题）
bash scripts/gate.sh
```

最小可用代码：

```cangjie
import lvgl4cj.*

main() {
    let rt = LvglRuntime()
    let disp = rt.startHeadless(320, 240)
    let lbl = LvLabel.create(LvObject.screenActive())
    lbl.setText("你好")
    lbl.on(LvEventCode.Clicked) { _ => println("clicked") }
    rt.pumpFrames(10)
    lbl.close()          // 不需要手动释放任何 C 侧资源
    rt.deinitLvgl()
}
```

> `LvglRuntime` 提供四种启动方式：`startHeadless`（无显示后端，测试/嵌入式）、
> `startPumped`（调用方驱动 `pump()`，嵌入式主循环形态）、`startThreaded`（LVGL 独占线程）、
> `runForever`（托管主循环）。

---

## 当前能力

**以下数字为 2026-09-22 实测**（不是设计目标）：

| 项 | 数量 |
| --- | --- |
| L1 模块（`src/*.cj`，不含测试） | **19** |
| C 侧实现（`native/src/*.c`） | **22** |
| ABI 声明函数 | **254**（已实现 **254**，未实现 **0** —— 自检为「声明与产物完全一致」） |
| 仓颉侧 `foreign` 声明 | **271** |
| C 用例 | **10 个可执行 / 520 项检查** |
| 仓颉测试 | **80 用例（79 通过 / 1 跳过 / 0 失败）** |

已具备的能力（摘要）：

- **运行时**：四种启动方式、`pumpFrames` / `timerNextMs`（供外部调度器算休眠）、
  跨线程投递 `post` / `postAndWait` / `tryPost`（`try*` 不阻塞，供主循环内调用避免自锁）
- **句柄与生命周期**：四态句柄（`UNINIT`/`ALIVE`/`INVALIDATED`/`RELEASED`）、
  id 单调递增**永不复用**、删除栈可报「谁删了我」、`INVALIDATED` 有界回收、
  父对象删除时子句柄**级联失效**（不是静默变野指针）
- **UI**：Display（headless / SDL2）、InDev（指针/按键/编码器）、
  对象与样式（27 个 setter）、21 种事件码、字体、动画、POSIX 文件系统
- **控件**：`LvObject`（基类）、`LvLabel`、`LvButton`，P1 批次 1 的
  `LvSwitch` / `LvCheckbox` / `LvBar`，P1 批次 2 的 `LvSlider` / `LvArc` /
  `LvLed` / `LvSpinner`，P1 批次 3 的 `LvDropdown`，P1 批次 4 的 `LvLine`，
  P1 批次 5 的 `LvImage`，P1 批次 6 的 `LvRoller`，P1 批次 7 的 `LvTextarea`，
  以及 `LvCanvas`（自定义绘制的唯一出口，§3.11.2）。
  **共 16 个**，P1 清单其余 3 个见「已知边界」
- **可观测性**：`LvDebug`（`objCount` / `memMonitor` / `perfSample` / `dumpTree`）、`LvBench`（性能测量）

---

## 平台支持

| 平台 | 运行测试 | 覆盖 | 状态 |
| --- | --- | --- | --- |
| **ubuntu-x64（WSL2）** | ✅ 10 个可执行 / **381 项** | 全量：C 层 + ASan + 仓颉层 + 门禁 | **已验证** |
| **真实 arm64 硬件**：Raspberry Pi Zero 2 W | ✅ 9 个可执行 / **345 项** | C 层（SDL2 后端未构建） | **已验证（真机）** |
| ubuntu-arm64（qemu） | ✅ 8 个可执行 / 332 项 | C 层（SDL2 关闭） | 已验证（模拟）·已被真机取代 |
| 交叉编译（aarch64） | ⛔ 未运行 | 编译期可移植性 | ✅ 零告警 |
| **macos-arm64** | ❌ **失败** | — | **未验证** |

真机复现：`bash scripts/run_on_target.sh pi`（默认 SSH 别名 `pi`）

> 真机那一行是本项目**唯一在目标形态硬件上**取得的证据，「arm64 已验证」不再需要
> "仅 qemu"的限定 —— **但只限 C 层**：宿主没有 arm64 的 SDL2 开发包，
> 所以 `test_sdl2_backend` 未构建、刷新路径走的是跳过分支。

---

## 质量门禁

`bash scripts/gate.sh` 共 **8 项**，全部为硬失败：

| 项 | 内容 |
| --- | --- |
| 1 | `cjfmt` **零 diff**（含生成文件） |
| 2 | `cjlint` **零告警** |
| 3–4 | `cjpm build` / `cjpm test` |
| 5 | `ctest` 全量 C 用例 |
| 6 | **ASan 零报告**，且工具**自证有效**（人为注入的泄漏必须被抓到） |
| 7 | **ABI ↔ L1 绑定一致性**（每个 style setter 都要有一致的升层包装） |
| 8 | 构建期 conf hash 双向校验 |

另有：

- **CI 平台矩阵**（`.github/workflows/ci.yml`）：ubuntu-x64 全量、ubuntu-arm64、
  macos-arm64、aarch64 交叉编译检查、C 层长跑（soak 作业）
- **性能 / soak 工具**：`scripts/perf.sh`（`--strict` 可用于目标平台验收）、`scripts/soak.sh`
- **真机部署**：`scripts/run_on_target.sh`

---

## ★ 已知边界（请先读这一节）

这一节比上面任何数字都重要。**未列出的能力不代表可用；列出的缺口都是已知的。**

1. **Canvas 已实现，但有两处已知限制**（细节见 `src/canvas.cj` 文件头）：
   - **线宽不可配**：`drawLine` / `drawArc` 的 ABI 里没有 `width` 参数，实现固定为 1 与 2；
   - **没有 `drawPolygon`**：设计文档 §3.11.2 的草稿里有它，但**冻结版桥接头只声明了 8 个
     canvas 函数**，不含 polygon —— 以头文件为契约记录，故不实现（需要时应走契约变更）。
   另注意颜色参数的**解释方式不一致**：绘制类与 `fillBg` 是 `0xRRGGBB`（高 8 位不解释，
   透明度走 `opa`），而 `setPalette` 是 `0xAARRGGBB`（调色板项自带 alpha）。
2. **控件 16 个**：`LvObject`（基类）、`LvLabel`、`LvButton`、`LvSwitch`、
   `LvCheckbox`、`LvBar`、`LvSlider`、`LvArc`、`LvLed`、`LvSpinner`、
   `LvDropdown`、`LvLine`、`LvImage`、`LvRoller`、`LvTextarea`、`LvCanvas`。
   设计文档 §7.1 的 P1 清单还剩 **3 个**未做，全是复合控件：
   `chart` `table` `keyboard`。
   它们之所以被放在后续批次，不是因为难，而是各自引入**新的值类型**
   （图像描述符、点数组、字符串列表），各带一套所有权问题 —— 需要逐个单独设计。
   在此之前需用原生 API 补齐。
3. **内置 CJK 字库不是"中文可用"的开关。** 官方描述为
   `1000 most common CJK radicals`，但实测（`native/probe/probe_font_coverage.c`）
   是一份**手工拼合的混合清单**：约 1118 个 CJK 字 + 173 个日文假名，
   **有缺字**（问/厅/灯/调/窗/帘/显 缺失，而 問/調/窓 在册）。
   缺字时 LVGL **只打一行日志，画面上直接少一块，不报错、不抛异常**。
   因此「这段文案能不能显示」只能逐个字符核对 —— 不存在一个开关式的答案。
   可用 `LvFont.builtin(...).missingGlyphs("文本")` 自查。
4. **性能数字全部来自 WSL2，不是目标平台。** §11.2 明确要求量化指标在目标平台判定，
   所以当前结论是「实现具备达标能力、无数量级偏差」，**不是**「性能验收通过」。
   其中两项门槛还需重新校准：FFI 实测 1.74–1.96 μs vs 门槛 2 μs（余量仅 2–20%）；
   FPS 30–31 而门槛 ≥30 —— 那是刷新周期 `LV_DEF_REFR_PERIOD = 33ms` 的**周期上限**，
   与渲染能力无关（CPU 忙碌比仅 10–20%）。要更高 FPS 必须下调刷新周期。
5. **24h soak 未运行。** 120s/300s 缩放版下计数漂移全为 0（alive/obj/closures/anim_ctx）、
   无崩溃；但 RSS 是 GC 振荡量，短跑不足以判定收敛。
6. **macOS 未验证，且 CI 上当前是失败的**（`test_queue`）。已定位为测试侧问题
   （用 `usleep` 当同步原语 + 未建立"队列为空"的前提），修复进行中。
7. **真机未验证 SDL2 后端**（见上），因此目标平台的 FPS / 刷新时间仍未测。
8. **真机长跑未做**：单轮 345 项通过是实质证据，但不等于长跑结论。
9. **模糊测试未做**：当前 soak 是确定性负载形态（§11.4 未完成）。

---

## 目录结构

```
src/            L1 仓颉接口（19 个模块 + ffi_bridge.cj + 9 个测试文件）
                generated/ 由 scripts/gen_conf_const.py 生成，勿手改
native/         C 实现（22 个 .c）
  include/      lvglcj_bridge.h（对外契约，206 个声明，逐条注释）
                lvglcj_internal.h（内部）
  cmake/        含 aarch64-linux-gnu.cmake 交叉工具链
backend/sdl2/   SDL2 后端
test/native/    C 用例（10 个）+ target_suite.sh（目标机侧执行脚本）
native/probe/   诊断探针（6 个，用于把争议变成实测）
examples/       hello_cj（示例）、bench_cj（性能采集工具）
scripts/        构建、门禁、性能、soak、真机部署、配置生成等 12 个脚本
docs/           结果与基线记录（见下）
third_party/    LVGL 源码（由 fetch_lvgl.sh 拉取，不入库）
```

---

## 文档索引

| 文档 | 内容 |
| --- | --- |
| `lvgl4cj_设计方案_v0.4_冻结版.md` | **设计依据**：接口契约、不变式、验收门槛（§11.2/§11.3） |
| `docs/P0_RESULTS.md` | P0 实施记录：每个断言的实测结论、定位过程、修复 |
| `docs/benchmarks/2026-09-20_P0_gate.md` | 门禁与内存基线 |
| `docs/benchmarks/2026-09-21_perf_soak_platform.md` | 性能 / soak / 平台矩阵（含真机实测） |
| `docs/C4_DEFECT.md` | 一处已知缺陷的记录 |

---

## 开发约定

- **改了 C 侧就要重建库**：`bash scripts/build_native.sh`（会更新 `libs/` 并做 ABI 自检）
- **提交前跑门禁**：`bash scripts/gate.sh`（8 项全绿才算过）
- **格式化**：`cjfmt -d src`（就地）；生成文件也必须在格式化后零 diff
- **新增 ABI 符号**：契约头的注释要写清**语义与失败方式**，并同步 L1 包装 + 测试
- **不要写"看起来对"的注释**：本项目已出现过多次「注释与实测不符」的情况
  （如内置字库被描述成"常用汉字表"），正确措辞往往就在仓库里，先核对再落笔
