# lvgl4cj 设计方案
## 仓颉语言绑定 LVGL 的 GUI 框架

> 版本：**v0.2**（针对 v0.1 评审意见修订）
> 目标 LVGL 版本：**v9.x**（主线，锁 9.2+）
> 目标仓颉 SDK：**1.1.0**
> 定位：面向 **Linux / ARM64 Linux / OpenHarmony 的嵌入式 HMI**，桌面 SDL2 作为开发与验证环境

---

## 修订说明：v0.1 → v0.2

本版针对评审意见逐项补齐。下表用于快速定位，正文不再重复解释"为什么补"。

### A 类：可能导致返工或事故的缺陷

| # | 评审意见 | 修订位置 | 处置 |
|---|---|---|---|
| A1 | 线程模型未区分仓颉轻量级线程与 OS 线程 | **§3.6 重写** | 给出三方案 + **P0 阻塞性验证项 V1**，默认走 C 侧 OS 线程 |
| A2 | 32 位平台 `closure_id` 转 `void*` 截断 | **§3.2** | 改用 `intptr_t` 显式转换 + 明确"MVP 仅支持 64 位"约束 |
| A3 | 事件语义缺冒泡 / trickle / current_target | **§3.7 新增** | 补全事件模型，含 `stop_bubbling`、`current_target`、事件码全表 |
| A4 | 动画 API 完全缺失 | **§3.10 新增** | `lv_anim` FFI 设计 + exec_cb trampoline + 生命周期 |
| A5 | 绘制事件与自定义绘制未表态 | **§3.11 新增** | 明确 `lv_draw_*` 为**非目标**，改用 Canvas 路径 |
| A6 | 句柄表状态机不完整 | **§3.1 重写** | 四态状态机 + 转移图 + 幂等性定义 |
| A7 | 回调重入规则缺失 | **§3.8 新增** | 延迟删除队列 + 回调期句柄锁定 + 重入矩阵 |
| A8 | 任务队列语义缺失 | **§3.9 新增** | 有界队列 + 背压 + `Future` + 关闭策略 + 线程断言 |
| A9 | 绘制缓冲细节不足 | **§3.4 重写** | stride 对齐、颜色格式、DMA/cache、旋转、`flush_wait_cb`、模式差异 |
| A10 | 配置 hash 编译期写入机制未说明 | **§3.5** | 补代码生成流程、文件位置、增量构建 |
| A11 | 错误码映射与上下文不足 | **§3.12 + 附录 B** | 完整错误码表 + 映射规则 + 错误上下文 + 全局错误回调 |

### B 类：API 覆盖与语义缺口

| 评审意见 | 修订位置 |
|---|---|
| 控件清单不全 | **§7.1** 完整控件清单与分批 |
| 文件系统 / 图片 / 字体 | **§3.13 + §5.10** `lv_fs`、图片解码、字体加载 |
| Group / 焦点 / 键盘导航 | **§3.13 + §5.4** `lv_group` 与 indev 绑定 |
| 样式属性覆盖不足 | **§5.7 + §7.2** 补全到 40+ 属性 |
| 事件枚举未定义完整 | **§3.7 表 3-7** 完整事件码表 |
| 调试 / 可观测性 API | **§5.11 + §7.3** 对象树 dump、截图、性能计数器 |
| API 稳定性标记 | **§7.4** `@Experimental` 标记与弃用策略 |

### C 类：工程化与文档完整性

| 评审意见 | 修订位置 |
|---|---|
| C1 构建系统集成 | **§9.1** cjpm × CMake 协作、符号可见性、动静库 |
| C2 SDL2 后端细节 | **§8.1 重写** 窗口/纹理/事件循环/像素格式/坐标变换 |
| C3 OH 后端可行性 | **§8.3 新增** NativeWindow / Vsync / Input / NAPI 约束分析 |
| C4 交叉编译矩阵 | **§9.3** 平台矩阵 + headless |
| C5 测试框架与 CI | **§10** 仓颉/C 测试框架、CI 矩阵、模糊测试 |
| C6 ASan 与仓颉 GC 兼容 | **§9.5 新增** 隔离策略 |
| C7 版本发布与治理 | **§14** 语义化版本、分支模型、PR/issue 模板 |
| C8 许可与 SBOM | **§14.2** SDL2/FreeType/libpng 许可清单 + SBOM 格式 |
| C9 缺 ADR | **§16** 关键决策记录 |
| C10 缺量化验收指标 | **§11 新增** 帧率、延迟、内存、CPU 具体门槛 |
| C11 风险缺 owner/deadline | **§13** 风险登记册补 owner / 触发条件 / 评估点 |
| C12 文档结构不完整 | **附录 A** 术语表；正文补状态机图、时序图 |
| C13 缺与现有绑定对比 | **§15** 对比 `lv_binding_rust` / MicroPython / lvgl-js |
| C14 缺用户文档规划 | **§14.4** 文档站点与教程规划 |

---

## 〇、一句话结论

**把项目定义为"受控 C ABI 工程"，而不是"自动生成 C 绑定的练习"。**

LVGL 是纯 C 库，ABI 层面比 Qt/C++ 简单；工程量集中在四件事：**线程亲和性、回调桥接、生命周期级联失效、绘制缓冲所有权**。这四件做对了，控件覆盖率只是时间问题。

---

## 一、项目定位与命名

### 1.1 命名

**`lvgl4cj`**，仓颉包名 `lvgl4cj`。与 TPC 现有风格一致（`lrc4cj`、`vlayout4cj`）。

### 1.2 支持矩阵（明确约束，避免后期争议）

| 维度 | MVP 支持 | 说明 |
|---|---|---|
| **指针宽度** | **仅 64 位** | ⚠️ 32 位平台 `void*` 与 `Int64` 不兼容（见 §3.2），二期评估 |
| 操作系统 | Linux x86_64 / ARM64、macOS（仅开发验证） | 见 §9.3 平台矩阵 |
| 目标场景 | 嵌入式 HMI、工业面板、智能设备 | **不含 MCU/RTOS**，见 §13 决策门 G1 |
| LVGL 版本 | 锁定 v9.2+ | 不承诺跨大版本兼容 |
| 线程模型 | 单 LVGL OS 线程 | 见 §3.6 |
| 仓颉 SDK | 1.1.0 | 见 §3.6 待确认项 V1 |

### 1.3 设计目标（按优先级）

1. **安全**：句柄失效可检测，无悬空指针与 double-free
2. **可调试**：错误有明确上下文，崩溃可定位到仓颉调用点
3. **确定性**：线程模型与重入行为有明确定义，不出现间歇性崩溃
4. **够用**：覆盖 HMI 常见控件与交互
5. **快**：UI 线程不被阻塞，FFI 开销可控

### 1.4 非目标（明确排除）

| 非目标 | 原因 | 替代路径 |
|---|---|---|
| 裸机 / MCU / RTOS | 仓颉运行时资源模型未验证（决策门 G1） | C 主控 + 仓颉业务 |
| 多线程并发调用 LVGL | LVGL 非线程安全 | 单线程 + 任务投递（§3.6/§3.9） |
| **`lv_draw_*` 底层绘制 API** | v9 draw unit 模型复杂，FFI 暴露成本极高 | **用 Canvas 控件**（§3.11） |
| 自动生成全量 API | 宏与回调必须手写 | 生成器只覆盖 setter/getter（§3.5） |
| 新的声明式 UI 框架 | 超出绑定层职责 | — |
| 32 位平台 | `void*` 截断风险（A2） | 二期用独立句柄表 |

---

## 二、总体架构：四层

```
┌─────────────────────────────────────────────────────────┐
│  L0  应用层（用户仓颉代码）                                │
└───────────────────┬─────────────────────────────────────┘
                    │ 仓颉调用
┌───────────────────▼─────────────────────────────────────┐
│  L1  仓颉安全 API 层      src/                            │
│      LvObject / LvStyle / LvEvent / LvTimer / LvAnim     │
│      LvDisplay / LvIndev / LvGroup / LvFs / LvglRuntime  │
│      · 句柄封装，不暴露裸指针                              │
│      · Resource 语义 + close()                            │
│      · 样式/事件/动画 DSL                                 │
│      · 线程调度：post { } → LVGL OS 线程                  │
└───────────────────┬─────────────────────────────────────┘
                    │ C ABI（扁平、稳定、无 C++ 符号）
┌───────────────────▼─────────────────────────────────────┐
│  L2  C 桥接层            native/ → liblvgl4cj_bridge     │
│      · 句柄表（ptr ↔ int64，四态状态机）                  │
│      · trampoline 集合（event/timer/anim/flush/read/fs）  │
│      · 闭包注册表（closure_id ↔ 仓颉闭包）                │
│      · 延迟删除队列（回调期安全删除）                      │
│      · LVGL OS 线程 + 任务队列                            │
│      · 绘制缓冲分配（stride 对齐）                        │
│      · 宏 → 常量查询 + 配置哈希                           │
└───────────────────┬─────────────────────────────────────┘
                    │ 原生 C 调用
┌───────────────────▼─────────────────────────────────────┐
│  L3  LVGL 原生           third_party/lvgl                │
│      锁定版本 + 固定 lv_conf.h + 补丁                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  L4  后端层（可插拔）                                      │
│      backend_sdl2    Linux/macOS 桌面（MVP）              │
│      backend_fbdev   Linux framebuffer                   │
│      backend_drm     Linux ARM64 HMI                     │
│      backend_oh      OpenHarmony（二期，见 §8.3）          │
│      backend_custom  自定义 flush/read 回调               │
└─────────────────────────────────────────────────────────┘
```

### 2.1 为什么必须有 L2

| 直接 FFI 的问题 | L2 解决方式 | 章节 |
|---|---|---|
| `lv_obj_t*` 裸指针，无失效检测 | 句柄表 + 四态状态机 | §3.1 |
| 回调是 C 函数指针，仓颉闭包传不进 | trampoline + closure_id | §3.2 |
| 32 位 `void*` 截断 | `intptr_t` 显式转换 + 64 位约束 | §3.2 |
| 仓颉线程可能被 M:N 迁移 | C 侧 OS 线程跑主循环 | §3.6 |
| 回调中删除对象导致重入崩溃 | 延迟删除队列 | §3.8 |
| `lv_conf.h` 编译期宏读不到 | 常量查询 + 哈希校验 | §3.5 |
| 绘制缓冲被 GC 移动 | C 侧对齐分配并持有 | §3.4 |
| 父删子导致子句柄悬空 | `LV_EVENT_DELETE` 级联失效 | §3.3 |
| 错误靠 assert，仓颉无感知 | 统一错误码 + 上下文 + 回调 | §3.12 |

---

## 三、核心机制（13 项）

### 3.1 句柄表与完整状态机 ★修订 A6

#### 3.1.1 四态定义

| 状态 | 含义 | 原生对象 | 句柄表项 | `ptr_of()` |
|---|---|---|---|---|
| `UNINIT` | 未初始化 | — | 无 | NULL |
| `ALIVE` | 有效 | 存在 | 存在 | 有效指针 |
| `INVALIDATED` | 原生对象已删除 | 已释放 | **仍存在** | NULL |
| `RELEASED` | 句柄表项已回收 | — | 已移除 | NULL |

**`INVALIDATED` 与 `RELEASED` 的区别是本设计的关键**：
- `INVALIDATED` 保留表项，是为了让仓颉侧能给出**明确报错**（"对象已被删除"）而非崩溃，同时保留调试信息（删除时的调用栈）
- `RELEASED` 才是真正释放表项内存

#### 3.1.2 状态转移图

```
                 create / handle_of
                        │
                        ▼
                   ┌─────────┐
                   │  ALIVE  │◄──────────┐
                   └────┬────┘           │
                        │                │
        ┌───────────────┼────────────────┼──────────────┐
        │               │                │              │
   close()       LV_EVENT_DELETE    (无操作)       (无操作)
   (显式)         (LVGL 内部删除)          │              │
        │               │                │              │
        ▼               ▼                │              │
   ┌─────────────────────────┐           │              │
   │      INVALIDATED        │           │              │
   │  (保留表项 + 删除栈)      │           │              │
   └───────────┬─────────────┘           │              │
               │                          │              │
        release()                         │              │
               │                          │              │
               ▼                          │              │
        ┌─────────────┐                   │              │
        │  RELEASED   │                   │              │
        └─────────────┘                   │              │
                                          │              │
   任何状态下再次 close() ─────────────────┘              │
   （幂等，直接返回）                                      │
                                                          │
   任何状态下再次 release() ──────────────────────────────┘
   （幂等，直接返回）
```

#### 3.1.3 幂等性契约（写死，必须单测覆盖）

| 操作 | 在 `ALIVE` | 在 `INVALIDATED` | 在 `RELEASED` |
|---|---|---|---|
| `close()` | → INVALIDATED | 无操作（幂等） | 无操作（幂等） |
| `release()` | → INVALIDATED → RELEASED | → RELEASED | 无操作（幂等） |
| `isAlive()` | true | false | false |
| 任何业务 API | 正常 | 抛 `InvalidHandle` | 抛 `InvalidHandle` |

