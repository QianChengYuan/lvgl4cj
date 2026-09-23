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

**以下数字为 2026-09-23 实测**（不是设计目标）：

| 项 | 数量 |
| --- | --- |
| L1 模块（`src/*.cj`，不含测试） | **19** |
| C 侧实现（`native/src/*.c`） | **22** |
| ABI 声明函数 | **293**（按 `scripts/build_native.sh` 构建末尾的自检口径；该自检的结论是「声明与产物**完全一致**」） |
| 仓颉侧 `foreign` 声明 | **305** |
| C 用例 | **10 个可执行 / 701 项检查**（由 `scripts/gate.sh` 直接产出，见平台支持一节） |
| 仓颉测试 | **86 用例（85 通过 / 1 跳过 / 0 失败）** |

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
  P1 批次 8 的 `LvTable`，P1 批次 9 的 `LvKeyboard`，P1 批次 10 的 `LvChart`，
  以及 `LvCanvas`（自定义绘制的唯一出口，§3.11.2）。
  **共 19 个** —— 设计文档 §7.1 的 P1 清单**已全部完成**
- **可观测性**：`LvDebug`（`objCount` / `memMonitor` / `perfSample` / `dumpTree`）、`LvBench`（性能测量）

---

## 平台支持

| 平台 | 运行测试 | 覆盖 | 状态 |
| --- | --- | --- | --- |
| **ubuntu-x64（WSL2）** | ✅ 10 个可执行 / **701 项检查** | 全量：C 层 + ASan + 仓颉层 + 门禁 | **已验证** |
| **真实 arm64 硬件**：Raspberry Pi Zero 2 W | ✅ 9 个可执行 / **345 项检查** | C 层 + **24h 长跑** + KMSDRM 上屏（aarch64 侧 `test_sdl2_backend` 未构建） | **已验证（真机）** |
| ubuntu-arm64（qemu） | ✅ 8 个可执行 / 332 项检查 | C 层（SDL2 关闭） | 已验证（模拟）·已被真机取代 |
| 交叉编译（aarch64） | ⛔ 未运行 | 编译期可移植性 | ✅ 零告警 |
| **macos-arm64** | ❌ **失败** | — | **未验证** |

★ 各行「N 项检查」是**各自那一次全量运行的快照**，不是同一时刻测的，会随用例增加而变化 ——
早先这三个数字（381 / 345 / 332）就因此互相对不上。宿主机的当前准确值由门禁直接给出：
`scripts/gate.sh` 会打印 `C 单测全绿（10 个可执行，合计 N 项检查）`。数字现在**可复现**，
不再依赖人手工求和（那次求和还漏掉了 `test_handle_table` 的 `PASS：` 前缀）。

真机复现：`bash scripts/run_on_target.sh pi`（默认 SSH 别名 `pi`）
真机长跑：在目标机上 `bash target_soak.sh 86400` —— 结果见
[`docs/benchmarks/2026-09-23_soak_target_24h.md`](docs/benchmarks/2026-09-23_soak_target_24h.md)
（24 小时 / 1.57 亿步 / 0 失败 / RSS 平台化，产物随文档入库）