#### 3.1.4 `close()` 内部的顺序安全（评审 A6 的具体问题）

问题：`close()` 里先 `lvglcj_obj_delete`，DELETE 钩子会 `invalidate`，然后仓颉侧再 `release`，中间状态是否安全？

**答案：安全，因为 INVALIDATED 正是为这个中间态设计的。** 完整时序：

```
仓颉 obj.close()
  │
  ├─ 1. lvglcj_obj_delete(h)
  │     └─ C 侧 lv_obj_delete(obj)
  │           └─ LVGL 同步触发 LV_EVENT_DELETE
  │                 └─ lvglcj_delete_hook(e)
  │                       ├─ (a) 注销该对象所有 closure（从闭包表移除，防泄漏）
  │                       ├─ (b) 递归 invalidate 子句柄
  │                       └─ (c) invalidate 自身句柄 → 状态 = INVALIDATED
  │
  ├─ 2. 回到仓颉侧，调 lvglcj_handle_release(h)
  │     └─ 状态 INVALIDATED → RELEASED
  │
  └─ 3. closed = true（本地标记，保证仓颉侧幂等）
```

关键点：**步骤 (a) 必须在 (b)(c) 之前**。反了会导致失效过程中触发已注销/半注销的闭包。这个顺序在 C 侧 `lifecycle.c` 里写死，并加注释锁定。

#### 3.1.5 LVGL 内部删除时仓颉如何感知

场景：screen 切换（`lv_screen_load` + `auto_del=true`）、`lv_obj_clean`、控件内部逻辑删除子对象。

**统一靠 `LV_EVENT_DELETE` 钩子**——只要对象是通过 `lvglcj_obj_create` 创建的，就一定挂了钩子，无论谁删都能感知。

⚠️ **例外**：LVGL 内部自己创建的对象（如 dropdown 的列表、msgbox 的按钮）不经过我们的创建入口，没有钩子。**策略**：这类对象不向仓颉暴露句柄，或者在暴露前由 C 侧补挂钩子。

---

### 3.2 回调桥接与 32 位安全 ★修订 A2

#### 3.2.1 closure_id 传递的 32 位问题

v0.1 写 `(void*)closure_id`，`int64 → void*` 在 32 位截断。**修订**：

```c
#include <stdint.h>

// closure_id 使用 int32_t 范围，保证 intptr_t 可无损往返
#define LVGLCJ_CLOSURE_ID_MAX  (INT32_MAX)

static inline void *lvglcj_cid_to_ptr(int32_t cid) {
    return (void *)(intptr_t)cid;       // ★ 显式经 intptr_t，非直接强转
}

static inline int32_t lvglcj_ptr_to_cid(void *p) {
    return (int32_t)(intptr_t)p;        // ★ 对称还原
}
```

配套约束：
- 闭包表使用 `int32_t` ID，分配时检查不超过 `INT32_MAX`，超出则复用或报错
- **MVP 明确仅支持 64 位平台**（§1.2）。32 位（含 ARM32 OH 设备）需二期评估：改用独立句柄表传 ID，而非指针编码

#### 3.2.2 trampoline 集合

每类回调一个固定 C 函数，共 6 个：

| # | trampoline | 对应 LVGL 回调 | user_data 装载 |
|---|---|---|---|
| T1 | `lvglcj_event_trampoline` | `lv_event_cb_t` | closure_id |
| T2 | `lvglcj_timer_trampoline` | `lv_timer_cb_t` | closure_id |
| T3 | `lvglcj_anim_exec_trampoline` | `lv_anim_exec_xcb_t` | closure_id |
| T4 | `lvglcj_flush_trampoline` | `lv_display_flush_cb_t` | closure_id |
| T5 | `lvglcj_indev_read_trampoline` | `lv_indev_read_cb_t` | closure_id |
| T6 | `lvglcj_fs_trampoline` | `lv_fs_drv_t` 各回调 | drv_id + op |

事件 trampoline 示例：

```c
static void lvglcj_event_trampoline(lv_event_t *e) {
    int32_t cid = lvglcj_ptr_to_cid(lv_event_get_user_data(e));

    // ★ 事件句柄也走句柄表，避免把 lv_event_t* 直接暴露给仓颉
    int64_t evh = lvglcj_handle_of(e);

    // ★ 异常不得跨越 C 边界：仓颉侧回调的返回值即异常标记
    int rc = lvglcj_call_closure(cid, evh);
    if (rc != 0) {
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, cid, __func__);
    }
    // 事件对象是栈上临时结构，回调返回后立即失效
    lvglcj_handle_invalidate(evh);
    lvglcj_handle_release(evh);
}
```

#### 3.2.3 仓颉侧闭包捕获能力：**待确认项 V2**

v0.1 假定"仓颉 FFI 回调不能捕获局部变量"。但 CJQT6 中存在 `setOnTimeoutCapture({ => ... })` 这类**可捕获闭包**变体，说明仓颉可能原生支持捕获回调。

**P0 必须先确认**：

| 情况 | 处置 |
|---|---|
| 仓颉原生支持捕获回调 | **直接用**，省掉自建闭包表，大幅简化设计 |
| 仅顶层 `@C func` 支持 | 沿用自建闭包表 + closure_id 方案（§3.2.1） |
| 需运行时附着才能调仓颉 | 见待确认项 V1（§3.6） |

这条影响 §3.2 与 §3.8 的实现复杂度，**必须在 P0 第一周内确认**。

---

### 3.3 生命周期级联失效（沿用 v0.1，补充细节）

见 §3.1.4 时序。补充两条 v0.1 未明确的：

1. **递归失效的顺序**：先深后浅（叶子 → 根），保证父对象在处理时子对象已失效
2. **`lv_obj_clean()`**：清空子对象但保留父对象。钩子需区分"clean"与"delete"——通过 `lv_event_get_code` 判断，v9 中 clean 触发的是 `LV_EVENT_DELETE`（对每个子），因此父对象不应被失效

---

### 3.4 绘制缓冲 ★重写 A9

#### 3.4.1 为什么必须在 C 侧分配

**LVGL v9 的 `lv_display_set_buffers()` 接收裸缓冲区指针并在 flush 期间长期持有。**

⚠️ **绝不能把仓颉 `Array<UInt8>` 的底层指针传给 LVGL** —— GC 可能移动或回收，导致花屏/段错误。**缓冲必须在 C 侧分配并由桥接层持有。**

#### 3.4.2 stride 对齐（v0.1 遗漏）

LVGL v9 引入 `lv_draw_buf_t` 与 stride 对齐概念。**缓冲区大小不能简单按 `w * h * bpp` 计算**：

```c
// 错误的 v0.1 算法
buf_bytes = w * h / 10 * bytes_per_pixel;     // ✗ 未考虑 stride 对齐

// 正确算法
size_t lvglcj_calc_buf_bytes(int32_t w, int32_t h, int32_t bpp) {
    // 每行字节数按 LV_DRAW_BUF_STRIDE_ALIGN 向上取整
    size_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    return stride * h;                         // ✓ 用 LVGL 自己的算子
}
```

**原则：任何缓冲尺寸计算都调 LVGL 提供的 `lv_draw_buf_*` 算子，不自己算。** 不同版本对齐要求不同，硬编码必然出错。

#### 3.4.3 颜色格式与字节序

v9 中 `lv_color_t` 恒为 RGB888，但**显示缓冲格式由 `lv_display_set_color_format()` 独立决定**，两者不同：

| 格式 | bpp | 说明 |
|---|---|---|
| `LV_COLOR_FORMAT_RGB565` | 2 | 最常见，MCU 屏首选 |
| `LV_COLOR_FORMAT_RGB888` | 3 | 无 alpha |
| `LV_COLOR_FORMAT_ARGB8888` / `XRGB8888` | 4 | 桌面/带 GPU 场景 |

⚠️ **flush 回调签名 v9 为 `(lv_display_t*, const lv_area_t*, uint8_t* px_map)`** —— 第三参是 `uint8_t*`，**不是 v8 的 `lv_color_t*`**。后端必须按 `lv_display_get_color_format()` 决定按 2/3/4 字节解释，否则"偶尔花屏"。

字节序：RGB565 在 SPI 屏上通常需 `lv_draw_sw_rgb565_swap()` 或传输时交换，后端需提供开关。

#### 3.4.4 三种渲染模式差异

| 模式 | 缓冲大小 | flush 语义 | 适用 |
|---|---|---|---|
| `PARTIAL` | ≥ 1/10 屏 | 只刷脏区，分块调用多次 | **MVP 默认**，省内存 |
| `DIRECT` | **整屏** | 只刷脏区，但缓冲含完整图像 | 有足够 RAM |
| `FULL` | **整屏** | 每次全刷 | 双缓冲传统模型 |

分块处理：LVGL 可能多次调用 flush，用 `lv_display_flush_is_last()` 判断最后一块。**后端不能在非最后块时提交帧**。

#### 3.4.5 `flush_wait_cb`（v0.1 遗漏）

异步传输（DMA）场景下，若 flush 返回后传输未完成就继续渲染，会撕裂。v9 提供：

```c
lv_display_set_flush_wait_cb(disp, my_wait_cb);   // LVGL 用它等待传输完成
```

**作用**：不用在 flush 里自旋阻塞，LVGL 通过 wait_cb 用信号量/轮询高效等待。**后端若支持异步传输，应实现 wait_cb**；同步传输（SDL2 纹理拷贝）可省略。

#### 3.4.6 缓存一致性与 DMA

ARM 带 cache 的平台，DMA 直接读缓冲会读到脏数据。需要：
- 缓冲分配考虑 cache line 对齐（≥64B）
- 传输前 `clean`，接收后 `invalidate`
- 由后端提供 `flush_cb` 内的 cache 操作钩子（MVP 不涉及，写入设计约束）

#### 3.4.7 其他

| 项 | 处理 |
|---|---|
| **malloc 失败** | `lvglcj_display_create` 返回负错误码（`ERR_OUT_OF_MEMORY`），仓颉侧抛异常；**不静默降级** |
| **旋转** | `lv_display_set_rotation()` 会改变宽高语义，后端需监听 `LV_EVENT_RESOLUTION_CHANGED` 重建缓冲 |
| **删除顺序** | 先 `lv_display_delete()`（LVGL 停止引用），**再** `free()` 缓冲。反了会 use-after-free |
| **双缓冲同步** | DIRECT/FULL 模式下 LVGL 内部管理两块缓冲的乒乓，桥接层只负责分配与最终释放 |

---

### 3.5 `lv_conf.h` 版本化与代码生成 ★修订 A10

#### 3.5.1 宏 → 查询函数

```c
// native/conf_probe.c
int32_t lvglcj_conf_color_depth(void)  { return LV_COLOR_DEPTH; }
int32_t lvglcj_conf_use_log(void)      { return LV_USE_LOG; }
int32_t lvglcj_conf_mem_size(void)     { return LV_MEM_SIZE; }
int32_t lvglcj_conf_stride_align(void) { return LV_DRAW_BUF_STRIDE_ALIGN; }
uint32_t lvglcj_conf_hash(void);        // 关键宏拼接后 FNV-1a
const char *lvglcj_version(void);       // "9.2.x"
```

#### 3.5.2 编译期哈希如何写入仓颉（v0.1 未说明的机制）

仓颉没有 C 预处理器，**必须靠构建脚本生成 `.cj` 源文件**：

```
构建流程：
  1. CMake 编译 native 层，同时编译一个"探针小程序" probe_conf
  2. 运行 probe_conf，输出 JSON：
     { "conf_hash": "0xA1B2C3D4", "version": "9.2.1",
       "color_depth": 32, "stride_align": 64 }
  3. scripts/gen_conf_const.py 读取 JSON，生成：
     src/generated/conf_const.cj
  4. cjpm build 编译该文件
```

生成文件内容：

```cangjie
// AUTO-GENERATED by scripts/gen_conf_const.py — DO NOT EDIT
package lvgl4cj.generated

public const CONF_HASH: UInt32 = 0xA1B2C3D4
public const LVGL_VERSION: String = "9.2.1"
public const COLOR_DEPTH: Int32 = 32
public const STRIDE_ALIGN: Int32 = 64
```

**增量构建**：CMake 中把 `lv_conf.h` 声明为 `configure_file` 的依赖；JSON 内容未变时不重写 `.cj` 文件（避免触发无谓重编译）。

#### 3.5.3 启动校验

```cangjie
func checkCompatibility(): Unit {
    if (lvglcj_conf_hash() != CONF_HASH) {
        throw LvglException(
            LvglError.InvalidConfig,
            "LVGL 构建配置不匹配：绑定层基于 hash=0x${CONF_HASH}，" +
            "当前库 hash=0x${lvglcj_conf_hash()}。请重新编译 native 层。"
        )
    }
}
```

---

### 3.6 线程模型 ★重大修订 A1

#### 3.6.1 问题本质

**LVGL 要求所有 `lv_*` 调用在同一个 OS 线程内。仓颉的 `spawn` 创建的是轻量级线程（用户态线程，M:N 调度），可能被调度到不同 OS 线程上。**

v0.1 说"起一个专用线程跑主循环"是错的——如果那个线程是仓颉轻量级线程，它就可能在 OS 线程间迁移，单线程模型直接失效，且失败是**间歇性的**（P0 可能"看起来跑通"）。

#### 3.6.2 三个候选方案

| 方案 | 机制 | 优点 | 风险 |
|---|---|---|---|
| **A（推荐）** | **C 侧 `pthread_create` OS 线程**跑主循环；仓颉只通过队列投递任务 | OS 线程确定，与仓颉调度解耦 | 需仓颉支持"外部 OS 线程调用仓颉闭包" |
| B | 仓颉侧 `spawn` + 验证亲和性 | 纯仓颉，无附着问题 | 亲和性不可控，M:N 迁移即失效 |
| C | 泵模式：仓颉主线程定期调 `lvglcj_pump()` | 最简单 | UI 与业务抢同一线程，阻塞即卡顿 |

#### 3.6.3 方案 A 的关键未知：**待确认项 V1（P0 阻塞项）**

方案 A 要求在一个**由 C 创建、仓颉运行时不认识的 OS 线程**里执行仓颉闭包。这需要仓颉运行时提供线程附着机制（类似 JNI `AttachCurrentThread`）。

```
C OS 线程循环：
  while (running) {
      task = queue_pop();              // 取仓颉投递的任务
      if (task) {
          lvglcj_attach_cangjie();     // ★ 需要这一步，仓颉是否提供？
          call_cangjie_closure(task);
          lvglcj_detach_cangjie();
      }
      lv_timer_handler();
      sleep_ms(5);
  }
```

**P0 第一周必须确认**（这是全项目最大的技术不确定性）：

| 确认项 | 方法 |
|---|---|
| 仓颉是否提供外部 OS 线程附着 API | 查 SDK 文档 / 问仓颉社区 / 读 `cangjie_runtime` 源码 |
| 若没有，仓颉轻量级线程是否稳定绑定 OS 线程 | **探针测试**：在 `spawn` 的循环里连续 1000 次记录 `gettid()`，统计是否变化 |
| 若也没有，闭包执行是否仅限仓颉线程 | 则需退回方案 C（泵模式） |

#### 3.6.4 探针测试（P0 第 1 周交付）

```c
// C 侧
int64_t lvglcj_current_os_tid(void);   // Linux: gettid(), macOS: pthread_threadid_np()
```

```cangjie
// 测试：仓颉线程在循环中是否迁移 OS 线程
func probeThreadAffinity(): Unit {
    var tids = HashSet<Int64>()
    let t = spawn {
        for (_ in 0..10000) {
            tids.put(lvglcj_current_os_tid())
            sleep(1.milliseconds)      // ★ 故意 sleep，触发可能的迁移
        }
    }
    t.join()
    println("观察到 ${tids.size} 个不同 OS 线程 ID")
    // size == 1 → 亲和性稳定，方案 B 可行
    // size  > 1 → 必须走方案 A 或 C
}
```

**这个测试结果是方案选型的唯一依据，不许拍脑袋。**

#### 3.6.5 线程断言（无论选哪个方案都要有）

所有 `lv_*` C 入口加断言（Debug 构建）：

```c
#define LVGLCJ_ASSERT_LVGL_THREAD()                                    \
    do {                                                               \
        if (lvglcj_os_thread_self() != g_lvgl_thread_id) {              \
            lvglcj_record_error(LVGLCJ_ERR_WRONG_THREAD, 0, __func__);  \
            return LVGLCJ_ERR_WRONG_THREAD;                             \
        }                                                               \
    } while (0)
```

**价值**：把"间歇性崩溃"变成"第一次跨线程调用就明确报错"。

#### 3.6.6 最终模型（方案 A）

```cangjie
public class LvglRuntime {
    private let taskQueue: BoundedQueue<() -> Unit>   // 见 §3.9
    private var running: Bool = false

    public func start(): Unit {
        lvglcj_init()
        checkCompatibility()
        lvglcj_set_tick_cb()
        lvglcj_set_log_cb(onLvglLog)
        lvglcj_start_thread()               // ★ C 侧 pthread_create
        this.running = true
    }

    public func post(task: () -> Unit): Future<Unit> { ... }   // §3.9

    public func stop(mode: ShutdownMode): Unit { ... }          // §3.9
}
```

---

### 3.7 事件语义完整模型 ★新增 A3

#### 3.7.1 target vs current_target（必须有）

| 概念 | 含义 | FFI |
|---|---|---|
| `target` | **最初**触发事件的对象（冒泡链起点） | `lvglcj_event_get_target` |
| `current_target` | **当前**正在处理该事件的对象（冒泡链中的某一级） | `lvglcj_event_get_current_target` ★新增 |

父容器收到冒泡上来的事件时，`target` 是子按钮，`current_target` 是父容器。绑定层必须同时暴露两者。

#### 3.7.2 冒泡与 trickle

- **冒泡（bubbling）**：子对象事件向父级传播。需 `lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE)`
- **trickle**：v9 新增，事件从父向子"下渗"
- 停止传播：`lv_event_stop_bubbling(e)` / `lv_event_stop_trickling(e)`

```c
// 新增 FFI
int32_t lvglcj_event_stop_bubbling(int64_t evh);
int32_t lvglcj_event_stop_trickling(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);
int32_t lvglcj_obj_add_flag(int64_t obj, int32_t flag);   // 已存在，用于 EVENT_BUBBLE
```

#### 3.7.3 事件码表（完整）

| 类别 | 事件码 | MVP |
|---|---|---|
| 输入 | `PRESSED` `PRESSING` `PRESS_LOST` `RELEASED` `CLICKED` `LONG_PRESSED` `LONG_PRESSED_REPEAT` | ✅ |
| 值变更 | `VALUE_CHANGED` `READY` `CANCEL` | ✅ |
| 生命周期 | `DELETE` `CHILD_CHANGED` `SCREEN_LOADED` `SCREEN_UNLOADED` | ✅（`DELETE` 内部用） |
| 焦点 | `FOCUSED` `DEFOCUSED` | ✅ |
| 布局/尺寸 | `SIZE_CHANGED` `STYLE_CHANGED` `LAYOUT_CHANGED` `REFR_EXT_DRAW_SIZE` | 部分 |
| 滚动 | `SCROLL_BEGIN` `SCROLL_END` `SCROLL` | P1 |
| 绘制 | `DRAW_MAIN_BEGIN` `DRAW_MAIN` `DRAW_MAIN_END` `DRAW_POST_BEGIN` `DRAW_POST` `DRAW_POST_END` | ⚠️ 见 §3.11 |
| 特殊 | `REFRESH` `ALL` `PREPROCESS` `HIT_TEST` `COVER_CHECK` | P2 |

#### 3.7.4 事件分发顺序

LVGL 按**注册顺序**调用同一对象的多个回调。绑定层保证：
- 内部 `DELETE` 钩子**总是第一个注册**（保证最先执行，先注销闭包）
- 用户回调按用户注册顺序

---

### 3.8 回调重入规则 ★新增 A7

#### 3.8.1 重入场景矩阵

| 场景 | 是否允许 | 机制 |
|---|---|---|
| 回调中修改**其他**对象属性 | ✅ 允许 | — |
| 回调中**创建**对象 | ✅ 允许 | — |
| 回调中删除**其他**对象 | ⚠️ 允许但走**延迟删除** | 见下 |
| 回调中删除**自身** | ⚠️ 允许但走**延迟删除** | 见下 |
| 回调中**注销**自己的回调 | ⚠️ 允许但走**延迟注销** | 见下 |
| 回调中触发新事件（`lv_obj_send_event`） | ⚠️ 限制深度 ≤ 8 | 超限报错 |
| 回调中 `post` 并**同步等待** | ❌ **禁止** | 死锁，运行时检测 |
| 回调中抛异常 | ❌ 禁止跨越 C 边界 | trampoline 捕获（§3.2.2） |

#### 3.8.2 延迟删除队列（核心机制）

**问题**：回调执行中直接 `lv_obj_delete`，若该对象正在事件分发链上，会导致迭代器失效/use-after-free。

**方案**：执行上下文标记 + 延迟队列

```c
// 进入 trampoline 时递增，退出时递减
static _Atomic int g_callback_depth = 0;
static lvglcj_deferred_list_t g_deferred;

int32_t lvglcj_obj_delete(int64_t h) {
    if (atomic_load(&g_callback_depth) > 0) {
        // ★ 回调中：不立即删除，入队
        lvglcj_deferred_push(DEFER_DELETE, h);
        lvglcj_handle_mark_pending_delete(h);   // 标记，业务 API 开始拒绝
        return LVGLCJ_OK_DEFERRED;
    }
    return lvglcj_obj_delete_now(h);
}

// LVGL 主循环每轮 drain 之后、lv_timer_handler 之前执行
void lvglcj_drain_deferred(void) {
    lvglcj_deferred_list_t d = lvglcj_deferred_swap(&g_deferred);
    for (each item in d) {
        switch (item.op) {
            case DEFER_DELETE:  lvglcj_obj_delete_now(item.h); break;
            case DEFER_UNREG:   lvglcj_unregister_closure_now(item.cid); break;
        }
    }
}
```

**给仓颉用户的语义**：回调中调 `obj.close()` 不会立即生效，但对象立刻进入"待删除"状态（后续 API 调用抛异常），实际删除最迟在下一帧完成。**这个语义必须写进用户文档。**

#### 3.8.3 回调期句柄锁定

回调执行期间，被回调引用的对象句柄标记为 `IN_CALLBACK`，此时：
- `close()` → 转延迟删除
- 其他业务 API → 允许（LVGL 本身允许回调中改属性）

#### 3.8.4 死锁检测

`post` 的同步等待版本（`postAndWait`）检测当前是否在 LVGL 线程：

```c
if (lvglcj_os_thread_self() == g_lvgl_thread_id) {
    return LVGLCJ_ERR_DEADLOCK_RISK;    // ★ 明确报错，不真锁
}
```

---

### 3.9 任务队列语义 ★新增 A8

#### 3.9.1 完整定义

| 属性 | 定义 |
|---|---|
| 容量 | **有界**，默认 1024；可配置 |
| 满时策略 | `post` 阻塞等待（默认）／抛 `QueueFull`／丢弃最旧（可选） |
| 返回值 | `Future<Unit>`（可 `await`，也可忽略） |
| 异常传播 | 任务抛异常 → 存入 `Future`，由调用方 `await` 时重新抛出；未 await 则走全局错误回调 |
| 关闭策略 | `Drain`（执行完剩余）／`Discard`（丢弃）／`DrainWithTimeout(ms)` |
| 线程断言 | 非 LVGL 线程直接调 API → `ERR_WRONG_THREAD`（§3.6.5） |

#### 3.9.2 接口

```cangjie
public class LvglRuntime {
    // 异步投递，返回 Future
    public func post(task: () -> Unit): Future<Unit>

    // 同步投递并等待（禁止在 LVGL 线程内调用，见 §3.8.4）
    public func postAndWait(task: () -> Unit): Unit

    // 带返回值
    public func postAndGet<T>(task: () -> T): Future<T>

    public func stop(mode: ShutdownMode): Unit
}

public enum ShutdownMode { Drain | Discard | DrainWithTimeout(Int64) }
```

#### 3.9.3 关闭时未执行任务

- `Drain`：执行完队列中所有任务再退出（可能耗时，需日志提示）
- `Discard`：清空队列，未执行任务触发 `TaskDiscarded` 回调（用户可记录）
- `DrainWithTimeout(n)`：n 毫秒内尽量执行，超时后按 `Discard` 处理

---

### 3.10 动画 API ★新增 A4

#### 3.10.1 LVGL v9 动画模型

```c
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, obj);                    // 目标对象
lv_anim_set_values(&a, 0, 100);              // 起止值
lv_anim_set_time(&a, 300);                   // 时长 ms
lv_anim_set_exec_cb(&a, exec_cb);            // ★ 回调：void (*)(void*, int32_t)
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
lv_anim_start(&a);
```

#### 3.10.2 FFI 设计

```c
int64_t lvglcj_anim_create(void);                       // 返回 anim 句柄
int32_t lvglcj_anim_set_var(int64_t anim, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t anim, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t anim, int32_t cid);     // ★ trampoline T3
int32_t lvglcj_anim_set_path(int64_t anim, int32_t path);       // 枚举：linear/ease_in/out/in_out/overshoot/bounce/step
int32_t lvglcj_anim_set_repeat(int64_t anim, int32_t cnt);      // -1 或 LV_ANIM_REPEAT_INFINITE
int32_t lvglcj_anim_set_playback(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t anim, int32_t cid);  // ★ 对象被删时清理闭包
int32_t lvglcj_anim_start(int64_t anim);
int32_t lvglcj_anim_delete(int64_t anim);
int32_t lvglcj_obj_delete_anim(int64_t obj);     // 删除对象上所有动画
```