> 真机那一行曾是**唯一在目标形态硬件上**取得的证据，「arm64 已验证」不再需要
> "仅 qemu"的限定 —— **但只限 C 层**：宿主没有 arm64 的 SDL2 开发包，
> 所以 aarch64 侧的 `test_sdl2_backend`（C 用例）仍未构建、刷新路径走的是跳过分支。
>
> ★ 这**不再**意味着"目标平台上没验证过 SDL2 后端"：2026-09-23 已在同一台设备上
> 用 **KMSDRM 直出**（不经过 X11/Wayland）跑通 `examples/hello_cj` ——
> 1920×1080、渲染 72 帧、截图取回宿主机分析（无黑带/无重影）、
> 且**实际生效的驱动名实测为 `KMSDRM`**（而不是悄悄回落到别的驱动）。
> 目标平台的性能基线见 `docs/benchmarks/2026-09-22_perf_target_pizero2w.txt`。
> 复现：`LVGLCJ_TARGET_SUDO_PW=<密码> bash scripts/run_on_target_kmsdrm.sh pi`
> （该脚本先让出 DRM master 再跑，结束时自动还原桌面；前提是接了一块屏）。

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
2. **控件 19 个**：`LvObject`（基类）、`LvLabel`、`LvButton`、`LvSwitch`、
   `LvCheckbox`、`LvBar`、`LvSlider`、`LvArc`、`LvLed`、`LvSpinner`、
   `LvDropdown`、`LvLine`、`LvImage`、`LvRoller`、`LvTextarea`、`LvTable`、
   `LvKeyboard`、`LvChart`、`LvCanvas`。
   设计文档 §7.1 的 **P1 清单已全部完成**（10 个批次、19 个控件）。

   ★ **中文字形：构建已启用 `LV_USE_TINY_TTF`，中文不再受内置字体限制。**
   背景：LVGL 内置的 CJK 字体（`lv_font_simsun_16_cjk`）注释写明只覆盖 1000 个常用字，
   超出的字会被**静默丢弃**（一个字一行 WARN），界面上表现为"文字整片消失"——
   本项目实测一个示例缺 33 个字。现在两条路都可以走：
     · `LvFont.loadTtf(path, size)` —— 直接读 TTF，任意中文可用（开发期方便，
       代价是运行期解析字体）；
     · `LvFont.load(path)` —— 读 LVGL 转换后的 `.bin` 字体（产品形态，产物小，
       但要用转换工具且通常需要子集化）。
   两者路径都走 LVGL 的文件系统（盘符 `A`，根目录见 `LV_FS_POSIX_PATH`，默认为进程工作目录），
   所以**系统字体的绝对路径不能直接用**，要表示成相对 FS 根的路径。

   **全控件示例**：`examples/gallery_cj` —— 把 19 个控件一次摆在窗口里（11 张卡片），
   屏幕顶部有一行每秒刷新的统计（FPS / CPU / 刷新耗时 / 绘制耗时 / 对象数 / 动画数 / 任务队列），
   退出时把末次采样打到 stdout。另有两个开关：
   `--static` 关掉全部动画（用于对照「静止时是否也抖」）、`--smoke` 有界运行后干净退出。
   运行：`cd examples/gallery_cj && cjpm run`（无显示环境可加 `SDL_VIDEODRIVER=dummy --smoke`，
   但那样不会真正呈现，FPS 只在有显示的机器上才有意义）。
   ★ 注意示例的初始化顺序**不是风格问题**：所有建 UI 的代码必须在 `startThreaded()` 之前，
   之后要用 LVGL 只能走 `rt.post { }`（主线程已不是 LVGL 线程，直接调 `lv_*` 会被断言拒绝）。

   ★ `LvChart` 的 series 用**索引**标识而不是指针 —— 这是本项目唯一一处自己维护的索引。
   原因两条：`lv_chart_series_t` 不是 `lv_obj_t`（进不了对象句柄表，也就没有"对象被删时
   自动失效"那套机制），而 `lv_chart.h` **不公开结构体**（拿不到 `chart->series[i]`，
   没法按索引回头问 LVGL）。
   由此有两条使用者必须知道的后果：**图表被删除（含被父对象级联删掉）后，它的 series
   索引一律不可用**；**`remove()` 之后该索引永久失效**，新加的曲线拿新索引、**不复用
   旧号** —— 这个取舍是刻意的：让"旧索引悄悄指向新曲线、于是安静地画出错数据"不可能发生。
   （判据用的是"图表句柄是否仍 ALIVE"而不是"我们记得删过"，因为级联删除不经过我们的
   包装函数。）

   ★ 使用 `LvKeyboard` 有一条必须遵守的契约：**删除 textarea 之前要先
   `clearTextarea()`**。LVGL 不会在 textarea 被删时清理这条绑定（读实现确认：
   键盘单向持有裸指针，`lv_keyboard.c` 无 `LV_EVENT_DELETE` 处理，而 `lv_textarea.c`
   完全不引用 keyboard），按键处理却直接解引用它 —— 删后按键就是踩悬空指针，
   且崩溃点在 LVGL 的事件回调里，本层没有可拦截的位置。
   （`setTextarea` 的入参类型是 `LvTextarea` 而非 `LvObject`，把"传错对象"这条
   约束交给编译期而不是文档。）

   ★ 封装 `table` 时在本项目锁定的 **LVGL v9.2.2** 上发现一个**上游缺陷**：
   `lv_table_set_column_count` 在**缩小列数**时会解引用 NULL 而段错误
   （`lv_table.c:280` 的 `if(table->cell_data[idx]->user_data)` 缺 NULL 检查；
   兄弟函数 `lv_table_set_row_count` 在同样位置**有**该检查 —— 这正是判定
   它是上游问题而非我们误用的依据）。
   触发条件平凡到不能不管：**先设一格把表撑开，再把列数调小**。
   处置：版本是冻结的，不改第三方源码，在 C 层缩小前把所有将被丢弃的格子填成空串
   （O(丢弃格数)，只在缩小时发生）。回归用例 **`★★ 缩列不崩溃`** 钉住它 ——
   升级 LVGL 时该用例就是哨兵。
 3. **内置 CJK 字库不是"中文可用"的开关。** 官方描述为
   `1000 most common CJK radicals`，但实测（`native/probe/probe_font_coverage.c`）
   是一份**手工拼合的混合清单**：约 1118 个 CJK 字 + 173 个日文假名，
   **有缺字**（问/厅/灯/调/窗/帘/显 缺失，而 問/調/窓 在册）。
   缺字时 LVGL **只打一行日志，画面上直接少一块，不报错、不抛异常**。
   因此「这段文案能不能显示」只能逐个字符核对 —— 不存在一个开关式的答案。
   可用 `LvFont.builtin(...).missingGlyphs("文本")` 自查。
4. ~~**性能数字全部来自 WSL2，不是目标平台。**~~ **目标平台基线已采集**（2026-09-22，
   见 `docs/benchmarks/2026-09-22_perf_target_pizero2w.txt`）：Pi Zero 2 W 上
   `ffi.raw` **7.09 μs/次**（WSL2 上是 1.74–1.96 μs，相差约 4 倍）、
   `fps.partial` **30**、`fps.fullscreen` **14**、启动到首帧 **328 ms**、内存峰值占用 **2%**。
   §11.2 要求量化指标在目标平台判定 —— 现在判定依据取自目标平台，
   但仍然**不是**「性能验收通过」：门槛（2 μs / ≥30 FPS）与目标平台的实测值需要重新校准。
   `fps.partial = 30` 顶到的是刷新周期 `LV_DEF_REFR_PERIOD = 33ms` 的**周期上限**，
   与渲染能力无关（CPU 忙碌比仅 10–20%）；要更高 FPS 必须下调刷新周期。
5. ~~**24h soak 未运行。**~~ **已于 2026-09-23 完成**（目标机连续 86400 秒）：
   1.57 亿步确定性负载、10 项检查 0 失败、末态计数归位（`alive 2→2` / `obj 6→6` /
   `anim_ctx=0`）、RSS 平台化（末段与中段同为 3264 KB，峰值出现在第 60 秒的预热）。
   证据与逐行产物：`docs/benchmarks/2026-09-23_soak_target_24h.md`。
   ★ 仍未覆盖：多轮重复、fd / 线程等其它资源的采样。
6. **macOS 未验证**（本机环境是 WSL2，无法验证）。CI 上曾经失败（`test_queue` 3 项），
   根因在**测试侧**：拿 `usleep` 当同步原语、且未建立"队列为空"的前提 ——
   该修复**已落地**（提交 `ce097e5`；`21bcb5b` 又把"任务没入队"与"入了队没人唤醒"分开诊断），
   本机（Linux）上该用例全绿；**待 CI 复验**。
7. ~~**真机未验证 SDL2 后端。**~~ **已验证**（2026-09-23，见平台支持一节）：
   目标机上用 **KMSDRM 直出**跑通 `hello_cj`（1920×1080、72 帧、截图取回分析无黑带/无重影、
   实际驱动名实测为 `KMSDRM`）；FPS / 刷新基线也已于 2026-09-22 在目标机上采集。
   ★ 仍未做的部分：aarch64 侧的 `test_sdl2_backend`（那组 C 断言）未构建 ——
   目标机上验证的是**端到端示例**，不是这些断言；
   另外 KMSDRM 要求独占 DRM master（跑前必须让出桌面，脚本已处理），
   且该设备上一次 Present 会超过 100 ms —— 这是性能特征，
   已不再造成丢数据（判据 `slotClobber` 恒为 0，见提交 `8d20351`）。
8. **真机长跑只做过一轮**：24 小时通过是实质证据，但单轮不等于统计结论 ——
   偶发问题要靠多轮或更长时间才可能暴露（另：长跑用的是 ASan 构建，不能当性能数据）。