#### 3.10.3 exec_cb trampoline（T3）

```c
static void lvglcj_anim_exec_trampoline(void *var, int32_t value) {
    int32_t cid = lvglcj_ptr_to_cid(var);      // ★ var 即 closure_id
    int rc = lvglcj_call_closure2(cid, value);
    if (rc != 0) lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, cid, __func__);
}
```

⚠️ **LVGL v9 的 `exec_cb` 没有独立的 user_data 参数**，只有 `var`。因此把 `var` 直接用作 closure_id（不指向真实对象）。对象引用由仓颉闭包自己持有。这是 v9 动画 API 的约束，需要在文档说明。

#### 3.10.4 生命周期

- 动画挂在对象上，对象删除时 LVGL 自动停止动画
- **仓颉侧闭包必须清理**：靠 `lv_anim_set_deleted_cb` 注册清理回调
- 手动 `lvglcj_obj_delete_anim(obj)` 也可清理

#### 3.10.5 仓颉 DSL

```cangjie
LvAnim {
    target(btn)
    values(0, 100)
    duration(300)
    path(AnimPath.EaseOut)
    repeat(AnimRepeat.Infinite)
    playback(150)
    onExec { v => btn.setWidth(v) }
}.start()
```

---

### 3.11 自定义绘制策略 ★新增 A5

#### 3.11.1 决策：`lv_draw_*` 底层 API 为非目标

LVGL v9 的绘制架构是 **draw unit 模型**（`lv_draw_unit_t` + `evaluate_cb`/`dispatch_cb`），要 FFI 暴露需要桥接整个 draw 上下文、图层、任务分发链，**复杂度与风险远超收益**。

**明确写入非目标（§1.4）**：不暴露 `lv_draw_*`、`lv_draw_unit_*`、`LV_EVENT_DRAW_*`。

#### 3.11.2 替代路径：Canvas 控件

LVGL 提供 `lv_canvas`，自带缓冲并提供 `lv_canvas_draw_*` 系列（点/线/矩形/弧/多边形/图像），足以覆盖 HMI 常见的自定义绘制需求（仪表盘、波形、自定义图表）。

```c
int64_t lvglcj_canvas_create(int64_t parent);
int32_t lvglcj_canvas_set_buffer(int64_t canvas, int32_t w, int32_t h);  // C 侧分配
int32_t lvglcj_canvas_set_palette(int64_t canvas, int32_t idx, uint32_t color);
int32_t lvglcj_canvas_draw_point(int64_t canvas, int32_t x, int32_t y, uint32_t color);
int32_t lvglcj_canvas_draw_line(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_rect(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_arc(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_polygon(int64_t canvas, ...);
int32_t lvglcj_canvas_fill_bg(int64_t canvas, uint32_t color);
```

缓冲同样在 C 侧分配（§3.4 原则一致）。

#### 3.11.3 若确需 `LV_EVENT_DRAW_*`

列入 **P3 专项评估**，需要单独设计 draw 上下文的不透明句柄 + 受限绘制指令集。不进 MVP。

---

### 3.12 错误模型 ★修订 A11

#### 3.12.1 错误码表（完整，见附录 B）

C 侧统一 `int32_t` 返回值，0 = 成功，负数 = 错误。仓颉侧映射为 `LvglException`。

#### 3.12.2 错误上下文

错误不只是码，必须带上下文：

```c
typedef struct {
    int32_t  code;
    int64_t  handle;        // 相关句柄（若有）
    int32_t  closure_id;    // 相关闭包（若是回调错误）
    const char *func;       // C 侧函数名
    const char *msg;        // 可读信息
} lvglcj_error_t;

int32_t lvglcj_last_error(lvglcj_error_t *out);   // 取最后一次错误
void    lvglcj_clear_error(void);
```

仓颉侧：

```cangjie
public class LvglException <: Exception {
    public let code: LvglError
    public let handle: Option<Int64>
    public let nativeFunc: String
}
```

#### 3.12.3 全局错误回调

回调内异常被 trampoline 吞掉后，用户需要感知：

```cangjie
LvglRuntime.onError { err =>
    println("[lvgl4cj] ${err.code} @ ${err.nativeFunc}, handle=${err.handle}")
}
```

**默认行为**：未注册时打印到 stderr，不静默。

---

### 3.13 文件系统 / 图片 / 字体 / Group ★新增

#### 3.13.1 文件系统（`lv_fs`）

LVGL 的 `lv_fs_drv_t` 需要注册 open/read/seek/tell/close 回调。**设计决策：MVP 不暴露自定义 fs 驱动注册（回调多、收益低），改为在 C 侧预置一个 "POSIX" 驱动**（`lv_fs_posix` 或自写），覆盖图片/字体从文件路径加载的需求。

```c
int32_t lvglcj_fs_init_posix(const char *root);   // 初始化，root 为沙箱根目录
```

自定义 fs 驱动列为 P3，需 5 个 trampoline（T6）。

#### 3.13.2 图片解码

| 格式 | 方案 | 许可 |
|---|---|---|
| 内置解码器 | LVGL 自带（编译进 `lv_conf.h` 的 `LV_USE_...`） | MIT |
| PNG | `lv_libpng`（依赖 libpng）或 LVGL 内置 | libpng: zlib-like |
| JPG | `lv_libjpeg-turbo` | IJG / BSD |
| 位图 | `lv_image_set_src(obj, "A:path")` | — |

MVP 启用内置解码器 + PNG（若需要）。**许可需入 SBOM（§14.2）**。

#### 3.13.3 字体

- 内置字体：编译进 `lv_conf.h`（`LV_FONT_DEFAULT`）
- 外部字体：`lv_font_load()` 从 fs 加载 —— 需 C 侧持有字体对象，句柄暴露
- 多语言/中文：**字体体积是嵌入式主要成本**，需提供字体子集化工具说明

```c
int64_t lvglcj_font_load(const char *path);
int32_t lvglcj_obj_set_style_text_font(int64_t obj, int64_t font, int32_t selector);
```

#### 3.13.4 Group（键盘/编码器导航）

v0.1 有 indev 但无 group，导致键盘导航不完整。补全：

```c
int64_t lvglcj_group_create(void);
int32_t lvglcj_group_delete(int64_t group);
int32_t lvglcj_group_add_obj(int64_t group, int64_t obj);
int32_t lvglcj_group_remove_obj(int64_t obj);
int32_t lvglcj_group_focus_obj(int64_t obj);
int64_t lvglcj_group_get_focused(int64_t group);
int32_t lvglcj_group_set_default(int64_t group);
int32_t lvglcj_indev_set_group(int64_t indev, int64_t group);   // ★ 绑定
```

仓颉侧：`LvGroup`，`indev.bindGroup(group)`。
---

## 四、目录结构（v0.2 更新）

```
lvgl4cj/
├── README.md
├── LICENSE                        # Apache-2.0
├── NOTICE
├── CHANGELOG.md
├── CONTRIBUTING.md
├── cjpm.toml
├── src/                           # ── L1 仓颉安全 API 层 ──
│   ├── lvgl.cj                    # 顶层门面：init / version / 常量
│   ├── runtime.cj                 # LvglRuntime：OS 线程、主循环、任务队列
│   ├── handle.cj                  # 句柄封装与 Resource 语义（四态）
│   ├── error.cj                   # LvglException、错误码、全局错误回调
│   ├── queue.cj                   # BoundedQueue + Future
│   ├── core/
│   │   ├── object.cj              # LvObject
│   │   ├── display.cj             # LvDisplay（缓冲/格式/旋转/flush）
│   │   ├── indev.cj               # LvIndev
│   │   ├── group.cj               # ★ LvGroup（焦点导航）
│   │   ├── event.cj               # LvEvent + 事件码枚举 + 冒泡控制
│   │   ├── style.cj               # LvStyle + 样式 DSL
│   │   ├── timer.cj               # LvTimer
│   │   ├── anim.cj                # ★ LvAnim + 动画 DSL
│   │   ├── canvas.cj              # ★ LvCanvas（自定义绘制替代路径）
│   │   ├── fs.cj                  # ★ LvFs
│   │   ├── font.cj                # ★ LvFont
│   │   └── theme.cj
│   ├── widgets/
│   │   ├── label.cj  button.cj  image.cj  slider.cj  switch.cj
│   │   ├── checkbox.cj  dropdown.cj  roller.cj  textarea.cj
│   │   ├── bar.cj  arc.cj  led.cj  line.cj  spinner.cj
│   │   ├── chart.cj  table.cj  tabview.cj  menu.cj  msgbox.cj
│   │   ├── keyboard.cj  calendar.cj  scale.cj  animimg.cj  span.cj
│   │   └── imgbtn.cj  win.cj  tileview.cj
│   ├── layout/
│   │   ├── flex.cj
│   │   └── grid.cj
│   ├── debug/                     # ★ 可观测性
│   │   ├── dump.cj                # 对象树 dump
│   │   ├── screenshot.cj          # 截图
│   │   └── perf.cj                # 性能计数器 / 内存监控
│   ├── ffi/
│   │   └── bridge.cj              # ★ 所有 foreign 声明集中在此
│   └── generated/
│       └── conf_const.cj          # ★ 自动生成，勿手改
├── native/                        # ── L2 C 桥接层 ──
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── lvglcj_bridge.h
│   │   ├── lvglcj_error.h
│   │   ├── handle_table.h
│   │   └── callback.h
│   ├── src/
│   │   ├── handle_table.c         # 四态状态机
│   │   ├── lifecycle.c            # LV_EVENT_DELETE 钩子 + 级联失效
│   │   ├── callback.c             # ★ trampoline T1-T6 + 闭包表
│   │   ├── deferred.c             # ★ 延迟删除/注销队列
│   │   ├── thread.c               # ★ OS 线程 + 任务队列 + 线程断言
│   │   ├── display.c              # 缓冲分配（stride 对齐）
│   │   ├── indev.c
│   │   ├── group.c                # ★
│   │   ├── obj.c
│   │   ├── style.c
│   │   ├── event.c                # 冒泡/current_target
│   │   ├── timer.c
│   │   ├── anim.c                 # ★
│   │   ├── canvas.c               # ★
│   │   ├── fs.c                   # ★
│   │   ├── font.c                 # ★
│   │   ├── conf_probe.c
│   │   ├── debug.c                # ★ dump / 截图 / 性能
│   │   └── log.c
│   └── probe/
│       └── probe_conf.c           # ★ 输出 conf JSON
├── backend/
│   ├── sdl2/                      # MVP
│   ├── fbdev/
│   ├── drm/
│   └── oh/                        # 二期
├── third_party/
│   └── lvgl/                      # 锁定版本 + lv_conf.h + 补丁
├── examples/
│   ├── hello_cj/
│   ├── widgets_demo/
│   ├── anim_demo/                # ★
│   ├── canvas_demo/              # ★
│   └── hmi_panel/
├── test/
│   ├── unit/
│   │   ├── handle_test.cj         # 四态 + 幂等
│   │   ├── callback_test.cj       # 注册/注销/重入
│   │   ├── event_test.cj          # 冒泡/current_target
│   │   ├── thread_test.cj         # ★ 亲和性探针
│   │   └── anim_test.cj           # ★
│   ├── ffi_contract_test.cj       # 结构体布局/偏移
│   ├── soak_test.cj               # 24h
│   └── native/
│       ├── CMakeLists.txt
│       └── test_handle_table.c    # ★ C 侧单测
├── scripts/
│   ├── build_native.sh / .ps1
│   ├── gen_conf_const.py          # ★
│   ├── gen_bindings.py
│   └── run_debug.ps1
├── docs/
│   ├── adr/                       # ★ 架构决策记录（§16）
│   ├── api/                       # ★ API 参考
│   └── tutorials/                 # ★
└── .github/
    ├── ISSUE_TEMPLATE/
    ├── PULL_REQUEST_TEMPLATE.md
    └── workflows/ci.yml
```

---

## 五、C ABI 接口清单

**约定**：`lvglcj_<子系统>_<动作>`，全 `extern "C"`，返回 `int32_t` 错误码（0 成功）。默认符号可见性 `-fvisibility=hidden`，导出标记 `LVGLCJ_API`。

### 5.1 运行时与线程

```c
int32_t lvglcj_init(void);
int32_t lvglcj_deinit(void);
void    lvglcj_set_tick_cb(void);
int32_t lvglcj_set_log_cb(int32_t cid);
const char *lvglcj_version(void);
uint32_t lvglcj_conf_hash(void);

// ★ 线程（§3.6）
int32_t lvglcj_start_thread(void);
int32_t lvglcj_stop_thread(int32_t shutdown_mode);
int64_t lvglcj_current_os_tid(void);          // 探针用
int32_t lvglcj_is_lvgl_thread(void);
int32_t lvglcj_post_task(int64_t task_id);    // 仓颉投递
void    lvglcj_drain_deferred(void);          // 延迟队列（§3.8）
```

### 5.2 句柄（四态）

```c
int64_t lvglcj_handle_of(void *ptr);
int32_t lvglcj_handle_alive(int64_t h);
int32_t lvglcj_handle_state(int64_t h);       // ★ 返回四态枚举
void    lvglcj_handle_invalidate(int64_t h);
void    lvglcj_handle_release(int64_t h);
int32_t lvglcj_handle_pending_delete(int64_t h);   // ★ 延迟删除标记
```

### 5.3 Display（★ 补齐 A9）

```c
int64_t lvglcj_display_create(int32_t w, int32_t h, int32_t color_format,
                              int32_t buf_mode, int32_t buf_lines);
// 缓冲由 C 侧按 lv_draw_buf_width_to_stride 分配

int32_t lvglcj_display_set_color_format(int64_t disp, int32_t fmt);
int32_t lvglcj_display_set_flush_cb(int64_t disp, int32_t cid);
int32_t lvglcj_display_set_flush_wait_cb(int64_t disp, int32_t cid);   // ★
int32_t lvglcj_display_flush_ready(int64_t disp);
int32_t lvglcj_display_flush_is_last(int64_t disp);                    // ★
int32_t lvglcj_display_set_rotation(int64_t disp, int32_t rot);        // ★
int32_t lvglcj_display_set_resolution(int64_t disp, int32_t w, int32_t h);
int32_t lvglcj_display_get_color_format(int64_t disp);                 // ★
int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid);  // ★ 分辨率变更
int32_t lvglcj_display_delete(int64_t disp);   // ★ 先删 display 再 free 缓冲
```

### 5.4 InDev + Group（★ 新增）

```c
int64_t lvglcj_indev_create(int32_t type);    // POINTER/KEYPAD/ENCODER/BUTTON
int32_t lvglcj_indev_set_read_cb(int64_t indev, int32_t cid);
int32_t lvglcj_indev_set_display(int64_t indev, int64_t disp);
int32_t lvglcj_indev_set_group(int64_t indev, int64_t group);   // ★
int32_t lvglcj_indev_delete(int64_t indev);

int64_t lvglcj_group_create(void);            // ★
int32_t lvglcj_group_delete(int64_t g);       // ★
int32_t lvglcj_group_add_obj(int64_t g, int64_t obj);
int32_t lvglcj_group_remove_obj(int64_t obj);
int32_t lvglcj_group_focus_obj(int64_t obj);
int64_t lvglcj_group_get_focused(int64_t g);
int32_t lvglcj_group_focus_next(int64_t g);
int32_t lvglcj_group_focus_prev(int64_t g);
int32_t lvglcj_group_set_default(int64_t g);
```

### 5.5 对象

```c
int64_t lvglcj_obj_create(int64_t parent);      // 自动挂 DELETE 钩子
int32_t lvglcj_obj_delete(int64_t obj);         // ★ 回调中自动转延迟
int32_t lvglcj_obj_clean(int64_t obj);          // ★ 清子对象但保留自身
int32_t lvglcj_obj_set_pos / set_size / set_parent
int32_t lvglcj_obj_add_flag / remove_flag
int32_t lvglcj_obj_add_state / remove_state
int64_t lvglcj_screen_active(void);
int32_t lvglcj_screen_load(int64_t scr);
int32_t lvglcj_screen_load_anim(int64_t scr, int32_t anim_type,
                                int32_t time, int32_t delay, int32_t auto_del);  // ★
int64_t lvglcj_obj_get_child(int64_t obj, int32_t idx);
int32_t lvglcj_obj_get_child_count(int64_t obj);
```

### 5.6 事件（★ 补齐 A3）

```c
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int32_t cid);
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t ev_dsc);
int32_t lvglcj_obj_remove_event_by_cid(int64_t obj, int32_t cid);   // ★
int32_t lvglcj_obj_send_event(int64_t obj, int32_t code, int64_t param);

// 事件读取
int32_t lvglcj_event_get_code(int64_t evh);
int64_t lvglcj_event_get_target(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);   // ★
int64_t lvglcj_event_get_user_data(int64_t evh);
int32_t lvglcj_event_stop_bubbling(int64_t evh);        // ★
int32_t lvglcj_event_stop_trickling(int64_t evh);       // ★

// Display / InDev 事件
int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid);
int32_t lvglcj_indev_add_event(int64_t indev, int32_t code, int32_t cid);
```

### 5.7 样式（★ 扩充）

v9 样式 API 已去掉 state 参数，改用 **selector**。以下均带 `int32_t selector` 后缀参数。

| 类别 | 属性 |
|---|---|
| 背景 | `bg_color` `bg_opa` `bg_grad_color` `bg_grad_dir` `bg_grad_stop` `bg_image_src` `bg_image_opa` `bg_image_recolor` `bg_image_tiled` |
| 边框 | `border_width` `border_color` `border_opa` `border_side` `border_post` |
| 圆角 | `radius` `clip_corner` |
| 内外边距 | `pad_top/bottom/left/right/row/column` `pad_all`（便捷） |
| 尺寸 | `width` `height` `min_width` `max_width` `min_height` `max_height` `length` |
| 文本 | `text_color` `text_opa` `text_font` `text_letter_space` `text_line_space` `text_decor` `text_align` ★ |
| 阴影 | `shadow_width` `shadow_color` `shadow_opa` `shadow_offset_x/y` `shadow_spread` ★ |
| 轮廓 | `outline_width` `outline_color` `outline_opa` `outline_pad` ★ |
| 变换 | `translate_x/y` `scale_x/y` `rotate` `transform_pivot_x/y` ★ |
| 图片 | `image_opa` `image_recolor` `image_recolor_opa` |
| 线条 | `line_width` `line_dash_width` `line_dash_gap` `line_rounded` |
| 弧形 | `arc_width` `arc_rounded` `arc_color` `arc_opa` `arc_image_src` |
| 动画 | `anim` `anim_time` `anim_speed` `transition` ★ |
| 混合 | `blend_mode` ★ |
| 布局 | `layout` `base_dir` |
| 其他 | `opa` `color_filter_opa` `recolor` `bitmap_mask_src` `rotary_sensitivity` |

```c
int64_t lvglcj_style_create(void);
int32_t lvglcj_style_delete(int64_t style);
int32_t lvglcj_style_set_<prop>(int64_t style, <type> value);
int32_t lvglcj_obj_add_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style_all(int64_t obj);
int64_t lvglcj_obj_get_style_<prop>(int64_t obj, int32_t part);   // ★ 读取
```

`transition` 需 `lv_style_transition_dsc_t`，MVP 提供简化版（时长 + 路径 + 属性列表）。

### 5.8 Timer

```c
int64_t lvglcj_timer_create(int32_t cid, int32_t period_ms);
int32_t lvglcj_timer_delete(int64_t timer);
int32_t lvglcj_timer_pause(int64_t timer);
int32_t lvglcj_timer_resume(int64_t timer);
int32_t lvglcj_timer_set_period(int64_t timer, int32_t ms);
int32_t lvglcj_timer_ready(int64_t timer);
int32_t lvglcj_timer_handler(void);        // 包装 lv_timer_handler
```

### 5.9 动画（★ 新增，见 §3.10）

```c
int64_t lvglcj_anim_create(void);
int32_t lvglcj_anim_set_var(int64_t a, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t a, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t a, int32_t cid);      // trampoline T3
int32_t lvglcj_anim_set_path(int64_t a, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t a, int32_t cnt);
int32_t lvglcj_anim_set_playback(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t a, int32_t cid);   // ★ 闭包清理
int32_t lvglcj_anim_set_start_cb / ready_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_start(int64_t a);
int32_t lvglcj_anim_delete(int64_t a);
int32_t lvglcj_obj_delete_anim(int64_t obj);                  // ★
int32_t lvglcj_anim_count_running(void);

// 路径枚举
typedef enum { LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT,
               OVERSHOOT, BOUNCE, STEP } lvglcj_anim_path_t;
```

### 5.10 文件系统 / 图片 / 字体（★ 新增）

```c
int32_t lvglcj_fs_init_posix(const char *root);
int64_t lvglcj_font_load(const char *path);
int32_t lvglcj_font_delete(int64_t font);
int32_t lvglcj_obj_set_style_text_font(int64_t obj, int64_t font, int32_t sel);
int32_t lvglcj_image_set_src(int64_t img, const char *path);
```

### 5.11 调试 / 可观测性（★ 新增）

```c
// 对象树 dump（返回 JSON 字符串，仓颉侧解析）
const char *lvglcj_debug_dump_tree(int64_t root, int32_t max_depth);
void    lvglcj_debug_free_str(const char *s);

// 内存监控
int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag,
                           uint32_t *max_used, uint32_t *free_size);

// 性能计数器
typedef struct {
    uint32_t fps;
    uint32_t cpu_percent;        // LVGL 内部统计（需开 LV_USE_PROFILER）
    uint32_t refr_time_ms;
    uint32_t draw_time_ms;
    uint32_t obj_count;
} lvglcj_perf_t;
int32_t lvglcj_perf_sample(lvglcj_perf_t *out);

// 句柄表诊断
int32_t lvglcj_handle_count(int32_t state);    // ★ 按状态统计，泄漏检测
```

---

## 六、仓颉 API 设计

### 6.1 最小示例（目标形态）

```cangjie
import lvgl4cj.*
import lvgl4cj.core.*
import lvgl4cj.widgets.*

main() {
    let runtime = LvglRuntime()
    runtime.start()

    // SDL2 后端
    let disp = Sdl2Backend.createDisplay(800, 480, ColorFormat.RGB565)
    let indev = Sdl2Backend.createPointer(disp)

    let scr = LvScreen.active()
    let btn = LvButton.create(scr)
    btn.setSize(120, 50)
    btn.align(Align.Center)

    let label = LvLabel.create(btn)
    label.setText("点我")

    var count = 0
    btn.on(Event.Clicked) { e =>
        count += 1
        label.setText("点了 ${count} 次")
    }

    runtime.runForever()
}
```

### 6.2 样式 DSL（扩充属性）

```cangjie
let cardStyle = LvStyle {
    bgColor(0xFF2A2D3E)
    bgOpa(255)
    bgGradColor(0xFF3A3D4E)
    bgGradDir(GradDir.Vertical)
    radius(12)
    padAll(16)
    borderWidth(0)
    shadowWidth(12)              // ★
    shadowColor(0x000000)
    shadowOpa(120)
    textFont(theme.fontBody)     // ★
    textColor(0xFFFFFF)
}

card.apply(cardStyle, Part.Main)
card.apply(pressedStyle, Part.Main | State.Pressed)
card.apply(disabledStyle, Part.Main | State.Disabled)

// 过渡（★）
card.applyTransition(Transition {
    duration(200)
    path(AnimPath.EaseOut)
    props(StyleProp.BgColor, StyleProp.Radius)
}, Part.Main)
```

### 6.3 事件 DSL（含冒泡）

```cangjie
// 基础
let h = btn.on(Event.Clicked) { e =>
    println("clicked")
}
h.remove()          // ★ 显式注销

// 冒泡：父容器接收子对象事件
container.addFlag(ObjFlag.EventBubble)
container.on(Event.Clicked) { e =>
    let origin = e.target          // ★ 最初触发者（子按钮）
    let current = e.currentTarget  // ★ 当前处理者（container）
    println("来自 ${origin} 的事件冒泡到 ${current}")
}

// 停止冒泡
container.on(Event.Clicked) { e =>
    if (shouldStop(e)) { e.stopBubbling() }   // ★
}
```

### 6.4 动画 DSL（★ 新增）

```cangjie
// 属性动画
LvAnim {
    target(btn)
    values(80, 160)
    duration(300)
    path(AnimPath.EaseOut)
    onExec { v => btn.setWidth(v) }
}.start()

// 循环 + 回放
LvAnim {
    target(led)
    values(0, 255)
    duration(800)
    repeat(Repeat.Infinite)
    playback(400)
    onExec { v => led.setBrightness(v) }
}.start()

// 时间线（多个动画编排，P1）
let tl = LvTimeline()
tl.at(0)   { fadeIn(screen) }
tl.at(300) { slideUp(card) }
tl.start()
```

### 6.5 Canvas（★ 自定义绘制替代路径）

```cangjie
let canvas = LvCanvas.create(parent)
canvas.setBuffer(200, 200)
canvas.fillBg(0xFF1A1D2E)

// 自绘仪表弧
canvas.drawArc(cx, cy, radius, startAngle, endAngle, 0xFF00D9B5)
canvas.drawLine(x1, y1, x2, y2, 0xFFFFFF)
canvas.drawPolygon(points, 0xFFE5484D)
```

### 6.6 错误处理

```cangjie
LvglRuntime.onError { err =>
    println("[lvgl4cj] ${err.code} @ ${err.nativeFunc}, handle=${err.handle}")
}

try {
    obj.setWidth(100)
} catch (e: LvglException) {
    match (e.code) {
        case InvalidHandle => println("对象已被删除")
        case WrongThread   => println("必须在 LVGL 线程调用，请用 runtime.post{}")
        case _ => println(e.message)
    }
}
```