9. **模糊测试未做**：当前 soak 是确定性负载形态（§11.4 未完成）。
10. **输入只在「合成事件」下验证过，没有真实输入硬件。** 指针 / 滚轮 / 按键都走
    「SDL 事件 → 后端 `feedIndev` / `takeWheelSteps` / `takeKey`」这一条路，
    测试与示例用的是注入与 SDL 鼠标事件（`injectWheel`、探针、CI 里的 dummy 驱动）。
    **触摸屏、旋转编码器、实体按键都没有在真实硬件上验证过。**
    触摸设备上 SDL 通常把触摸事件转成鼠标事件，理论上走同一条路 —— 但"理论上"不算验证，
    故如实标为缺口。由此：小屏触摸面板示例（`examples/hmi_panel`）**暂时不做** ——
    它的核心交互恰恰是这条未验证的路，写了也无法给出证据。

---

## 目录结构

```
src/            L1 仓颉接口（19 个模块 + ffi_bridge.cj + 9 个测试文件）
                generated/ 由 scripts/gen_conf_const.py 生成，勿手改
native/         C 实现（22 个 .c）
  include/      lvglcj_bridge.h（对外契约，293 个声明，逐条注释）
                lvglcj_internal.h（内部）
  cmake/        含 aarch64-linux-gnu.cmake 交叉工具链
backend/        sdl2/（窗口与 KMSDRM 直出）+ null/（无显示环境，同时提供 ABI 兜底定义）
test/native/    C 用例（10 个）+ target_suite.sh / target_soak.sh（目标机侧执行脚本）
native/probe/   诊断探针（6 个，用于把争议变成实测）
examples/       hello_cj（最小示例）、gallery_cj（全控件展示）、bench_cj（性能采集）
scripts/        构建、门禁、性能、soak、真机部署、KMSDRM 实测、配置生成等 18 个脚本
docs/           结果与基线记录（见下）
third_party/    LVGL 源码（由 fetch_lvgl.sh 拉取，不入库）
```

---

## 文档索引

| 文档 | 内容 |
| --- | --- |
| `lvgl4cj_设计方案_v0.4_冻结版.md` | **设计依据**：接口契约、不变式、验收门槛（§11.2/§11.3） |
| `docs/P0_RESULTS.md` | P0 实施记录：每个断言的实测结论、定位过程、修复（可用 `scripts/run_probe.sh` 复现） |
| `docs/C4_DEFECT.md` | 一处已知缺陷的记录（回调帧内构造异常导致 SIGSEGV；结论为平台限制） |
| `docs/benchmarks/2026-09-20_P0_gate.md` | 门禁与内存基线 |
| `docs/benchmarks/2026-09-20_soak_scaled_{120s,300s}.txt` | P0 期缩放 soak 记录（含一条 **FAIL**：300s 实际只跑 293s —— 如实保留，未删改） |
| `docs/benchmarks/2026-09-21_perf_soak_platform.md` | 性能 / soak / 平台矩阵（含真机实测） |
| `docs/benchmarks/2026-09-22_perf_target_pizero2w.txt` | **目标平台性能基线**（Pi Zero 2 W；§11.2 的判定依据） |
| `docs/benchmarks/2026-09-23_soak_target_24h.md` | **24 小时长跑证据**（附逐行产物：RSS 曲线、全部 stdout） |

> `docs/benchmarks/` 下的文件按**日期**命名，各自是那一次运行的**快照**：
> 数字不随代码更新而回溯修改；有新结论就另开文件并注明取代关系
> （例：`2026-09-23_soak_target_24h.md` 取代了 09-21 那篇里的"长跑未做"）。
>
> 设计文档的早期版本（v0.1「设计稿」/ v0.2 / v0.3 / v0.4 非冻结）已从工作区移除：
> 它们被**冻结版**取代，且仓库内**无任何引用**。需要时从 git 历史取回：
> `git log --diff-filter=D --name-only -- 'lvgl4cj_设计方案*'`

---

## 开发约定

- **改了 C 侧就要重建库**：`bash scripts/build_native.sh`（会更新 `libs/` 并做 ABI 自检）
- **提交前跑门禁**：`bash scripts/gate.sh`（8 项全绿才算过）
- **格式化**：`cjfmt -d src`（就地）；生成文件也必须在格式化后零 diff
- **新增 ABI 符号**：契约头的注释要写清**语义与失败方式**，并同步 L1 包装 + 测试
- **不要写"看起来对"的注释**：本项目已出现过多次「注释与实测不符」的情况
  （如内置字库被描述成"常用汉字表"），正确措辞往往就在仓库里，先核对再落笔