### 6.7 线程使用规范（★ 必须写进用户文档）

```
✅ 正确：
   runtime.post { btn.setWidth(100) }        // 从任意线程投递

✅ 正确（已在 LVGL 线程，如事件回调内）：
   btn.setWidth(100)

❌ 错误：从业务线程直接调用
   btn.setWidth(100)      → 抛 WrongThread

❌ 错误：回调内同步等待
   btn.on(Event.Clicked) { e =>
       runtime.postAndWait { ... }           // → 抛 DeadlockRisk
   }
```

### 6.8 明确非目标的用户可见声明（★）

在 README 与 API 文档显著位置声明：

> **lvgl4cj 不提供**：
> - `lv_draw_*` 底层绘制 API（用 `LvCanvas` 替代）
> - `LV_EVENT_DRAW_*` 绘制事件
> - 自定义 `lv_fs` 驱动注册（用内置 POSIX 驱动）
> - 32 位平台支持
> - MCU / RTOS 支持（见决策门 G1）
---

## 七、API 覆盖清单

### 7.1 控件清单（完整，分批交付）

| 批次 | 控件 | 说明 |
|---|---|---|
| **P0** | `obj`(base) `label` `button` | 最小闭环 |
| **P1** | `slider` `switch` `checkbox` `image` `bar` `arc` `led` `line` `spinner` `dropdown` | 常用交互 |
| **P1** | `chart` `table` `roller` `textarea` `keyboard` | 复合控件 |
| **P2** | `tabview` `menu` `msgbox` `win` `tileview` `scale` `calendar` `animimg` `span` `imgbtn` | 高级容器 |
| **P3** | `3dtexture` `flex/grid 布局对象` 其余 | 按需 |

**验收口径**：P0 三个控件走通链路；P1 达到 18 个控件，覆盖 HMI 90% 场景；P2 补齐到 28 个。

> 注：v9 命名已变（`btn`→`button`、`img`→`image`、`scr`→`screen`、`del`→`delete`）。绑定层一律用 **v9 命名**，不提供 v8 别名，避免两套 API 混淆。

### 7.2 样式属性覆盖

见 §5.7 表，共 **40+ 属性**，分 15 类。P0 覆盖背景/圆角/边距/尺寸/文本 5 类；P1 补齐阴影/轮廓/变换/过渡/图片/线条/弧形。

### 7.3 可观测性 API（★ 新增）

| 能力 | API | 用途 |
|---|---|---|
| 对象树 dump | `LvDebug.dumpTree()` → JSON | 调试布局、泄漏分析 |
| 内存监控 | `LvDebug.memMonitor()` | `lv_mem_monitor` 封装 |
| 性能采样 | `LvDebug.perfSample()` | FPS / CPU / 重绘耗时 |
| 句柄统计 | `LvDebug.handleCount(state)` | **泄漏检测**：稳定后 `ALIVE` 数应收敛 |
| 截图 | `LvDebug.screenshot(path)` | SDL2 后端可用，视觉回归测试 |
| 对象计数 | `LvDebug.objCount()` | 与句柄数对比，检测句柄泄漏 |

**这条特别重要**：`handleCount(ALIVE)` 与 LVGL 内部 `objCount` 长期应保持一致或差值恒定。差值持续增大 = 句柄泄漏。写入 soak test 断言。

### 7.4 API 稳定性标记（★ 新增）

```cangjie
@Experimental      // 可能变更，不保证兼容
public func setRotarySensitivity(v: Int32): Unit

@Deprecated(since: "0.3.0", use: "setColorFormat")   // 弃用
public func setColorDepth(v: Int32): Unit
```

规则：
- 新 API 默认 `@Experimental`，经过一个 minor 版本且无变更 → 转稳定
- 弃用 API 保留至少 2 个 minor 版本，编译期告警
- 所有 `@Experimental` API 在文档站单独列出

---

## 八、后端适配

### 8.1 SDL2 后端详解 ★重写 C2

#### 8.1.1 组件

| SDL 对象 | 用途 |
|---|---|
| `SDL_Window` | 宿主窗口 |
| `SDL_Renderer` | 渲染器（软件或硬件加速） |
| `SDL_Texture` | 像素缓冲载体，`SDL_PIXELFORMAT_RGB565` 等 |
| `SDL_Event` | 输入事件源 |

#### 8.1.2 线程关系（关键）

**SDL 事件循环必须在创建窗口的线程**（多数平台要求主线程）。而 LVGL 主循环在 C 侧 OS 线程。两者**必须分离**：

```
主线程（OS main thread）
  └─ SDL_Init / CreateWindow / CreateRenderer
  └─ while: SDL_PollEvent → 转成 LVGL 输入状态写入共享缓冲
     （或由 LVGL 线程的 read_cb 反过来 Poll）

LVGL OS 线程
  └─ while: drain deferred → drain tasks → lv_timer_handler() → sleep 5ms
     └─ flush_cb: SDL_UpdateTexture + SDL_RenderCopy + SDL_RenderPresent
```

⚠️ **注意**：`SDL_RenderPresent` 若在 LVGL 线程调用，某些平台（macOS/Cocoa）会出问题——**渲染调用必须在主线程**。

**解决方案**：flush_cb 只做 `SDL_UpdateTexture`（写像素），把"提交渲染"作为任务投递回主线程执行。或者用 `SDL_PushEvent` 自定义事件通知主线程渲染。

**这个约束必须在 P0 验证**，是 SDL2 后端最容易踩的坑。

#### 8.1.3 像素格式转换

| `lv_display` 格式 | SDL 纹理格式 |
|---|---|
| `RGB565` | `SDL_PIXELFORMAT_RGB565` |
| `RGB888` | `SDL_PIXELFORMAT_RGB888` |
| `ARGB8888` | `SDL_PIXELFORMAT_ARGB8888` |
| `XRGB8888` | `SDL_PIXELFORMAT_XRGB8888` |

**必须一一对应**，否则花屏。RGB565 还要注意字节序（`SDL_PIXELFORMAT_RGB565` vs `BGR565`）。

#### 8.1.4 输入映射

| SDL 事件 | LVGL indev |
|---|---|
| `SDL_MOUSEMOTION` / `BUTTONDOWN/UP` | pointer：`point.x/y` + `state` |
| `SDL_MOUSEWHEEL` | encoder：`enc_diff` |
| `SDL_KEYDOWN/UP` | keypad：`key` + `state` |
| `SDL_TEXTINPUT` | textarea 输入 |
| `SDL_WINDOWEVENT_CLOSE` | 触发 runtime.stop() |

坐标变换：若窗口尺寸 ≠ display 分辨率（缩放），需按比例换算。旋转也要同步换算。

#### 8.1.5 多窗口

MVP **仅支持单窗口**。多窗口需多个 `lv_display`，且输入焦点归属复杂，列入 P3。

#### 8.1.6 决策：手写 flush/read 而非用 LVGL 内置 SDL 驱动

LVGL v9 内置了 SDL 驱动（`LV_USE_SDL`）。**但 MVP 仍手写 flush/read 回调**，理由：

1. 内置驱动绕过 trampoline，**恰恰跳过了最需要验证的回调链路**
2. 手写能让我们控制线程关系（§8.1.2）
3. 内置驱动的行为在版本间可能变化

**折中**：内置驱动可作为"参考实现"对照，或在 P2 提供 `Sdl2Backend(useBuiltin: true)` 开关用于性能对照。

### 8.2 fbdev / DRM

| 后端 | 要点 |
|---|---|
| fbdev | 打开 `/dev/fb0`，`mmap`，flush 直接 `memcpy` 到 framebuffer；需处理 stride 与像素格式；无 GPU |
| DRM/GBM | 更复杂：`drmModeSetCrtc`、dumb buffer 或 GBM bo、双缓冲 + page flip；ARM64 HMI 主力路径 |

两者都在 LVGL 内置驱动中有参考，**P2 阶段优先复用内置驱动**（此时回调链路已在 SDL2 验证过）。

### 8.3 OpenHarmony 后端可行性分析 ★新增 C3

OH 是仓颉官方明确支持的平台（运行时层面），但**显示/输入链路需单独分析**：

| 环节 | OH 机制 | 约束与风险 |
|---|---|---|
| **窗口** | `NativeWindow` (OH_NativeWindow) | 需通过 NAPI 获取，仓颉能否直接调用 OH NDK？**待确认** |
| **渲染** | `OH_NativeWindow_NativeWindowRequestBuffer` + 写 buffer + `FlushBuffer` | 与 LVGL flush_cb 模型契合（区域刷新 → 提交） |
| **Vsync** | `OH_NativeVSync` | 可用 Vsync 驱动 `lv_timer_handler`，比固定 5ms sleep 更省电 |
| **输入** | `OH_Input` 多模输入 / ArkTS 侧事件 | **最大不确定性**：仓颉侧能否订阅 OH 输入事件？若不能，需 ArkTS 层转发 |
| **NAPI** | OH 原生扩展机制 | 仓颉与 OH NAPI 的互操作能力**需实测** |
| **ArkTS 互操作** | 若仓颉 UI 嵌在 ArkTS 页面内 | 需要仓颉 ⇄ ArkTS 的桥，官方支持程度未知 |

**结论：OH 后端的风险不在 LVGL，在仓颉与 OH 系统服务的互操作能力。**

**建议**：OH 后端立项前，先做**独立探针项目**（不涉及 LVGL），验证三件事：
1. 仓颉能否调用 OH NDK（NativeWindow / NativeVSync）
2. 仓颉能否接收 OH 输入事件
3. 仓颉与 ArkTS 的互操作路径

**这三件事不确定，OH 后端就不该排期。** 列为决策门 G2。

---

## 九、构建系统与工程化 ★新增

### 9.1 cjpm 与 CMake 协作

```
构建顺序（必须串行）：
  1. CMake 构建 LVGL（third_party/lvgl）→ liblvgl.a
  2. CMake 构建桥接层（native）→ liblvgl4cj_bridge.a/.so
  3. 运行 probe_conf → conf.json
  4. python gen_conf_const.py → src/generated/conf_const.cj
  5. cjpm build → 链接 1、2 的产物 → 可执行文件
```

`cjpm.toml` 关键配置：

```toml
[package]
  cjc-version = "1.1.0"
  name = "lvgl4cj"
  version = "0.1.0"
  output-type = "static"          # 库模式；示例单独 executable
  src-dir = "src"

[ffi.c]
  path  = ["native/include"]
  clink = ["lvgl4cj_bridge", "lvgl"]

[target.x86_64-unknown-linux-gnu]
  [target.x86_64-unknown-linux-gnu.bin-dependencies]
    path-option = ["${LVGLCJ_NATIVE_LIB_DIR}"]
```

⚠️ **注意**：cjpm 对 `[ffi.c]` / `clink` 的具体字段支持以 SDK 1.1.0 实际为准。**P0 第一步应照抄 `CJQT6/cjpm.toml`**（那是唯一被验证过的模板，旧版 `[native]/[requirements]` 非标准字段已被移除）。

统一构建脚本：`scripts/build.sh` 串起 CMake → probe → gen → cjpm。

### 9.2 符号可见性与库形态

```cmake
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# 仅导出标记 LVGLCJ_API 的函数
```

| 形态 | 何时用 |
|---|---|
| **静态库**（默认） | 嵌入式部署，无动态链接器；体积更可控 |
| 动态库 | 桌面开发，热重载方便 |

MVP 默认静态，桌面开发可选动态。

### 9.3 交叉编译与平台矩阵 ★新增 C4

| 平台 | 架构 | 工具链 | 用途 | CI |
|---|---|---|---|---|
| Ubuntu 22.04 | x86_64 | GCC 11+ | 主开发 | ✅ |
| Ubuntu 22.04 | ARM64 | aarch64-linux-gnu-gcc | 交叉验证 | ✅ |
| macOS 13+ | arm64 | Clang | 开发验证 | ✅ |
| Windows 11 | x86_64 | MSVC 2022 | 可选 | ⚠️ P2 |
| OpenHarmony | ARM64 | OH SDK | 二期，过 G2 后 | ❌ |

**headless 运行**（无显示环境，CI 必需）：
- 提供 `backend_null`：display 的 flush_cb 为空操作，indev 无输入
- 用于单测、soak test、CI
- 这也是**唯一能在 CI 里跑 LVGL 逻辑测试的方式**

```
CI 矩阵 = {ubuntu-x64, ubuntu-arm64, macos-arm64} × {debug+ASan, release}
```

### 9.4 平台差异清单

| 差异 | 处理 |
|---|---|
| `gettid` vs `pthread_threadid_np` | `thread.c` 内条件编译 |
| SDL2 版本差异 | 固定最低版本 2.0.20+，CMake `find_package` |
| 字节序 | 颜色格式转换处条件处理 |
| 对齐要求 | 一律走 `lv_draw_buf_*` 算子，不硬编码 |

### 9.5 ASan 与仓颉 GC 兼容性 ★新增 C6

**问题**：ASan 会拦截所有内存分配。仓颉 GC 自行管理堆，可能出现：
- ASan 误报仓颉 GC 内部内存为泄漏
- 仓颉 GC 分配的内存未被 ASan 追踪，漏报真实问题
- 两者拦截机制冲突导致崩溃

**隔离策略**：

1. **分层启用**：C 桥接层 + LVGL 用 ASan 编译；仓颉侧不启用。用 `ASAN_OPTIONS` 的 `suppressions` 屏蔽仓颉运行时符号
2. **suppression 文件**：`scripts/asan_suppressions.txt`，屏蔽 `cangjie*` / `CJ*` 前缀符号
3. **验证 ASan 有效性**：先人为制造一个 C 侧泄漏，确认 ASan 能报出（**验证工具本身可用**）
4. **备选**：若 ASan 与仓颉 GC 根本冲突，退回 `valgrind --tool=memcheck`（慢但隔离性好）或 LVGL 内置内存检查（`LV_USE_MEM_MONITOR` + 断言）

**P0 必须确认 ASan 可用**，否则内存类缺陷无法自动检测，soak test 价值大打折扣。

---

## 十、测试策略与 CI ★修订 C5

### 10.1 测试分层

| 层级 | 框架 | 内容 | CI |
|---|---|---|---|
| C 侧单测 | 自写断言 + CMake CTest | 句柄表四态、状态机转移、延迟队列 | ✅ |
| FFI 契约 | 仓颉单测 | 结构体 offset/padding、枚举值一致性 | ✅ |
| 仓颉单测 | 仓颉测试框架（`@Test`） | 句柄、回调、事件、动画语义 | ✅ |
| 集成测试 | headless 后端 | 建 UI 树 → 模拟输入 → 断言状态 | ✅ |
| 视觉回归 | SDL2 + 截图 diff | P2 | ⚠️ |
| Soak | 24h 长跑 | 内存/句柄/稳定性 | 夜间 |
| 模糊测试 | P2 | 随机 UI 操作序列 | 夜间 |
| 性能基准 | `scripts/bench` | 帧率/延迟/内存 | 夜间 |

### 10.2 FFI 契约测试（最重要）

```cangjie
@Test
func testStructLayout(): Unit {
    // C 侧与仓颉侧分别计算 lv_area_t 的字段偏移，必须相等
    let cOffsets = lvglcj_probe_area_offsets()   // C 侧返回 offsetof 数组
    let cjOffsets = areaOffsetsFromCangjie()     // 仓颉侧结构体布局
    @Assert(cOffsets == cjOffsets)
}
```

**价值**：LVGL 结构体一旦因配置变化，这个测试立刻报警，避免"莫名花屏/崩溃"。

### 10.3 关键测试用例清单

| 用例 | 断言 |
|---|---|
| `handle_test` | 四态转移正确；`close()` 重复调用安全；`release()` 幂等 |
| `callback_test` | 注册→触发→注销；对象删除时闭包自动清理；回调抛异常被吞并记录 |
| `event_test` | `target` ≠ `current_target`（冒泡场景）；`stopBubbling` 生效 |
| `thread_test` | **亲和性探针**（§3.6.4）；跨线程调用抛 `WrongThread` |
| `anim_test` | 动画执行、停止、对象删除时清理 |
| `deferred_test` | 回调中 `close()` 不崩，下一帧生效 |
| `lifecycle_test` | 父删→子句柄失效；`lv_obj_clean` 后父仍存活 |
| `leak_test` | 创建 1 万对象再删除，`handleCount(ALIVE)` 与 `objCount` 差值恒定 |
| `soak_test` | 24h：RSS 收敛、句柄收敛、无崩溃 |

### 10.4 CI 流水线

```yaml
# .github/workflows/ci.yml 摘要
jobs:
  native:
    matrix: [ubuntu-x64, ubuntu-arm64, macos-arm64]
    steps: [cmake build lvgl + bridge, ctest]
  probe:        # 生成 conf_const.cj
  cjpm:
    steps: [cjfmt --check, cjlint, cjpm build, cjpm test]
  asan:         # 仅 ubuntu-x64，夜间
  soak:         # 夜间，24h（可缩为 2h 用于 PR 门禁）
```

**门禁**：`cjfmt` 零 diff、`cjlint` 零告警、单测全绿、ASan 无报告。

### 10.5 模糊测试（P2）

随机生成 UI 操作序列（创建/删除/改属性/触发事件/hard delete），断言不崩溃且句柄无泄漏。这是发现重入缺陷最有效的手段。
---

## 十一、量化验收指标 ★新增 C10

### 11.1 P0 门禁指标（不过不放行）

| 指标 | 门槛 | 测量方式 |
|---|---|---|
| **线程亲和性** | 探针测试中 OS 线程 ID 变化次数 = 0，或已确认走方案 A | §3.6.4 |
| **句柄泄漏** | 1 万次 create/delete 后 `handleCount(ALIVE)` 增量 = 0 | §7.3 |
| **句柄与对象一致** | `handleCount(ALIVE) - objCount` 恒定（不随操作次数增长） | soak |
| **回调异常** | 回调抛异常不导致进程崩溃，错误回调被调用 | 单测 |
| **延迟删除** | 回调中 `close()` 自身不崩溃，下一帧对象确实删除 | 单测 |
| **ASan** | 零报告（且已验证 ASan 工具本身有效） | CI |

### 11.2 P1 性能指标

| 指标 | 门槛 | 测量条件 |
|---|---|---|
| 帧率 | ≥ 30 FPS（800×480，局部刷新） | SDL2，中等复杂度 UI（~50 对象） |
| 帧率（全屏动画） | ≥ 20 FPS | 800×480 全屏过渡 |
| 输入延迟 | ≤ 50 ms（点击到回调触发） | 事件时间戳差 |
| 单次 FFI 调用开销 | ≤ 2 μs | 10 万次 `lv_obj_set_x` 均值 |
| 事件回调延迟 | ≤ 200 μs（C 触发 → 仓颉闭包执行） | 探针计时 |
| 启动时间 | ≤ 500 ms（`lv_init` 到首帧） | 冷启动测量 |

### 11.3 内存指标

| 指标 | 门槛 | 测量 |
|---|---|---|
| RSS 增长 | **24h ≤ 10 MB** | 每小时采样 |
| LVGL 内存池 | 峰值 ≤ 配置值的 80% | `lv_mem_monitor` |
| 句柄表大小 | 稳态后不再增长 | `handleCount` |
| 闭包表大小 | 对象删除后归零 | 诊断接口 |

### 11.4 稳定性指标

| 指标 | 门槛 |
|---|---|
| 24h 连续运行 | 零崩溃、零 ASan 报告、内存收敛 |
| 100 万次事件触发 | 无崩溃、无句柄泄漏 |
| 随机操作模糊测试（2h） | 无崩溃 |

### 11.5 基线记录要求

**所有指标必须在 `docs/benchmarks/` 记录**：日期、平台、LVGL 版本、仓颉 SDK 版本、硬件配置、原始数据。**没有基线，优化无从谈起。**

---

## 十二、MVP 路线（v0.2 更新）

### 阶段 P0：跑通闭环 + 排掉阻塞性不确定项

**第 1 周（阻塞项清零，不写业务代码）**

| # | 任务 | 产出 |
|---|---|---|
| V1 | 仓颉外部 OS 线程能否执行闭包（§3.6.3） | 方案 A/B/C 选型结论 |
| V2 | 仓颉 FFI 回调是否支持捕获（§3.2.3） | 闭包表是否自建 |
| V3 | 亲和性探针测试（§3.6.4） | 实测数据 |
| V4 | ASan 与仓颉 GC 兼容性（§9.5） | 内存检测方案 |
| V5 | cjpm `[ffi.c]` 字段可用性 | 照抄 CJQT6 模板验证 |

⚠️ **这五项不确定，P0 后续工作无法正确设计。必须先做，不许边猜边写。**

**第 2-4 周（闭环）**

| # | 任务 | 验收 |
|---|---|---|
| 1 | `lv_init` / tick / `lv_timer_handler` 主循环 | 稳定刷帧 |
| 2 | SDL2 display flush + pointer indev | 按钮可点，坐标正确，非花屏 |
| 3 | 对象树 + 四态句柄 + 级联失效 | 父删→子失效，无悬空 |
| 4 | 事件回调 + timer 回调 | 注册/注销/重入安全 |
| 5 | 样式最小集 + Flex 布局 | 布局正确 |

**P0 压力测试**：`button → clicked → 删除自己 → 重建`。跑不稳就不许加控件。

### 阶段 P1：可用

- 18 个控件（§7.1）
- 样式补齐到 40+ 属性；动画 API；Group 与键盘导航
- 延迟删除队列、冒泡语义、错误模型
- 四平台 CI + ASan；`examples/widgets_demo`、`anim_demo`
- 量化指标达标（§11.2/11.3）

### 阶段 P2：工程化

- 绑定生成器（setter/getter 自动生成）
- ARM64 Linux 交叉编译 + DRM 后端
- 控件补齐到 28 个；Canvas；fs/字体/图片
- 模糊测试、视觉回归、性能基线入库
- `examples/hmi_panel`

### 阶段 P3：外延（需过决策门）

- OpenHarmony 后端（**先过 G2**，§8.3）
- MCU / RTOS（**先过 G1**，§13）
- `LV_EVENT_DRAW_*` 评估
- 32 位平台

---

## 十三、风险登记册 ★修订 C11

| ID | 风险 | 等级 | 应对 | **Owner** | **触发条件** | **评估时间点** |
|---|---|---|---|---|---|---|
| R1 | **仓颉无法在外部 OS 线程执行闭包** | 高 | 退回泵模式（方案 C） | 架构负责人 | V1 结论为否 | **P0 第 1 周** |
| R2 | 仓颉轻量级线程 M:N 迁移 | 高 | 强制方案 A + 线程断言 | 架构负责人 | 探针 OS TID 变化 > 0 | **P0 第 1 周** |
| R3 | ASan 与仓颉 GC 冲突 | 中 | 用 valgrind / LVGL 内置检查 | 测试负责人 | ASan 误报或崩溃 | **P0 第 1 周** |
| R4 | 回调内异常跨越 C 边界导致崩溃 | 高 | trampoline 捕获 + 全局错误回调 | 桥接层负责人 | 单测失败 | P0 第 3 周 |
| R5 | 绘制缓冲被 GC 移动 → 花屏 | 高 | C 侧对齐分配（§3.4） | 桥接层负责人 | 出现间歇花屏 | P0 第 2 周 |
| R6 | 父删子导致悬空句柄 | 高 | DELETE 钩子级联失效 | 桥接层负责人 | 单测失败 | P0 第 3 周 |
| R7 | 回调中删除自身导致重入崩溃 | 高 | 延迟删除队列（§3.8） | 桥接层负责人 | 压力测试失败 | P0 第 4 周 |
| R8 | `lv_conf` 不匹配导致隐蔽 bug | 中 | 哈希校验（§3.5） | 构建负责人 | 换机器构建失败 | P0 第 2 周 |
| R9 | LVGL 版本升级破坏 API | 中 | 版本锁定 + 独立升级分支 | 维护者 | — | 每季度 |
| R10 | 控件 API 量大手写不完 | 中 | 生成器 + 分批交付 | API 负责人 | 进度落后 2 周 | P1 中期 |
| R11 | **仓颉运行时不适合 MCU** | 高 | 见决策门 G1 | 架构负责人 | 基准测试不达标 | **P2 完成后** |
| R12 | OH 系统服务互操作不通 | 高 | 见决策门 G2；独立探针 | OH 负责人 | 探针失败 | **P2 完成后** |
| R13 | 32 位平台 `void*` 截断 | 中 | MVP 仅 64 位；二期独立句柄表 | 架构负责人 | 出现 32 位需求 | 需求出现时 |
| R14 | LVGL 许可与 SBOM 不合规 | 中 | 许可清单前置审查（§14.2） | 合规负责人 | 入库前审查 | 发布前 |

### 决策门

#### G1：是否进入 MCU / RTOS（P2 后）

| 测量项 | 门槛 |
|---|---|
| 仓颉运行时静态 footprint | ≤ 目标板 Flash 的 50% |
| 堆峰值 | ≤ 目标板 RAM 的 50% |
| **GC 停顿** | **≤ 16 ms**（否则动画卡顿） |
| 启动时间 | 满足产品冷启动要求 |
| 静态链接可行性 | 无包管理环境能否部署 |
| libc / OS 依赖 | 裸机或最小 RTOS 能否运行 |

**任一关键项不达标 → 不做 MCU 版本。** 替代方案：**C 主控渲染 + 仓颉跑业务逻辑**。

⚠️ LVGL 官方最低资源是 16 KB RAM / 64 KB Flash 级，那是 **LVGL 核心**的口径；仓颉运行时带 GC、线程、标准库，完整进程占用**完全不是一个量级**。不能由"LVGL 很轻"推出"仓颉 + LVGL 能上 MCU"。

#### G2：是否启动 OH 后端（P2 后）

三项探针全部通过才排期：
1. 仓颉能调 OH NDK（NativeWindow / NativeVSync）
2. 仓颉能接收 OH 输入事件
3. 仓颉与 ArkTS 互操作路径明确

---

## 十四、许可、治理与发布 ★修订 C7/C8

### 14.1 许可

| 组件 | 许可 | 备注 |
|---|---|---|
| **LVGL 主线** | **MIT**（v8/v9 已改，非 GPLv3） | ⚠️ 必须锁定 commit 并核对 `LICENCE.txt` |
| lvgl4cj 绑定层 | **Apache-2.0** | 与 LVGL 分离 |
| **SDL2** | zlib（2.0.x）/ MIT 或专有（3.x） | ⚠️ 按实际选版本核对 |
| **libpng** | PNG Reference Library (zlib-like) | 若启用 PNG 解码 |
| **libjpeg-turbo** | IJG / BSD-3 / Zlib 三选一 | 若启用 JPG |
| **FreeType** | GPLv2 或 FTL | ⚠️ **GPL 风险**，字体渲染若用 FreeType 需评估 |
| 内置字体 | 多为 SIL OFL / Apache | 逐个核对 |
| ThorVG（矢量） | MIT | 若启用 |

**行动项**：
- `third_party/lvgl` 保留原始 `LICENCE.txt` + 版本哈希
- CI 生成 **SPDX + CycloneDX** 双格式 SBOM
- 中心仓制品 manifest 写明 `license` / `sourceUrl` / `vendorInfo`
- **FreeType 的 GPL 选项需在启用前专项评估**（用 FTL 双许可可规避）

### 14.2 版本与分支

- **语义化版本** `MAJOR.MINOR.PATCH`；`0.x` 阶段 minor 可含破坏性变更
- 分支模型：`main`（稳定）/ `develop`（集成分支）/ `feature/*` / `release/*`
- `CHANGELOG.md` 按 [Keep a Changelog] 格式
- PR 模板：变更描述 / 关联 issue / 测试证据 / 许可影响
- Issue 模板：Bug（含最小复现）/ Feature / FFI 契约问题
- 提交规范：Conventional Commits（`feat:` `fix:` `ffi:` `docs:`）

### 14.3 制品与发布

| 制品 | 职责 |
|---|---|
| `lvgl4cj-core` | 仓颉安全 API、句柄、生命周期 |
| `lvgl4cj-sys` | C ABI、LVGL 版本锁定、native 构建 |
| `lvgl4cj-backend-sdl` | Linux 桌面后端 |
| `lvgl4cj-backend-drm` | ARM64 HMI（P2） |

准入路径：**独立原型 → SIG 孵化 → TPC → 中心仓制品**。每一步的准入标准需以官方最新贡献流程为准（本方案未取得可直接引用的官方规范 URL，属治理建议而非承诺）。

### 14.4 用户文档规划 ★新增 C14

| 类型 | 内容 |
|---|---|
| 快速开始 | 10 分钟跑通 `hello_cj` |
| 教程 | 控件、布局、样式、动画、事件、Canvas |
| **线程指南** | §6.7 规范，独立成章（最容易出错） |
| **生命周期指南** | 句柄四态、延迟删除语义 |
| API 参考 | 由源码注释生成 |
| 迁移指南 | LVGL 版本升级、绑定层版本升级 |
| 故障排查 | 花屏/崩溃/泄漏 的定位手册 |
| **非目标清单** | §6.8 独立成页 |

文档站：MkDocs 或类似，随 CI 发布。

---

## 十五、与现有绑定对比 ★新增 C13

| 绑定 | 语言 | 方案 | 可借鉴 |
|---|---|---|---|
| **lv_binding_rust** | Rust | `bindgen` 生成 sys 层 + 手写安全层 + `embedded-graphics` 风格抽象 | ★★★ **分层方式（sys / safe）与本方案 L1/L2 一致**；其 `NativeStyle` 生命周期处理值得参考 |
| **lv_binding_micropython** | MicroPython | C 模块 + 对象映射 | ★★ 回调转 Python callable 的桥接 |
| **lvgl-js** | JS | JerryScript / QuickJS 绑定 | ★ 轻量 VM 上的对象管理 |
| **LVGL 官方 C++ 绑定** | C++ | RAII 包装 | ★★ 对象生命周期 RAII 思路 |
| **CJQT6** | 仓颉 | 三层 C ABI 桥接 Qt6 | ★★★ **同语言先例**：工程结构、`close()` 语义、枚举 `.value` 约定 |

**三条关键借鉴**：

1. **`sys` / `safe` 分层**（来自 Rust 绑定）—— 与本方案 L1/L2 完全对应，验证了分层必要性
2. **不追求自动生成全量 API**（Rust 绑定也是 bindgen + 大量手写）—— 印证 §7.4 分批策略
3. **同语言的 CJQT6 经验**——`Resource` + `close()`、禁用终结器、回调用顶层 `@C func`（或 Capture 变体），这些约定直接沿用

**一个差异**：Rust 绑定面临的所有权问题（borrow checker vs LVGL 对象树）在仓颉不存在（GC 语言），但换来的是"GC 可能移动/延迟释放"的新问题——这正是 §3.4（缓冲必须在 C 侧）和 §3.1（显式 close）要解决的。

---

## 十六、架构决策记录（ADR 摘要）★新增 C9

完整 ADR 存 `docs/adr/`，格式：背景 / 备选 / 决策 / 后果。

| ID | 决策 | 关键理由 |
|---|---|---|
| **ADR-001** | 用句柄表（自增 ID）而非裸指针或指针地址作句柄 | 地址复用会导致"误判存活"；ID 可携带状态与调试信息 |
| **ADR-002** | 绘制缓冲在 C 侧分配 | GC 可能移动/回收仓颉数组，传给 LVGL 会花屏或段错误 |
| **ADR-003** | 单 LVGL OS 线程 + 任务队列 | LVGL 非线程安全；仓颉线程可能 M:N 迁移，不能依赖仓颉线程语义 |
| **ADR-004** | C 侧 OS 线程跑主循环（方案 A） | 唯一能保证 OS 线程亲和性的方式；依赖待确认项 V1 |
| **ADR-005** | 不暴露 `lv_draw_*`，用 Canvas | v9 draw unit 模型 FFI 成本极高，Canvas 覆盖 HMI 90% 需求 |
| **ADR-006** | 显式 `close()`，禁用终结器 | GC 时机不确定，靠它析构原生对象会导致使用中对象被释放 |
| **ADR-007** | 延迟删除队列 | 回调中同步删除会导致事件链 use-after-free |
| **ADR-008** | `lv_conf` 哈希校验 | 把"莫名花屏/崩溃"前置为"启动一条明确报错" |
| **ADR-009** | MVP 仅 64 位 | 32 位 `void*` 无法承载 int64 句柄/闭包 ID |
| **ADR-010** | 锁定 LVGL v9.x，不提供 v8 别名 | 两套命名并存会长期增加维护与认知成本 |
| **ADR-011** | 静态库为默认形态 | 嵌入式部署无动态链接器；体积可控 |
| **ADR-012** | 手写 SDL2 flush/read 而非用内置驱动 | 内置驱动绕过 trampoline，跳过最需验证的回调链路 |

---

## 十七、第一个可交付物（不变）

**两周目标不是"框架"，是这一张图 + 六个断言：**

```
examples/hello_cj/
  → SDL2 窗口 800×480，深色背景，中间一个按钮
  → 点击按钮，文字从 "点我" 变成 "点了 N 次"
  → 窗口不崩，退出时干净释放
```

**六个断言**（v0.2 增加一条）：

1. `lv_init` → `lv_timer_handler()` 循环稳定跑 10 分钟不崩
2. flush 回调被调用，画面正确（非花屏/非黑屏）
3. 点击命中按钮，事件回调触发，坐标正确
4. 删除父容器后，子对象句柄 `isAlive() == false`
5. 退出时 ASan 报告零泄漏
6. **★ 回调中 `close()` 自身不崩溃，下一帧对象确实被删除**

配套前置：§12 的 V1–V5 五项不确定项结论。

**这六条过了，项目成立；过不了，先修设计，不要往前堆控件。**

---

## 附录 A：术语表

| 术语 | 含义 |
|---|---|
| **L1 / L2 / L3 / L4** | 仓颉 API 层 / C 桥接层 / LVGL 原生层 / 后端层 |
| **句柄（handle）** | `Int64` 自增 ID，间接引用原生对象；非指针地址 |
| **四态** | `UNINIT` / `ALIVE` / `INVALIDATED` / `RELEASED`（§3.1） |
| **trampoline** | 固定 C 函数，作为 LVGL 回调入口，内部转发到仓颉闭包 |
| **closure_id** | 仓颉闭包在 C 侧闭包表中的 `int32` 标识 |
| **延迟删除** | 回调执行期间不立即删除，入队到下一帧执行（§3.8） |
| **selector** | v9 样式机制：`Part | State` 组合，替代 v8 的 state 参数 |
| **flush_cb** | 显示刷新回调，把渲染结果送到屏幕 |
| **read_cb** | 输入设备读取回调 |
| **stride** | 每行像素字节数（含对齐填充），≠ `width × bpp` |
| **OS 线程 / 仓颉线程** | pthread 级真实线程 / 仓颉运行时管理的轻量级线程（M:N） |
| **G1 / G2** | 决策门：MCU 准入 / OH 后端准入 |

## 附录 B：错误码表

| 码 | 名称 | 含义 | 仓颉异常 |
|---|---|---|---|
| 0 | `OK` | 成功 | — |
| -1 | `INVALID_HANDLE` | 句柄失效（对象已删除） | `LvglException(InvalidHandle)` |
| -2 | `NOT_INITIALIZED` | 未调用 `lv_init` | `LvglException(NotInitialized)` |
| -3 | `INVALID_CONFIG` | `lv_conf` 哈希不匹配 | `LvglException(InvalidConfig)` |
| -4 | `CALLBACK_THREW` | 仓颉回调抛异常（已吞掉） | 触发 `onError` |
| -5 | `OUT_OF_MEMORY` | LVGL 池耗尽或 malloc 失败 | `LvglException(OutOfMemory)` |
| -6 | `WRONG_THREAD` | 跨线程调用 LVGL API | `LvglException(WrongThread)` |
| -7 | `DEADLOCK_RISK` | 回调内同步等待 | `LvglException(DeadlockRisk)` |
| -8 | `QUEUE_FULL` | 任务队列满 | `LvglException(QueueFull)` |
| -9 | `BACKEND_FAILURE` | 后端（SDL/DRM）失败 | `LvglException(BackendFailure)` |
| -10 | `INVALID_ARGUMENT` | 参数非法 | `LvglException(InvalidArgument)` |
| -11 | `CLOSURE_EXHAUSTED` | 闭包 ID 耗尽 | `LvglException(ClosureExhausted)` |
| -12 | `PENDING_DELETE` | 对象处于待删除状态 | `LvglException(PendingDelete)` |
| -13 | `NOT_SUPPORTED` | 该 API 在当前构建配置下不可用 | `LvglException(NotSupported)` |
| -14 | `VERSION_MISMATCH` | LVGL 版本不匹配 | `LvglException(VersionMismatch)` |
| -100 | `OK_DEFERRED` | 成功但已延迟（非错误） | 无异常 |

## 附录 C：配置项表（`lv_conf.h` 关键项）

| 配置项 | 建议值 | 影响 |
|---|---|---|
| `LV_COLOR_DEPTH` | 16 或 32 | 颜色精度；v9 中 `lv_color_t` 恒为 RGB888，此项影响内置类型 |
| `LV_USE_LOG` | 1（Debug）/ 0（Release） | 是否转发日志到仓颉 |
| `LV_MEM_SIZE` | ≥ 64 KB | LVGL 内存池 |
| `LV_DRAW_BUF_STRIDE_ALIGN` | 由 LVGL 默认 | ⚠️ 缓冲大小计算必须考虑（§3.4.2） |
| `LV_USE_PROFILER` | 1 | 性能计数器（§5.11） |
| `LV_USE_MEM_MONITOR` | 1 | 内存监控 |
| `LV_USE_ASSERT_*` | Debug 开 | 提前暴露问题 |
| `LV_USE_FS_POSIX` | 1 | 文件系统（§3.13.1） |
| `LV_USE_PNG/JPG` | 按需 | ⚠️ 引入新依赖，需入 SBOM |
| `LV_FONT_DEFAULT` | 内置或外部 | 字体体积影响 Flash |
| `LV_USE_SDL` | 0（MVP 手写） | 见 §8.1.6 |
| `LV_DEF_REFR_PERIOD` | 33 ms（~30 FPS） | 刷新周期 |

**所有改动必须同步更新 `conf_const.cj` 并触发重新构建**（§3.5.2）。
