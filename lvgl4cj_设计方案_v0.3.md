# lvgl4cj 设计方案

## 仓颉语言绑定 LVGL 的 GUI 框架

> 版本：**v0.3**（针对 v0.2 评审意见修订）
> 目标 LVGL 版本：**v9.x**（主线，锁 9.2+）
> 目标仓颉 SDK：**1.1.0**
> 定位：面向 **Linux / ARM64 Linux / OpenHarmony 的嵌入式 HMI**，桌面 SDL2 作为开发与验证环境

---

## 修订说明：v0.2 → v0.3

本版针对 v0.2 评审意见修订。评审指出 v0.2 的主要问题不是"遗漏"，而是**"设计已承诺但底层假设未验证"的内在张力**，以及**二阶交互洞**。本版重点解决这两类问题。

### D 类：结构性张力（最严重）

| #    | 问题                                              | 修订位置       | 处置                                               |
| ---- | ------------------------------------------------- | -------------- | -------------------------------------------------- |
| D1   | §3.2 按"闭包表"完整设计，但承认取决于 V2          | **§3.2 重写**  | 改为**条件式设计**（V2 分支 A/B），V2 结论前不动工 |
| D2   | 方案 C 作为 V1 回退，但 §11.2 性能指标按方案 A 定 | **§11.2 重写** | 指标分方案 A / 方案 C 两档，明确标注               |
| D3   | ADR-004 是"待定 ADR"，与 ADR 语义冲突             | **§16 修订**   | 拆为 ADR-004（已确认原则）+ ADR-013（Pending）     |

### E 类：二阶交互洞

| #    | 问题                                                         | 修订位置    | 处置                                          |
| ---- | ------------------------------------------------------------ | ----------- | --------------------------------------------- |
| E1   | 延迟删除 drain 只 swap 一次，链式删除可能多帧延迟            | **§3.8.2**  | 循环 drain（最大 64 轮），明确"同帧完成"      |
| E2   | 队列满默认"阻塞等待"，与跨线程投递组合可能死锁               | **§3.9.1**  | 默认改 **fail-fast**，阻塞降为可选模式        |
| E3   | SDL2 flush 与主线程渲染缓冲同步缺口                          | **§8.1.2**  | 明确 flush_wait_cb 必须实现，等主线程渲染完成 |
| E4   | 线程断言"仅 Debug"，Release 退化为数据竞争                   | **§3.6.5**  | Release 保留 TLS 廉价检查（首次调用后关闭）   |
| E5   | 动画 exec_cb 无 user_data，var 用作 closure_id 后对象引用丢失 | **§3.10.3** | 明确对象引用由仓颉闭包持有，写入用户文档      |

### F 类：API 缺口与小瑕疵

| #    | 问题                                           | 修订位置                             |
| ---- | ---------------------------------------------- | ------------------------------------ |
| F1   | `OK_DEFERRED = -100` 破坏"负数=错误"约定       | **附录 B**：改为 `+1`                |
| F2   | `obj_send_event` 的 `param` 语义不明           | **§5.6**：明确为句柄表注册对象       |
| F3   | 缺 z-order API（`move_foreground/background`） | **§5.5 新增**                        |
| F4   | `dump_tree` 返回 JSON，仓颉侧解析负担          | **§5.11 改为结构化数组**             |
| F5   | 同事件码多回调语义未说明                       | **§3.7.4**：明确"按注册顺序全部触发" |
| F6   | FFI 契约测试未要求 `@C` 声明                   | **§10.2 补充**                       |
| F7   | `postAndGet<T>` 命名误导（返回 Future 非 T）   | **§3.9.2 改名 `postAsync<T>`**       |
| F8   | 线程断言宏只适用于返回 int32 的函数            | **§3.6.5 两套宏**                    |
| F9   | 缺 `lv_tick_get` / `lv_tick_elaps`             | **§5.1 新增**                        |

### G 类：工程化

| #    | 问题                                             | 修订位置                     |
| ---- | ------------------------------------------------ | ---------------------------- |
| G1   | 代码生成依赖 Python，嵌入式构建环境可能无        | **§9.1 明确依赖 + 替代路径** |
| G2   | FreeType GPL 风险，但中文 HMI 必然需要           | **§14.1 提前到 P1 前评估**   |
| G3   | P0 任务与 V1–V5 依赖关系未画                     | **§12 补依赖图**             |
| G4   | "MVP 不暴露自定义 fs 驱动"可能被误解为 P3 才需要 | **§3.13.1 明确 P2 前必须补** |
| G5   | ASan suppression 符号命名需实证                  | **§9.5 标注"需实证确认"**    |
| G6   | `obj_clean` 与 DELETE 钩子语义未区分             | **§3.3 补充**                |
| G7   | DRAW_* 列在事件码表但实际非目标                  | **§3.7.3 标 NOT_SUPPORTED**  |

### H 类：文档完整性

| #    | 问题                        | 修订位置               |
| ---- | --------------------------- | ---------------------- |
| H1   | 缺 TL;DR / P0 速查卡        | **附录 D 新增**        |
| H2   | 缺"非目标 → 替代路径"对照表 | **§1.5 新增**          |
| H3   | §17 与 §11.1 高度重叠       | **§17 合并引用 §11.1** |

---

## 〇、一句话结论

**把项目定义为"受控 C ABI 工程"，而不是"自动生成 C 绑定的练习"。**

LVGL 是纯 C 库，ABI 层面比 Qt/C++ 简单；工程量集中在四件事：**线程亲和性、回调桥接、生命周期级联失效、绘制缓冲所有权**。这四件做对了，控件覆盖率只是时间问题。

⚠️ **v0.3 的关键立场**：这四件事中的前两件（线程、回调）**依赖 P0 第 1 周的验证结论 V1/V2**。V1/V2 出结论前，§3.2 / §3.6 / §3.8 / §3.9 的**代码不应动工**，只做设计准备。

---

## 一、项目定位与命名

### 1.1 命名

**`lvgl4cj`**，仓颉包名 `lvgl4cj`。与 TPC 现有风格一致（`lrc4cj`、`vlayout4cj`）。

### 1.2 支持矩阵（明确约束，避免后期争议）

| 维度         | MVP 支持                                  | 说明                                                       |
| ------------ | ----------------------------------------- | ---------------------------------------------------------- |
| **指针宽度** | **仅 64 位**                              | ⚠️ 32 位平台 `void*` 与 `Int64` 不兼容（见 §3.2），二期评估 |
| 操作系统     | Linux x86_64 / ARM64、macOS（仅开发验证） | 见 §9.3 平台矩阵                                           |
| 目标场景     | 嵌入式 HMI、工业面板、智能设备            | **不含 MCU/RTOS**，见 §13 决策门 G1                        |
| LVGL 版本    | 锁定 v9.2+                                | 不承诺跨大版本兼容                                         |
| 线程模型     | 单 LVGL OS 线程                           | 见 §3.6                                                    |
| 仓颉 SDK     | 1.1.0                                     | 见 §3.6 待确认项 V1                                        |

### 1.3 设计目标（按优先级）

1. **安全**：句柄失效可检测，无悬空指针与 double-free
2. **可调试**：错误有明确上下文，崩溃可定位到仓颉调用点
3. **确定性**：线程模型与重入行为有明确定义，不出现间歇性崩溃
4. **够用**：覆盖 HMI 常见控件与交互
5. **快**：UI 线程不被阻塞，FFI 开销可控

### 1.4 非目标（明确排除）

| 非目标                       | 原因                                    | 替代路径                           |
| ---------------------------- | --------------------------------------- | ---------------------------------- |
| 裸机 / MCU / RTOS            | 仓颉运行时资源模型未验证（决策门 G1）   | C 主控 + 仓颉业务                  |
| 多线程并发调用 LVGL          | LVGL 非线程安全                         | 单线程 + 任务投递（§3.6/§3.9）     |
| **`lv_draw_*` 底层绘制 API** | v9 draw unit 模型复杂，FFI 暴露成本极高 | **用 Canvas 控件**（§3.11）        |
| 自动生成全量 API             | 宏与回调必须手写                        | 生成器只覆盖 setter/getter（§3.5） |
| 新的声明式 UI 框架           | 超出绑定层职责                          | —                                  |
| 32 位平台                    | `void*` 截断风险                        | 二期用独立句柄表                   |

### 1.5 非目标 → 替代路径对照表（新增 H2）

| 用户想做                              | 非目标 API          | 替代路径                                              | 章节           |
| ------------------------------------- | ------------------- | ----------------------------------------------------- | -------------- |
| 自定义绘制（仪表盘、波形）            | `lv_draw_*`         | `LvCanvas`                                            | §3.11.2 / §6.5 |
| 绘制事件拦截                          | `LV_EVENT_DRAW_*`   | 不支持；用 Canvas 重绘                                | §3.7.3         |
| 从 SPI Flash / 自定义存储加载图片字体 | 自定义 `lv_fs` 驱动 | MVP：POSIX 驱动 + 路径前缀；**P2 前必须补自定义驱动** | §3.13.1        |
| 在 MCU 上跑仓颉 + LVGL                | —                   | C 主控渲染 + 仓颉跑业务（过 G1 后评估）               | §13 G1         |
| 32 位 ARM 设备                        | —                   | 无（MVP）；P3 用独立句柄表                            | §13 R13        |
| 直接跨线程调 `btn.setWidth()`         | —                   | `runtime.post { }`                                    | §6.7           |

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
│      · 闭包注册表（条件：V2=B 时才需要）                   │
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
│      backend_null    headless（CI 用）                    │
└─────────────────────────────────────────────────────────┘
```

### 2.1 为什么必须有 L2

| 直接 FFI 的问题                   | L2 解决方式                     | 章节  |
| --------------------------------- | ------------------------------- | ----- |
| `lv_obj_t*` 裸指针，无失效检测    | 句柄表 + 四态状态机             | §3.1  |
| 回调是 C 函数指针，仓颉闭包传不进 | trampoline + closure_id（条件） | §3.2  |
| 32 位 `void*` 截断                | `intptr_t` 显式转换 + 64 位约束 | §3.2  |
| 仓颉线程可能被 M:N 迁移           | C 侧 OS 线程跑主循环            | §3.6  |
| 回调中删除对象导致重入崩溃        | 延迟删除队列                    | §3.8  |
| `lv_conf.h` 编译期宏读不到        | 常量查询 + 哈希校验             | §3.5  |
| 绘制缓冲被 GC 移动                | C 侧对齐分配并持有              | §3.4  |
| 父删子导致子句柄悬空              | `LV_EVENT_DELETE` 级联失效      | §3.3  |
| 错误靠 assert，仓颉无感知         | 统一错误码 + 上下文 + 回调      | §3.12 |

---

## 三、核心机制（13 项）

### 3.1 句柄表与完整状态机

#### 3.1.1 四态定义

| 状态          | 含义           | 原生对象 | 句柄表项   | `ptr_of()` |
| ------------- | -------------- | -------- | ---------- | ---------- |
| `UNINIT`      | 未初始化       | —        | 无         | NULL       |
| `ALIVE`       | 有效           | 存在     | 存在       | 有效指针   |
| `INVALIDATED` | 原生对象已删除 | 已释放   | **仍存在** | NULL       |
| `RELEASED`    | 句柄表项已回收 | —        | 已移除     | NULL       |

**`INVALIDATED` 与 `RELEASED` 的区别是本设计的关键**：
- `INVALIDATED` 保留表项，让仓颉侧能给出**明确报错**（"对象已被删除"）而非崩溃，同时保留调试信息（删除时的调用栈）
- `RELEASED` 才是真正释放表项内存

#### 3.1.2 状态转移图

```
                 create / handle_of
                        │
                        ▼
                   ┌─────────┐
                   │  ALIVE  │
                   └────┬────┘
                        │
        ┌───────────────┼────────────────┐
        │               │                │
   close()       LV_EVENT_DELETE    (任何状态下)
   (显式)         (LVGL 内部删除)     重复 close/release
        │               │            幂等直接返回
        ▼               ▼
   ┌─────────────────────────┐
   │      INVALIDATED        │
   │  (保留表项 + 删除栈)      │
   └───────────┬─────────────┘
               │
        release()
               │
               ▼
        ┌─────────────┐
        │  RELEASED   │
        └─────────────┘
```

#### 3.1.3 幂等性契约（写死，必须单测覆盖）

| 操作         | 在 `ALIVE`               | 在 `INVALIDATED`   | 在 `RELEASED`      |
| ------------ | ------------------------ | ------------------ | ------------------ |
| `close()`    | → INVALIDATED            | 无操作（幂等）     | 无操作（幂等）     |
| `release()`  | → INVALIDATED → RELEASED | → RELEASED         | 无操作（幂等）     |
| `isAlive()`  | true                     | false              | false              |
| 任何业务 API | 正常                     | 抛 `InvalidHandle` | 抛 `InvalidHandle` |

#### 3.1.4 `close()` 内部的顺序安全

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

**统一靠 `LV_EVENT_DELETE` 钩子**——只要对象是通过 `lvglcj_obj_create` 创建的，就一定挂了钩子，无论谁删都能感知。

⚠️ **例外**：LVGL 内部自己创建的对象（如 dropdown 的列表、msgbox 的按钮）不经过我们的创建入口，没有钩子。**策略**：这类对象不向仓颉暴露句柄，或在暴露前由 C 侧补挂钩子。

---

### 3.2 回调桥接（条件式设计，取决于 V2）★重写 D1

⚠️ **本章有两套设计，最终采用哪套由 P0 第 1 周的 V2 结论决定。V2 出结论前，本章代码不动工。**

#### 3.2.1 待确认项 V2：仓颉 FFI 回调是否支持捕获

CJQT6 中存在 `setOnTimeoutCapture({ => ... })` 这类**可捕获闭包**变体，说明仓颉可能原生支持捕获回调。

**P0 第 1 周必须先确认**：

| 情况                  | 分支  | 处置                                |
| --------------------- | ----- | ----------------------------------- |
| 仓颉原生支持捕获回调  | **A** | 直接用，无 closure_id，无全局闭包表 |
| 仅顶层 `@C func` 支持 | **B** | 自建闭包表 + closure_id             |

#### 3.2.2 分支 A：仓颉原生支持捕获（理想情况）

**设计**：无闭包表，`user_data` 直接装仓颉闭包指针（或运行时句柄）。

```c
// 注册流程：仓颉侧直接传闭包
//   obj.on(Event.Clicked) { e => ... }
//     → C 侧 lv_obj_add_event_cb(obj, trampoline, code, (void*)cj_closure_ptr)

static void lvglcj_event_trampoline(lv_event_t *e) {
    // user_data 是仓颉闭包指针
    void *closure = lv_event_get_user_data(e);
    int64_t evh = lvglcj_handle_of(e);
    int rc = lvglcj_call_closure_direct(closure, evh);   // ★ 分支 A
    if (rc != 0) lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, -1, __func__);
    lvglcj_handle_invalidate(evh);
    lvglcj_handle_release(evh);
}
```

**优点**：设计大幅简化，§3.8 延迟删除队列的"注销闭包"部分不需要，§3.9 任务队列也不需要 closure_id 映射。

#### 3.2.3 分支 B：仅顶层 `@C func`（回退方案）

**设计**：闭包存入全局表，`user_data` 装 `int32_t` closure_id。

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

```c
static void lvglcj_event_trampoline(lv_event_t *e) {
    int32_t cid = lvglcj_ptr_to_cid(lv_event_get_user_data(e));
    int64_t evh = lvglcj_handle_of(e);
    int rc = lvglcj_call_closure(cid, evh);              // ★ 分支 B
    if (rc != 0) {
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, cid, __func__);
    }
    lvglcj_handle_invalidate(evh);
    lvglcj_handle_release(evh);
}
```

配套约束：
- 闭包表使用 `int32_t` ID，分配时检查不超过 `INT32_MAX`，超出则复用或报错
- **MVP 明确仅支持 64 位平台**（§1.2）。32 位（含 ARM32 OH 设备）需二期评估：改用独立句柄表传 ID，而非指针编码

#### 3.2.4 trampoline 集合（两分支共用）

每类回调一个固定 C 函数，共 6 个：

| #    | trampoline                     | 对应 LVGL 回调          | user_data 装载              |
| ---- | ------------------------------ | ----------------------- | --------------------------- |
| T1   | `lvglcj_event_trampoline`      | `lv_event_cb_t`         | A: 闭包指针 / B: closure_id |
| T2   | `lvglcj_timer_trampoline`      | `lv_timer_cb_t`         | 同上                        |
| T3   | `lvglcj_anim_exec_trampoline`  | `lv_anim_exec_xcb_t`    | 同上                        |
| T4   | `lvglcj_flush_trampoline`      | `lv_display_flush_cb_t` | 同上                        |
| T5   | `lvglcj_indev_read_trampoline` | `lv_indev_read_cb_t`    | 同上                        |
| T6   | `lvglcj_fs_trampoline`         | `lv_fs_drv_t` 各回调    | drv_id + op                 |

**异常处理（两分支共用）**：仓颉闭包内抛出的异常必须在 trampoline 边界捕获，转成日志 + 全局错误回调（§3.12.3）。

---

### 3.3 生命周期级联失效

见 §3.1.4 时序。补充三条：

1. **递归失效的顺序**：先深后浅（叶子 → 根），保证父对象在处理时子对象已失效
2. **`lv_obj_clean()` 的语义**（新增 G6）：清空子对象但保留父对象。`lifecycle.c` 需通过事件目标的父子关系判断——如果 `current_target` 是正在被 clean 的父对象的子，则该子失效；父对象本身不失效
3. **DELETE 钩子的注册位置**：统一在 `lvglcj_obj_create` 里挂，保证所有通过绑定层创建的对象都有钩子

---

### 3.4 绘制缓冲

#### 3.4.1 为什么必须在 C 侧分配

**LVGL v9 的 `lv_display_set_buffers()` 接收裸缓冲区指针并在 flush 期间长期持有。**

⚠️ **绝不能把仓颉 `Array<UInt8>` 的底层指针传给 LVGL** —— GC 可能移动或回收，导致花屏/段错误。**缓冲必须在 C 侧分配并由桥接层持有。**

#### 3.4.2 stride 对齐

LVGL v9 引入 `lv_draw_buf_t` 与 stride 对齐概念。**缓冲区大小不能简单按 `w * h * bpp` 计算**：

```c
// 错误的算法
buf_bytes = w * h / 10 * bytes_per_pixel;     // ✗ 未考虑 stride 对齐

// 正确算法
size_t lvglcj_calc_buf_bytes(int32_t w, int32_t h, int32_t fmt) {
    size_t stride = lv_draw_buf_width_to_stride(w, fmt);   // ✓ 用 LVGL 自己的算子
    return stride * h;
}
```

**原则：任何缓冲尺寸计算都调 LVGL 提供的 `lv_draw_buf_*` 算子，不自己算。**

#### 3.4.3 颜色格式与字节序

v9 中 `lv_color_t` 恒为 RGB888，但**显示缓冲格式由 `lv_display_set_color_format()` 独立决定**，两者不同：

| 格式                                    | bpp  | 说明               |
| --------------------------------------- | ---- | ------------------ |
| `LV_COLOR_FORMAT_RGB565`                | 2    | 最常见，MCU 屏首选 |
| `LV_COLOR_FORMAT_RGB888`                | 3    | 无 alpha           |
| `LV_COLOR_FORMAT_ARGB8888` / `XRGB8888` | 4    | 桌面/带 GPU 场景   |

⚠️ **flush 回调签名 v9 为 `(lv_display_t*, const lv_area_t*, uint8_t* px_map)`** —— 第三参是 `uint8_t*`，**不是 v8 的 `lv_color_t*`**。后端必须按 `lv_display_get_color_format()` 决定按 2/3/4 字节解释，否则"偶尔花屏"。

字节序：RGB565 在 SPI 屏上通常需 `lv_draw_sw_rgb565_swap()` 或传输时交换，后端需提供开关。

#### 3.4.4 三种渲染模式差异

| 模式      | 缓冲大小  | flush 语义                 | 适用                 |
| --------- | --------- | -------------------------- | -------------------- |
| `PARTIAL` | ≥ 1/10 屏 | 只刷脏区，分块调用多次     | **MVP 默认**，省内存 |
| `DIRECT`  | **整屏**  | 只刷脏区，但缓冲含完整图像 | 有足够 RAM           |
| `FULL`    | **整屏**  | 每次全刷                   | 双缓冲传统模型       |

分块处理：LVGL 可能多次调用 flush，用 `lv_display_flush_is_last()` 判断最后一块。**后端不能在非最后块时提交帧**。

#### 3.4.5 `flush_wait_cb`

异步传输（DMA）场景下，若 flush 返回后传输未完成就继续渲染，会撕裂。v9 提供：

```c
lv_display_set_flush_wait_cb(disp, my_wait_cb);   // LVGL 用它等待传输完成
```

**作用**：不用在 flush 里自旋阻塞，LVGL 通过 wait_cb 用信号量/轮询高效等待。**后端若支持异步传输，必须实现 wait_cb**（§8.1.2 SDL2 场景明确要求）；同步传输（SDL2 纹理拷贝）可省略。

#### 3.4.6 缓存一致性与 DMA

ARM 带 cache 的平台，DMA 直接读缓冲会读到脏数据。需要：
- 缓冲分配考虑 cache line 对齐（≥64B）
- 传输前 `clean`，接收后 `invalidate`
- 由后端提供 `flush_cb` 内的 cache 操作钩子（MVP 不涉及，写入设计约束）

#### 3.4.7 其他

| 项              | 处理                                                         |
| --------------- | ------------------------------------------------------------ |
| **malloc 失败** | `lvglcj_display_create` 返回负错误码（`ERR_OUT_OF_MEMORY`），仓颉侧抛异常；**不静默降级** |
| **旋转**        | `lv_display_set_rotation()` 会改变宽高语义，后端需监听 `LV_EVENT_RESOLUTION_CHANGED` 重建缓冲 |
| **删除顺序**    | 先 `lv_display_delete()`（LVGL 停止引用），**再** `free()` 缓冲。反了会 use-after-free |
| **双缓冲同步**  | DIRECT/FULL 模式下 LVGL 内部管理两块缓冲的乒乓，桥接层只负责分配与最终释放 |

---

### 3.5 `lv_conf.h` 版本化与代码生成

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

#### 3.5.2 编译期哈希如何写入仓颉

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

**依赖声明**（G1 修订）：**Python 3.8+ 是构建时依赖**，写入 README 的"构建前置条件"。若目标环境无法安装 Python，提供**纯 CMake 替代路径**：用 `configure_file` + `file(WRITE)` 在 CMake 中直接生成 `.cj`（牺牲灵活性，但无 Python 依赖）。

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

### 3.6 线程模型

#### 3.6.1 问题本质

**LVGL 要求所有 `lv_*` 调用在同一个 OS 线程内。仓颉的 `spawn` 创建的是轻量级线程（用户态线程，M:N 调度），可能被调度到不同 OS 线程上。**

如果那个线程是仓颉轻量级线程，它可能在 OS 线程间迁移，单线程模型直接失效，且失败是**间歇性的**。

#### 3.6.2 三个候选方案

| 方案          | 机制                                                         | 优点                        | 风险                                 |
| ------------- | ------------------------------------------------------------ | --------------------------- | ------------------------------------ |
| **A（推荐）** | **C 侧 `pthread_create` OS 线程**跑主循环；仓颉只通过队列投递任务 | OS 线程确定，与仓颉调度解耦 | 需仓颉支持"外部 OS 线程调用仓颉闭包" |
| B             | 仓颉侧 `spawn` + 验证亲和性                                  | 纯仓颉，无附着问题          | 亲和性不可控，M:N 迁移即失效         |
| C             | 泵模式：仓颉主线程定期调 `lvglcj_pump()`                     | 最简单                      | UI 与业务抢同一线程，阻塞即卡顿      |

#### 3.6.3 方案 A 的关键未知：**待确认项 V1（P0 阻塞项）**

方案 A 要求在一个**由 C 创建、仓颉运行时不认识的 OS 线程**里执行仓颉闭包。这需要仓颉运行时提供线程附着机制（类似 JNI `AttachCurrentThread`）。

```
C OS 线程循环：
  while (running) {
      task = queue_pop();
      if (task) {
          lvglcj_attach_cangjie();     // ★ 需要这一步，仓颉是否提供？
          call_cangjie_closure(task);
          lvglcj_detach_cangjie();
      }
      lv_timer_handler();
      sleep_ms(5);
  }
```

**P0 第一周必须确认**：

| 确认项                                     | 方法                                                 |
| ------------------------------------------ | ---------------------------------------------------- |
| 仓颉是否提供外部 OS 线程附着 API           | 查 SDK 文档 / 问仓颉社区 / 读 `cangjie_runtime` 源码 |
| 若没有，仓颉轻量级线程是否稳定绑定 OS 线程 | **探针测试**（§3.6.4）                               |
| 若也没有，闭包执行是否仅限仓颉线程         | 则需退回方案 C（泵模式）                             |

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

#### 3.6.5 线程断言（两套宏，Release 保留）★修订 E4

**旧设计问题**：v0.2 只在 Debug 构建加断言，Release 下跨线程调用退化为静默数据竞争——比 Debug 下崩更难定位。

**修订**：Release 保留**廉价 TLS 检查**。`pthread_self()` 在现代 Linux/macOS 上是 TLS 读取，非系统调用，纳秒级开销。或者用**首次调用检查 + 后续关闭**策略。

```c
// 两套宏：一套用于返回 int32_t 的函数，一套用于 void 函数
#define LVGLCJ_CHECK_LVGL_THREAD_RET()                                 \
    do {                                                               \
        if (lvglcj_os_thread_self() != g_lvgl_thread_id) {              \
            lvglcj_record_error(LVGLCJ_ERR_WRONG_THREAD, 0, __func__);  \
            return LVGLCJ_ERR_WRONG_THREAD;                             \
        }                                                               \
    } while (0)

#define LVGLCJ_CHECK_LVGL_THREAD_VOID()                                \
    do {                                                               \
        if (lvglcj_os_thread_self() != g_lvgl_thread_id) {              \
            lvglcj_record_error(LVGLCJ_ERR_WRONG_THREAD, 0, __func__);  \
            return;                                                     \
        }                                                               \
    } while (0)
```

**Release 构建**：`g_lvgl_thread_id` 初始化后，检查始终启用。仅在 `LVGLCJ_DISABLE_THREAD_CHECK` 显式定义时关闭（用于性能基准极端测试）。

#### 3.6.6 最终模型

**方案 A（V1 通过时）**：

```cangjie
public class LvglRuntime {
    private let taskQueue: BoundedQueue<() -> Unit>
    private var running: Bool = false

    public func start(): Unit {
        lvglcj_init()
        checkCompatibility()
        lvglcj_set_tick_cb()
        lvglcj_set_log_cb(onLvglLog)
        lvglcj_start_thread()               // ★ C 侧 pthread_create
        this.running = true
    }

    public func post(task: () -> Unit): Future<Unit> { ... }
    public func stop(mode: ShutdownMode): Unit { ... }
}
```

**方案 C（V1 失败时，回退）**：

```cangjie
public class LvglRuntime {
    public func start(): Unit {
        lvglcj_init()
        checkCompatibility()
        // 不启线程；用户主循环定期调 pump
    }
    public func pump(): Unit {
        lvglcj_drain_deferred()
        lvglcj_timer_handler()
    }
}

// 用户代码：
while (running) {
    runtime.pump()
    // 业务逻辑
    sleep(5.milliseconds)
}
```

⚠️ **方案 C 下，UI 与业务抢同一线程**。§11.2 的性能指标分两档（见修订 D2）。

---

### 3.7 事件语义完整模型

#### 3.7.1 target vs current_target

| 概念             | 含义                                             | FFI                               |
| ---------------- | ------------------------------------------------ | --------------------------------- |
| `target`         | **最初**触发事件的对象（冒泡链起点）             | `lvglcj_event_get_target`         |
| `current_target` | **当前**正在处理该事件的对象（冒泡链中的某一级） | `lvglcj_event_get_current_target` |

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
int32_t lvglcj_obj_add_flag(int64_t obj, int32_t flag);
```

#### 3.7.3 事件码表（完整）★修订 G7

| 类别      | 事件码                                                       | MVP                                 |
| --------- | ------------------------------------------------------------ | ----------------------------------- |
| 输入      | `PRESSED` `PRESSING` `PRESS_LOST` `RELEASED` `CLICKED` `LONG_PRESSED` `LONG_PRESSED_REPEAT` | ✅                                   |
| 值变更    | `VALUE_CHANGED` `READY` `CANCEL`                             | ✅                                   |
| 生命周期  | `DELETE` `CHILD_CHANGED` `SCREEN_LOADED` `SCREEN_UNLOADED`   | ✅（`DELETE` 内部用）                |
| 焦点      | `FOCUSED` `DEFOCUSED`                                        | ✅                                   |
| 布局/尺寸 | `SIZE_CHANGED` `STYLE_CHANGED` `LAYOUT_CHANGED` `REFR_EXT_DRAW_SIZE` | 部分                                |
| 滚动      | `SCROLL_BEGIN` `SCROLL_END` `SCROLL`                         | P1                                  |
| 绘制      | `DRAW_MAIN_BEGIN` `DRAW_MAIN` `DRAW_MAIN_END` `DRAW_POST_BEGIN` `DRAW_POST` `DRAW_POST_END` | **NOT_SUPPORTED**（注册返回 `-13`） |
| 特殊      | `REFRESH` `ALL` `PREPROCESS` `HIT_TEST` `COVER_CHECK`        | P2                                  |

⚠️ **DRAW_* 事件注册时应直接返回 `NOT_SUPPORTED` 错误码**，不要留在表里让人误以为可用（§3.11 已明确为非目标）。

#### 3.7.4 事件分发顺序（新增 F5）

LVGL 按**注册顺序**调用同一对象的多个回调。绑定层保证：

1. 内部 `DELETE` 钩子**总是第一个注册**（保证最先执行，先注销闭包）
2. **同一事件码的多个用户回调按注册顺序全部触发**（LVGL 原生支持，非覆盖）
3. 用户回调中调 `stop_bubbling` 只影响向父级传播，不影响同对象其他回调

---

### 3.8 回调重入规则

#### 3.8.1 重入场景矩阵

| 场景                                    | 是否允许               | 机制                      |
| --------------------------------------- | ---------------------- | ------------------------- |
| 回调中修改**其他**对象属性              | ✅ 允许                 | —                         |
| 回调中**创建**对象                      | ✅ 允许                 | —                         |
| 回调中删除**其他**对象                  | ⚠️ 允许但走**延迟删除** | 见下                      |
| 回调中删除**自身**                      | ⚠️ 允许但走**延迟删除** | 见下                      |
| 回调中**注销**自己的回调                | ⚠️ 允许但走**延迟注销** | 见下                      |
| 回调中触发新事件（`lv_obj_send_event`） | ⚠️ 限制深度 ≤ 8         | 超限报错                  |
| 回调中 `post` 并**同步等待**            | ❌ **禁止**             | 死锁，运行时检测          |
| 回调中抛异常                            | ❌ 禁止跨越 C 边界      | trampoline 捕获（§3.2.4） |

#### 3.8.2 延迟删除队列（核心机制）★修订 E1

**问题**：回调执行中直接 `lv_obj_delete`，若该对象正在事件分发链上，会导致迭代器失效/use-after-free。

**方案**：执行上下文标记 + 延迟队列 + **循环 drain**

```c
static _Atomic int g_callback_depth = 0;
static lvglcj_deferred_list_t g_deferred;

int32_t lvglcj_obj_delete(int64_t h) {
    if (atomic_load(&g_callback_depth) > 0) {
        lvglcj_deferred_push(DEFER_DELETE, h);
        lvglcj_handle_mark_pending_delete(h);
        return LVGLCJ_OK_DEFERRED;    // 非负返回值（见附录 B 修订）
    }
    return lvglcj_obj_delete_now(h);
}

// ★ 循环 drain：处理链式延迟删除，直到队列为空或达到最大迭代次数
void lvglcj_drain_deferred(void) {
    const int MAX_ITER = 64;
    int iter = 0;
    while (!lvglcj_deferred_empty(&g_deferred)) {
        if (++iter > MAX_ITER) {
            lvglcj_record_error(LVGLCJ_ERR_DEFERRED_LOOP, 0, __func__);
            break;   // ★ 保护：避免无限循环，报错而非死锁
        }
        lvglcj_deferred_item_t item = lvglcj_deferred_pop(&g_deferred);
        switch (item.op) {
            case DEFER_DELETE:  lvglcj_obj_delete_now(item.h); break;
            case DEFER_UNREG:   lvglcj_unregister_closure_now(item.cid); break;
        }
        // ★ 处理过程中若触发新的延迟项，会进入 g_deferred，由 while 循环继续
    }
}
```

**语义明确**（写入用户文档）：**回调中调 `obj.close()` 不会立即生效，但对象立刻进入"待删除"状态（后续 API 调用抛异常），实际删除最迟在同一帧内完成**（drain 在主循环每轮 `lv_timer_handler` 之前执行）。

#### 3.8.3 回调期句柄锁定

回调执行期间，被回调引用的对象句柄标记为 `IN_CALLBACK`，此时：
- `close()` → 转延迟删除
- 其他业务 API → 允许（LVGL 本身允许回调中改属性）

#### 3.8.4 死锁检测

`postAndWait` 检测当前是否在 LVGL 线程：

```c
if (lvglcj_os_thread_self() == g_lvgl_thread_id) {
    return LVGLCJ_ERR_DEADLOCK_RISK;    // ★ 明确报错，不真锁
}
```

---

### 3.9 任务队列语义 ★修订 E2

#### 3.9.1 完整定义

| 属性         | 定义                                                         |
| ------------ | ------------------------------------------------------------ |
| 容量         | **有界**，默认 1024；可配置                                  |
| **满时策略** | **默认 fail-fast：抛 `QueueFull` 异常**；可选"阻塞等待"、"丢弃最旧" |
| 返回值       | `Future<Unit>`（可 `await`，也可忽略）                       |
| 异常传播     | 任务抛异常 → 存入 `Future`，由调用方 `await` 时重新抛出；未 await 则走全局错误回调 |
| 关闭策略     | `Drain`（执行完剩余）／`Discard`（丢弃）／`DrainWithTimeout(ms)` |
| 线程断言     | 非 LVGL 线程直接调 API → `ERR_WRONG_THREAD`（§3.6.5）        |

**为什么默认 fail-fast（修订 E2）**：

v0.2 默认"阻塞等待"会导致**跨线程投递死锁**：业务线程 A 投 T1 到满队列阻塞；LVGL 线程执行 T0 中又需投 T2 → 队列仍满 → 双向等待。

**Go channel、Rust crossbeam、Disruptor 均不默认阻塞**。跨线程消息系统默认阻塞几乎总是错误设计。

"阻塞等待"作为可选模式：`queue.setFullPolicy(Block)`，需要用户明确承担死锁风险。

#### 3.9.2 接口 ★修订 F7

```cangjie
public class LvglRuntime {
    // 异步投递，返回 Future
    public func post(task: () -> Unit): Future<Unit>

    // 异步投递 + 返回值（改名，语义清晰）
    public func postAsync<T>(task: () -> T): Future<T>      // ★ 由 postAndGet 改名

    // 同步投递并等待（禁止在 LVGL 线程内调用）
    public func postAndWait(task: () -> Unit): Unit

    public func stop(mode: ShutdownMode): Unit
}

public enum ShutdownMode { Drain | Discard | DrainWithTimeout(Int64) }
public enum QueueFullPolicy { FailFast | Block | DropOldest }
```

#### 3.9.3 关闭时未执行任务

- `Drain`：执行完队列中所有任务再退出（可能耗时，需日志提示）
- `Discard`：清空队列，未执行任务触发 `TaskDiscarded` 回调（用户可记录）
- `DrainWithTimeout(n)`：n 毫秒内尽量执行，超时后按 `Discard` 处理

---

### 3.10 动画 API

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
int64_t lvglcj_anim_create(void);
int32_t lvglcj_anim_set_var(int64_t anim, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t anim, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t anim, int32_t cid);     // ★ trampoline T3
int32_t lvglcj_anim_set_path(int64_t anim, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t anim, int32_t cnt);
int32_t lvglcj_anim_set_playback(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t anim, int32_t cid);  // 闭包清理
int32_t lvglcj_anim_start(int64_t anim);
int32_t lvglcj_anim_delete(int64_t anim);
int32_t lvglcj_obj_delete_anim(int64_t obj);
```

#### 3.10.3 exec_cb trampoline（T3）★修订 E5

```c
static void lvglcj_anim_exec_trampoline(void *var, int32_t value) {
    // 分支 A：var 即闭包指针
    // 分支 B：var 即 closure_id（intptr_t 编码）
    int rc = lvglcj_call_closure_anim(var, value);
    if (rc != 0) lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, -1, __func__);
}
```

⚠️ **v9 的 `exec_cb` 没有独立 user_data 参数，只有 `var`**。因此把 `var` 直接用作 closure_id（或闭包指针），**不指向真实对象**。

**关键语义澄清（修订 E5）**：
- `lv_anim_set_var(anim, obj)` 的 obj 参数**仅用于 LVGL 内部"对象删除时自动停动画"**，不参与 exec_cb 的身份识别
- **动画的目标对象引用由仓颉闭包自己持有**（`onExec { v => btn.setWidth(v) }` 已捕获 `btn`）
- 这层语义必须写进用户文档，否则用户会误以为 `setVar` 的对象就是动画目标

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
    onExec { v => btn.setWidth(v) }    // ★ 目标引用由闭包持有
}.start()
```

---

### 3.11 自定义绘制策略

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

### 3.12 错误模型

#### 3.12.1 错误码表

见附录 B。C 侧统一 `int32_t` 返回值，0 = 成功，负数 = 错误，正数 = 特殊成功标记。仓颉侧映射为 `LvglException`。

#### 3.12.2 错误上下文

```c
typedef struct {
    int32_t  code;
    int64_t  handle;        // 相关句柄（若有）
    int32_t  closure_id;    // 相关闭包（若是回调错误）
    const char *func;       // C 侧函数名
    const char *msg;        // 可读信息
} lvglcj_error_t;

int32_t lvglcj_last_error(lvglcj_error_t *out);
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

```cangjie
LvglRuntime.onError { err =>
    println("[lvgl4cj] ${err.code} @ ${err.nativeFunc}, handle=${err.handle}")
}
```

**默认行为**：未注册时打印到 stderr，不静默。

---

### 3.13 文件系统 / 图片 / 字体 / Group

#### 3.13.1 文件系统（`lv_fs`）★修订 G4

LVGL 的 `lv_fs_drv_t` 需要注册 open/read/seek/tell/close 回调。

**MVP 策略**：不暴露自定义 fs 驱动注册（回调多、收益低），改为在 C 侧预置一个 **"POSIX" 驱动**（`lv_fs_posix` 或自写），覆盖图片/字体从文件路径加载的需求。

```c
int32_t lvglcj_fs_init_posix(const char *root);
```

⚠️ **重要说明（修订 G4）**：嵌入式场景常有 SPI Flash、eMMC、自定义存储，**POSIX 驱动在 MVP 只是"能跑通"，不足以支撑真实 HMI**。**自定义 fs 驱动是 P2 前必须补的能力**，不是可选项。

自定义 fs 驱动需要 5 个 trampoline（T6）。

#### 3.13.2 图片解码

| 格式       | 方案                                            | 许可              |
| ---------- | ----------------------------------------------- | ----------------- |
| 内置解码器 | LVGL 自带（编译进 `lv_conf.h` 的 `LV_USE_...`） | MIT               |
| PNG        | `lv_libpng`（依赖 libpng）或 LVGL 内置          | libpng: zlib-like |
| JPG        | `lv_libjpeg-turbo`                              | IJG / BSD         |
| 位图       | `lv_image_set_src(obj, "A:path")`               | —                 |

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

```c
int64_t lvglcj_group_create(void);
int32_t lvglcj_group_delete(int64_t group);
int32_t lvglcj_group_add_obj(int64_t group, int64_t obj);
int32_t lvglcj_group_remove_obj(int64_t obj);
int32_t lvglcj_group_focus_obj(int64_t obj);
int64_t lvglcj_group_get_focused(int64_t group);
int32_t lvglcj_group_set_default(int64_t group);
int32_t lvglcj_indev_set_group(int64_t indev, int64_t group);
```

仓颉侧：`LvGroup`，`indev.bindGroup(group)`。

---

## 四、目录结构

```
lvgl4cj/
├── README.md
├── LICENSE                        # Apache-2.0
├── NOTICE
├── CHANGELOG.md
├── CONTRIBUTING.md
├── cjpm.toml
├── src/
│   ├── lvgl.cj
│   ├── runtime.cj
│   ├── handle.cj
│   ├── error.cj
│   ├── queue.cj
│   ├── core/
│   │   ├── object.cj
│   │   ├── display.cj
│   │   ├── indev.cj
│   │   ├── group.cj
│   │   ├── event.cj
│   │   ├── style.cj
│   │   ├── timer.cj
│   │   ├── anim.cj
│   │   ├── canvas.cj
│   │   ├── fs.cj
│   │   ├── font.cj
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
│   ├── debug/
│   │   ├── dump.cj
│   │   ├── screenshot.cj
│   │   └── perf.cj
│   ├── ffi/
│   │   └── bridge.cj
│   └── generated/
│       └── conf_const.cj          # 自动生成，勿手改
├── native/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── lvglcj_bridge.h
│   │   ├── lvglcj_error.h
│   │   ├── handle_table.h
│   │   └── callback.h
│   ├── src/
│   │   ├── handle_table.c
│   │   ├── lifecycle.c
│   │   ├── callback.c
│   │   ├── deferred.c
│   │   ├── thread.c
│   │   ├── display.c
│   │   ├── indev.c
│   │   ├── group.c
│   │   ├── obj.c
│   │   ├── style.c
│   │   ├── event.c
│   │   ├── timer.c
│   │   ├── anim.c
│   │   ├── canvas.c
│   │   ├── fs.c
│   │   ├── font.c
│   │   ├── conf_probe.c
│   │   ├── debug.c
│   │   └── log.c
│   └── probe/
│       └── probe_conf.c
├── backend/
│   ├── sdl2/
│   ├── fbdev/
│   ├── drm/
│   ├── oh/
│   └── null/                      # headless（CI 用）
├── third_party/
│   └── lvgl/
├── examples/
│   ├── hello_cj/
│   ├── widgets_demo/
│   ├── anim_demo/
│   ├── canvas_demo/
│   └── hmi_panel/
├── test/
│   ├── unit/
│   │   ├── handle_test.cj
│   │   ├── callback_test.cj
│   │   ├── event_test.cj
│   │   ├── thread_test.cj
│   │   ├── anim_test.cj
│   │   └── deferred_test.cj
│   ├── ffi_contract_test.cj
│   ├── soak_test.cj
│   └── native/
│       ├── CMakeLists.txt
│       └── test_handle_table.c
├── scripts/
│   ├── build_native.sh / .ps1
│   ├── gen_conf_const.py
│   ├── gen_bindings.py
│   └── run_debug.ps1
├── docs/
│   ├── adr/
│   ├── api/
│   ├── tutorials/
│   ├── benchmarks/
│   └── P0_QUICKSTART.md           # 附录 D
└── .github/
    ├── ISSUE_TEMPLATE/
    ├── PULL_REQUEST_TEMPLATE.md
    └── workflows/ci.yml
```

---

## 五、C ABI 接口清单

**约定**：`lvglcj_<子系统>_<动作>`，全 `extern "C"`，返回 `int32_t` 错误码（0 成功，负数错误，正数特殊标记）。默认符号可见性 `-fvisibility=hidden`，导出标记 `LVGLCJ_API`。

### 5.1 运行时与线程 ★修订 F9

```c
int32_t lvglcj_init(void);
int32_t lvglcj_deinit(void);
void    lvglcj_set_tick_cb(void);
int32_t lvglcj_set_log_cb(int32_t cid);
const char *lvglcj_version(void);
uint32_t lvglcj_conf_hash(void);

// ★ 时间（新增 F9）
uint32_t lvglcj_tick_get(void);              // lv_tick_get()
uint32_t lvglcj_tick_elaps(uint32_t prev);   // lv_tick_elaps()

// 线程
int32_t lvglcj_start_thread(void);
int32_t lvglcj_stop_thread(int32_t shutdown_mode);
int64_t lvglcj_current_os_tid(void);
int32_t lvglcj_is_lvgl_thread(void);
int32_t lvglcj_post_task(int64_t task_id);
void    lvglcj_drain_deferred(void);
```

### 5.2 句柄（四态）

```c
int64_t lvglcj_handle_of(void *ptr);
int32_t lvglcj_handle_alive(int64_t h);
int32_t lvglcj_handle_state(int64_t h);
void    lvglcj_handle_invalidate(int64_t h);
void    lvglcj_handle_release(int64_t h);
int32_t lvglcj_handle_pending_delete(int64_t h);
```

### 5.3 Display

```c
int64_t lvglcj_display_create(int32_t w, int32_t h, int32_t color_format,
                              int32_t buf_mode, int32_t buf_lines);
int32_t lvglcj_display_set_color_format(int64_t disp, int32_t fmt);
int32_t lvglcj_display_set_flush_cb(int64_t disp, int32_t cid);
int32_t lvglcj_display_set_flush_wait_cb(int64_t disp, int32_t cid);
int32_t lvglcj_display_flush_ready(int64_t disp);
int32_t lvglcj_display_flush_is_last(int64_t disp);
int32_t lvglcj_display_set_rotation(int64_t disp, int32_t rot);
int32_t lvglcj_display_set_resolution(int64_t disp, int32_t w, int32_t h);
int32_t lvglcj_display_get_color_format(int64_t disp);
int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid);
int32_t lvglcj_display_delete(int64_t disp);
```

### 5.4 InDev + Group

```c
int64_t lvglcj_indev_create(int32_t type);
int32_t lvglcj_indev_set_read_cb(int64_t indev, int32_t cid);
int32_t lvglcj_indev_set_display(int64_t indev, int64_t disp);
int32_t lvglcj_indev_set_group(int64_t indev, int64_t group);
int32_t lvglcj_indev_delete(int64_t indev);

int64_t lvglcj_group_create(void);
int32_t lvglcj_group_delete(int64_t g);
int32_t lvglcj_group_add_obj(int64_t g, int64_t obj);
int32_t lvglcj_group_remove_obj(int64_t obj);
int32_t lvglcj_group_focus_obj(int64_t obj);
int64_t lvglcj_group_get_focused(int64_t g);
int32_t lvglcj_group_focus_next(int64_t g);
int32_t lvglcj_group_focus_prev(int64_t g);
int32_t lvglcj_group_set_default(int64_t g);
```

### 5.5 对象 ★修订 F3

```c
int64_t lvglcj_obj_create(int64_t parent);
int32_t lvglcj_obj_delete(int64_t obj);
int32_t lvglcj_obj_clean(int64_t obj);
int32_t lvglcj_obj_set_pos / set_size / set_parent
int32_t lvglcj_obj_add_flag / remove_flag
int32_t lvglcj_obj_add_state / remove_state
int64_t lvglcj_screen_active(void);
int32_t lvglcj_screen_load(int64_t scr);
int32_t lvglcj_screen_load_anim(int64_t scr, int32_t anim_type,
                                int32_t time, int32_t delay, int32_t auto_del);
int64_t lvglcj_obj_get_child(int64_t obj, int32_t idx);
int32_t lvglcj_obj_get_child_count(int64_t obj);

// ★ z-order（新增 F3）
int32_t lvglcj_obj_move_foreground(int64_t obj);
int32_t lvglcj_obj_move_background(int64_t obj);
int32_t lvglcj_obj_move_to_index(int64_t obj, int32_t index);
int32_t lvglcj_obj_swap(int64_t obj1, int64_t obj2);
```

### 5.6 事件 ★修订 F2

```c
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int32_t cid);
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t ev_dsc);
int32_t lvglcj_obj_remove_event_by_cid(int64_t obj, int32_t cid);
int32_t lvglcj_obj_send_event(int64_t obj, int32_t code, int64_t param);
// ★ param 语义（修订 F2）：必须是句柄表注册的对象句柄（通过 lvglcj_handle_of 获得）
//    如需传自定义数据，通过 lvglcj_event_set_user_data / get_user_data 配套

int32_t lvglcj_event_set_user_data(int64_t evh, int64_t handle);   // ★ 新增
int32_t lvglcj_event_get_code(int64_t evh);
int64_t lvglcj_event_get_target(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);
int64_t lvglcj_event_get_user_data(int64_t evh);
int32_t lvglcj_event_stop_bubbling(int64_t evh);
int32_t lvglcj_event_stop_trickling(int64_t evh);

int32_t lvglcj_display_add_event(int64_t disp, int32_t code, int32_t cid);
int32_t lvglcj_indev_add_event(int64_t indev, int32_t code, int32_t cid);
```

### 5.7 样式

v9 样式 API 已去掉 state 参数，改用 **selector**。以下均带 `int32_t selector` 后缀参数。

| 类别     | 属性                                                         |
| -------- | ------------------------------------------------------------ |
| 背景     | `bg_color` `bg_opa` `bg_grad_color` `bg_grad_dir` `bg_grad_stop` `bg_image_src` `bg_image_opa` `bg_image_recolor` `bg_image_tiled` |
| 边框     | `border_width` `border_color` `border_opa` `border_side` `border_post` |
| 圆角     | `radius` `clip_corner`                                       |
| 内外边距 | `pad_top/bottom/left/right/row/column` `pad_all`             |
| 尺寸     | `width` `height` `min_width` `max_width` `min_height` `max_height` `length` |
| 文本     | `text_color` `text_opa` `text_font` `text_letter_space` `text_line_space` `text_decor` `text_align` |
| 阴影     | `shadow_width` `shadow_color` `shadow_opa` `shadow_offset_x/y` `shadow_spread` |
| 轮廓     | `outline_width` `outline_color` `outline_opa` `outline_pad`  |
| 变换     | `translate_x/y` `scale_x/y` `rotate` `transform_pivot_x/y`   |
| 图片     | `image_opa` `image_recolor` `image_recolor_opa`              |
| 线条     | `line_width` `line_dash_width` `line_dash_gap` `line_rounded` |
| 弧形     | `arc_width` `arc_rounded` `arc_color` `arc_opa` `arc_image_src` |
| 动画     | `anim` `anim_time` `anim_speed` `transition`                 |
| 混合     | `blend_mode`                                                 |
| 布局     | `layout` `base_dir`                                          |
| 其他     | `opa` `color_filter_opa` `recolor` `bitmap_mask_src` `rotary_sensitivity` |

```c
int64_t lvglcj_style_create(void);
int32_t lvglcj_style_delete(int64_t style);
int32_t lvglcj_style_set_<prop>(int64_t style, <type> value);
int32_t lvglcj_obj_add_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style_all(int64_t obj);
int64_t lvglcj_obj_get_style_<prop>(int64_t obj, int32_t part);
```

### 5.8 Timer

```c
int64_t lvglcj_timer_create(int32_t cid, int32_t period_ms);
int32_t lvglcj_timer_delete(int64_t timer);
int32_t lvglcj_timer_pause(int64_t timer);
int32_t lvglcj_timer_resume(int64_t timer);
int32_t lvglcj_timer_set_period(int64_t timer, int32_t ms);
int32_t lvglcj_timer_ready(int64_t timer);
int32_t lvglcj_timer_handler(void);
```

### 5.9 动画

```c
int64_t lvglcj_anim_create(void);
int32_t lvglcj_anim_set_var(int64_t a, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t a, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_set_path(int64_t a, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t a, int32_t cnt);
int32_t lvglcj_anim_set_playback(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_set_start_cb / ready_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_start(int64_t a);
int32_t lvglcj_anim_delete(int64_t a);
int32_t lvglcj_obj_delete_anim(int64_t obj);
int32_t lvglcj_anim_count_running(void);
```

### 5.10 文件系统 / 图片 / 字体

```c
int32_t lvglcj_fs_init_posix(const char *root);
int64_t lvglcj_font_load(const char *path);
int32_t lvglcj_font_delete(int64_t font);
int32_t lvglcj_obj_set_style_text_font(int64_t obj, int64_t font, int32_t sel);
int32_t lvglcj_image_set_src(int64_t img, const char *path);
```

### 5.11 调试 / 可观测性 ★修订 F4

```c
// 对象树 dump（结构化数组，非 JSON）
typedef struct {
    int64_t *handles;      // 输出数组
    int32_t *depths;       // 每个对象的深度
    char   **names;        // 对象名（可能为 NULL）
    int32_t  count;        // 实际数量
} lvglcj_tree_dump_t;

int32_t lvglcj_debug_dump_tree(int64_t root, int32_t max_depth,
                               lvglcj_tree_dump_t *out);
void    lvglcj_debug_free_tree_dump(lvglcj_tree_dump_t *dump);

// 内存监控
int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag,
                           uint32_t *max_used, uint32_t *free_size);

// 性能计数器
typedef struct {
    uint32_t fps;
    uint32_t cpu_percent;
    uint32_t refr_time_ms;
    uint32_t draw_time_ms;
    uint32_t obj_count;
} lvglcj_perf_t;
int32_t lvglcj_perf_sample(lvglcj_perf_t *out);

// 句柄表诊断
int32_t lvglcj_handle_count(int32_t state);
```

**为什么不用 JSON（修订 F4）**：仓颉标准库的 JSON 支持程度未知；返回结构化数组避免额外依赖，且仓颉侧遍历数组是零成本操作。

---

## 六、仓颉 API 设计

### 6.1 最小示例

```cangjie
import lvgl4cj.*
import lvgl4cj.core.*
import lvgl4cj.widgets.*

main() {
    let runtime = LvglRuntime()
    runtime.start()

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

### 6.2 样式 DSL

```cangjie
let cardStyle = LvStyle {
    bgColor(0xFF2A2D3E)
    bgOpa(255)
    bgGradColor(0xFF3A3D4E)
    bgGradDir(GradDir.Vertical)
    radius(12)
    padAll(16)
    borderWidth(0)
    shadowWidth(12)
    shadowColor(0x000000)
    shadowOpa(120)
    textFont(theme.fontBody)
    textColor(0xFFFFFF)
}

card.apply(cardStyle, Part.Main)
card.apply(pressedStyle, Part.Main | State.Pressed)
```

### 6.3 事件 DSL

```cangjie
// 基础
let h = btn.on(Event.Clicked) { e =>
    println("clicked")
}
h.remove()

// 同一事件码注册多个回调（全部触发，按注册顺序）
btn.on(Event.Clicked) { e => println("A") }
btn.on(Event.Clicked) { e => println("B") }   // A 和 B 都会执行

// 冒泡
container.addFlag(ObjFlag.EventBubble)
container.on(Event.Clicked) { e =>
    let origin = e.target
    let current = e.currentTarget
    println("来自 ${origin} 的事件冒泡到 ${current}")
}

container.on(Event.Clicked) { e =>
    if (shouldStop(e)) { e.stopBubbling() }
}
```

### 6.4 动画 DSL

```cangjie
LvAnim {
    target(btn)
    values(80, 160)
    duration(300)
    path(AnimPath.EaseOut)
    onExec { v => btn.setWidth(v) }
}.start()
```

### 6.5 Canvas

```cangjie
let canvas = LvCanvas.create(parent)
canvas.setBuffer(200, 200)
canvas.fillBg(0xFF1A1D2E)

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
        case QueueFull     => println("任务队列满，请降低投递频率")
        case _ => println(e.message)
    }
}
```

### 6.7 线程使用规范

```
✅ 正确：
   runtime.post { btn.setWidth(100) }

✅ 正确（已在 LVGL 线程，如事件回调内）：
   btn.setWidth(100)

❌ 错误：从业务线程直接调用
   btn.setWidth(100)      → 抛 WrongThread

❌ 错误：回调内同步等待
   btn.on(Event.Clicked) { e =>
       runtime.postAndWait { ... }           // → 抛 DeadlockRisk
   }
```

### 6.8 非目标用户可见声明

> **lvgl4cj 不提供**：
> - `lv_draw_*` 底层绘制 API（用 `LvCanvas` 替代）
> - `LV_EVENT_DRAW_*` 绘制事件
> - 自定义 `lv_fs` 驱动注册（MVP；P2 前补齐）
> - 32 位平台支持
> - MCU / RTOS 支持（见决策门 G1）

---

## 七、API 覆盖清单

### 7.1 控件清单（完整，分批交付）

| 批次   | 控件                                                         | 说明     |
| ------ | ------------------------------------------------------------ | -------- |
| **P0** | `obj`(base) `label` `button`                                 | 最小闭环 |
| **P1** | `slider` `switch` `checkbox` `image` `bar` `arc` `led` `line` `spinner` `dropdown` | 常用交互 |
| **P1** | `chart` `table` `roller` `textarea` `keyboard`               | 复合控件 |
| **P2** | `tabview` `menu` `msgbox` `win` `tileview` `scale` `calendar` `animimg` `span` `imgbtn` | 高级容器 |
| **P3** | `3dtexture` `flex/grid 布局对象` 其余                        | 按需     |

**验收口径**：P0 三个控件走通链路；P1 达到 18 个控件，覆盖 HMI 90% 场景；P2 补齐到 28 个。

> 注：v9 命名已变（`btn`→`button`、`img`→`image`、`scr`→`screen`、`del`→`delete`）。绑定层一律用 **v9 命名**，不提供 v8 别名。

### 7.2 样式属性覆盖

见 §5.7 表，共 **40+ 属性**，分 15 类。

### 7.3 可观测性 API

| 能力        | API                          | 用途                  |
| ----------- | ---------------------------- | --------------------- |
| 对象树 dump | `LvDebug.dumpTree()`         | 调试布局、泄漏分析    |
| 内存监控    | `LvDebug.memMonitor()`       | `lv_mem_monitor` 封装 |
| 性能采样    | `LvDebug.perfSample()`       | FPS / CPU / 重绘耗时  |
| 句柄统计    | `LvDebug.handleCount(state)` | **泄漏检测**          |
| 截图        | `LvDebug.screenshot(path)`   | SDL2 后端可用         |
| 对象计数    | `LvDebug.objCount()`         | 与句柄数对比          |

**`handleCount(ALIVE)` 与 LVGL 内部 `objCount` 长期应保持一致或差值恒定。差值持续增大 = 句柄泄漏。**

### 7.4 API 稳定性标记

```cangjie
@Experimental
public func setRotarySensitivity(v: Int32): Unit

@Deprecated(since: "0.3.0", use: "setColorFormat")
public func setColorDepth(v: Int32): Unit
```

规则：
- 新 API 默认 `@Experimental`，经过一个 minor 版本且无变更 → 转稳定
- 弃用 API 保留至少 2 个 minor 版本，编译期告警

---

## 八、后端适配

### 8.1 SDL2 后端详解 ★修订 E3

#### 8.1.1 组件

| SDL 对象       | 用途         |
| -------------- | ------------ |
| `SDL_Window`   | 宿主窗口     |
| `SDL_Renderer` | 渲染器       |
| `SDL_Texture`  | 像素缓冲载体 |
| `SDL_Event`    | 输入事件源   |

#### 8.1.2 线程关系（关键）★修订 E3

**SDL 事件循环必须在创建窗口的线程**（多数平台要求主线程）。而 LVGL 主循环在 C 侧 OS 线程。两者**必须分离**：

```
主线程（OS main thread）
  └─ SDL_Init / CreateWindow / CreateRenderer
  └─ while: SDL_PollEvent → 转成 LVGL 输入状态
     └─ 收到"渲染请求"事件 → SDL_RenderCopy + SDL_RenderPresent
        └─ 完成后置信号量

LVGL OS 线程
  └─ while: drain deferred → drain tasks → lv_timer_handler() → sleep 5ms
     └─ flush_cb: SDL_UpdateTexture
        └─ 投递"渲染请求"到主线程（SDL_PushEvent）
     └─ flush_wait_cb: 等待主线程完成信号量   ★ 关键
```

⚠️ **必须实现 `flush_wait_cb`（修订 E3）**：

v0.2 说"flush_cb 只做 SDL_UpdateTexture，提交渲染投递回主线程"——**但漏了同步**。LVGL 线程在 flush_cb 返回后认为缓冲可复用，会继续写下一帧。如果主线程还在从同一块纹理读（`SDL_RenderPresent` 未完成），**LVGL 就覆盖了正在被读的数据**。

**SDL2 后端的 `flush_wait_cb` 必须实现**：
- 主线程完成 `SDL_RenderPresent` 后置信号量
- `flush_wait_cb` 等待该信号量
- 否则 LVGL 线程会在主线程读取纹理期间覆盖缓冲

**这个约束必须在 P0 验证**，是 SDL2 后端最容易踩的坑。

#### 8.1.3 像素格式转换

| `lv_display` 格式 | SDL 纹理格式               |
| ----------------- | -------------------------- |
| `RGB565`          | `SDL_PIXELFORMAT_RGB565`   |
| `RGB888`          | `SDL_PIXELFORMAT_RGB888`   |
| `ARGB8888`        | `SDL_PIXELFORMAT_ARGB8888` |
| `XRGB8888`        | `SDL_PIXELFORMAT_XRGB8888` |

**必须一一对应**，否则花屏。RGB565 还要注意字节序。

#### 8.1.4 输入映射

| SDL 事件                            | LVGL indev          |
| ----------------------------------- | ------------------- |
| `SDL_MOUSEMOTION` / `BUTTONDOWN/UP` | pointer             |
| `SDL_MOUSEWHEEL`                    | encoder             |
| `SDL_KEYDOWN/UP`                    | keypad              |
| `SDL_TEXTINPUT`                     | textarea 输入       |
| `SDL_WINDOWEVENT_CLOSE`             | 触发 runtime.stop() |

#### 8.1.5 多窗口

MVP **仅支持单窗口**。多窗口需多个 `lv_display`，列入 P3。

#### 8.1.6 决策：手写 flush/read 而非用内置 SDL 驱动

LVGL v9 内置了 SDL 驱动（`LV_USE_SDL`）。**但 MVP 仍手写 flush/read 回调**，理由：

1. 内置驱动绕过 trampoline，**恰恰跳过了最需要验证的回调链路**
2. 手写能让我们控制线程关系（§8.1.2）
3. 内置驱动的行为在版本间可能变化

### 8.2 fbdev / DRM

| 后端    | 要点                                                         |
| ------- | ------------------------------------------------------------ |
| fbdev   | 打开 `/dev/fb0`，`mmap`，flush 直接 `memcpy`；需处理 stride 与像素格式 |
| DRM/GBM | `drmModeSetCrtc`、dumb buffer 或 GBM bo、双缓冲 + page flip；ARM64 HMI 主力路径 |

### 8.3 OpenHarmony 后端可行性分析

| 环节             | OH 机制                                                      | 约束与风险                                            |
| ---------------- | ------------------------------------------------------------ | ----------------------------------------------------- |
| **窗口**         | `NativeWindow` (OH_NativeWindow)                             | 需通过 NAPI 获取，仓颉能否直接调用 OH NDK？**待确认** |
| **渲染**         | `OH_NativeWindow_NativeWindowRequestBuffer` + 写 buffer + `FlushBuffer` | 与 LVGL flush_cb 模型契合                             |
| **Vsync**        | `OH_NativeVSync`                                             | 可用 Vsync 驱动 `lv_timer_handler`                    |
| **输入**         | `OH_Input` 多模输入 / ArkTS 侧事件                           | **最大不确定性**                                      |
| **NAPI**         | OH 原生扩展机制                                              | 仓颉与 OH NAPI 的互操作能力**需实测**                 |
| **ArkTS 互操作** | 若仓颉 UI 嵌在 ArkTS 页面内                                  | 需仓颉 ⇄ ArkTS 桥，官方支持程度未知                   |

**结论：OH 后端的风险不在 LVGL，在仓颉与 OH 系统服务的互操作能力。** 列为决策门 G2。

---

## 九、构建系统与工程化

### 9.1 cjpm 与 CMake 协作 ★修订 G1

```
构建顺序（必须串行）：
  1. CMake 构建 LVGL → liblvgl.a
  2. CMake 构建桥接层 → liblvgl4cj_bridge.a/.so
  3. 运行 probe_conf → conf.json
  4. python gen_conf_const.py → src/generated/conf_const.cj
  5. cjpm build → 链接 1、2 → 可执行文件
```

**构建前置依赖（修订 G1）**：**Python 3.8+**。写入 README。

**替代路径**：目标环境无法装 Python 时，用纯 CMake `configure_file` 生成 `.cj`（牺牲灵活性，无 Python 依赖）。

`cjpm.toml` 关键配置：

```toml
[package]
  cjc-version = "1.1.0"
  name = "lvgl4cj"
  version = "0.1.0"
  output-type = "static"
  src-dir = "src"

[ffi.c]
  path  = ["native/include"]
  clink = ["lvgl4cj_bridge", "lvgl"]
```

⚠️ **P0 第一步应照抄 `CJQT6/cjpm.toml`**（唯一被验证过的模板）。

### 9.2 符号可见性与库形态

```cmake
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
```

| 形态               | 何时用     |
| ------------------ | ---------- |
| **静态库**（默认） | 嵌入式部署 |
| 动态库             | 桌面开发   |

### 9.3 交叉编译与平台矩阵

| 平台         | 架构   | 工具链                | 用途           | CI   |
| ------------ | ------ | --------------------- | -------------- | ---- |
| Ubuntu 22.04 | x86_64 | GCC 11+               | 主开发         | ✅    |
| Ubuntu 22.04 | ARM64  | aarch64-linux-gnu-gcc | 交叉验证       | ✅    |
| macOS 13+    | arm64  | Clang                 | 开发验证       | ✅    |
| Windows 11   | x86_64 | MSVC 2022             | 可选           | ⚠️ P2 |
| OpenHarmony  | ARM64  | OH SDK                | 二期，过 G2 后 | ❌    |

**headless 运行**（CI 必需）：`backend_null`，flush_cb 空操作，indev 无输入。

```
CI 矩阵 = {ubuntu-x64, ubuntu-arm64, macos-arm64} × {debug+ASan, release}
```

### 9.4 平台差异清单

| 差异                              | 处理                        |
| --------------------------------- | --------------------------- |
| `gettid` vs `pthread_threadid_np` | `thread.c` 内条件编译       |
| SDL2 版本差异                     | 固定最低 2.0.20+            |
| 字节序                            | 颜色格式转换处条件处理      |
| 对齐要求                          | 一律走 `lv_draw_buf_*` 算子 |

### 9.5 ASan 与仓颉 GC 兼容性 ★修订 G5

**隔离策略**：

1. **分层启用**：C 桥接层 + LVGL 用 ASan 编译；仓颉侧不启用
2. **suppression 文件**：`scripts/asan_suppressions.txt`，屏蔽仓颉运行时符号

⚠️ **符号命名需实证确认（修订 G5）**：仓颉运行时符号未必以 `cangjie*` / `CJ*` 开头。**P0 阶段必须用 `nm` 检查实际符号，补充 suppression 规则**，否则 CI 上线后才发现屏蔽不全。

3. **验证 ASan 有效性**：先人为制造一个 C 侧泄漏，确认 ASan 能报出
4. **备选**：若 ASan 与仓颉 GC 根本冲突，退回 `valgrind --tool=memcheck` 或 LVGL 内置内存检查

---

## 十、测试策略与 CI

### 10.1 测试分层

| 层级     | 框架             | 内容                                | CI   |
| -------- | ---------------- | ----------------------------------- | ---- |
| C 侧单测 | 自写断言 + CTest | 句柄表四态、状态机转移、延迟队列    | ✅    |
| FFI 契约 | 仓颉单测         | 结构体 offset/padding、枚举值一致性 | ✅    |
| 仓颉单测 | 仓颉测试框架     | 句柄、回调、事件、动画语义          | ✅    |
| 集成测试 | headless 后端    | 建 UI 树 → 模拟输入 → 断言状态      | ✅    |
| 视觉回归 | SDL2 + 截图 diff | P2                                  | ⚠️    |
| Soak     | 24h 长跑         | 内存/句柄/稳定性                    | 夜间 |
| 模糊测试 | P2               | 随机 UI 操作序列                    | 夜间 |
| 性能基准 | `scripts/bench`  | 帧率/延迟/内存                      | 夜间 |

### 10.2 FFI 契约测试 ★修订 F6

```cangjie
@Test
func testStructLayout(): Unit {
    // ★ 所有与 C 交互的结构体必须用 @C 声明
    //   否则仓颉编译器可能重排字段，与 C 不一致
    let cOffsets = lvglcj_probe_area_offsets()
    let cjOffsets = areaOffsetsFromCangjie()
    @Assert(cOffsets == cjOffsets)
}
```

**前置要求（修订 F6）**：`lv_area_t` / `lv_point_t` 等与 C 交互的结构体**必须用 `@C` 声明**。测试同时验证注解存在，否则布局不保证一致。

### 10.3 关键测试用例清单

| 用例             | 断言                                                         |
| ---------------- | ------------------------------------------------------------ |
| `handle_test`    | 四态转移正确；`close()` 重复调用安全；`release()` 幂等       |
| `callback_test`  | 注册→触发→注销；对象删除时闭包自动清理；回调抛异常被吞并记录 |
| `event_test`     | `target` ≠ `current_target`；`stopBubbling` 生效；多回调按序全部触发 |
| `thread_test`    | **亲和性探针**（§3.6.4）；跨线程调用抛 `WrongThread`         |
| `anim_test`      | 动画执行、停止、对象删除时清理                               |
| `deferred_test`  | 回调中 `close()` 不崩，**同帧内**删除生效                    |
| `lifecycle_test` | 父删→子句柄失效；`lv_obj_clean` 后父仍存活                   |
| `leak_test`      | 创建 1 万对象再删除，`handleCount(ALIVE)` 与 `objCount` 差值恒定 |
| `soak_test`      | 24h：RSS 收敛、句柄收敛、无崩溃                              |

### 10.4 CI 流水线

```yaml
jobs:
  native:
    matrix: [ubuntu-x64, ubuntu-arm64, macos-arm64]
    steps: [cmake build lvgl + bridge, ctest]
  probe:
  cjpm:
    steps: [cjfmt --check, cjlint, cjpm build, cjpm test]
  asan:
  soak:
```

**门禁**：`cjfmt` 零 diff、`cjlint` 零告警、单测全绿、ASan 无报告。

### 10.5 模糊测试（P2）

随机生成 UI 操作序列（创建/删除/改属性/触发事件/hard delete），断言不崩溃且句柄无泄漏。

---

## 十一、量化验收指标 ★修订 D2

### 11.1 P0 门禁指标（不过不放行）

| 指标               | 门槛                                                  | 测量方式 |
| ------------------ | ----------------------------------------------------- | -------- |
| **线程亲和性**     | 探针测试 OS 线程 ID 变化次数 = 0，或已确认走方案 A    | §3.6.4   |
| **句柄泄漏**       | 1 万次 create/delete 后 `handleCount(ALIVE)` 增量 = 0 | §7.3     |
| **句柄与对象一致** | `handleCount(ALIVE) - objCount` 恒定                  | soak     |
| **回调异常**       | 回调抛异常不导致进程崩溃，错误回调被调用              | 单测     |
| **延迟删除**       | 回调中 `close()` 自身不崩溃，**同帧内**对象确实删除   | 单测     |
| **ASan**           | 零报告（且已验证 ASan 工具本身有效）                  | CI       |

### 11.2 P1 性能指标 ★修订 D2

**⚠️ 性能指标分两档，取决于 V1 结论。不许用方案 C 的实现去对方案 A 的指标。**

#### 方案 A（C 侧 OS 线程，推荐）

| 指标              | 门槛                              | 测量条件                    |
| ----------------- | --------------------------------- | --------------------------- |
| 帧率              | ≥ 30 FPS（800×480，局部刷新）     | SDL2，~50 对象              |
| 帧率（全屏动画）  | ≥ 20 FPS                          | 800×480 全屏过渡            |
| 输入延迟          | ≤ 50 ms（点击到回调触发）         | 事件时间戳差                |
| 单次 FFI 调用开销 | ≤ 2 μs                            | 10 万次 `lv_obj_set_x` 均值 |
| 事件回调延迟      | ≤ 200 μs（C 触发 → 仓颉闭包执行） | 探针计时                    |
| 启动时间          | ≤ 500 ms（`lv_init` 到首帧）      | 冷启动测量                  |

#### 方案 C（泵模式，回退）

⚠️ **方案 C 下 UI 与业务抢同一线程**。以下门槛是**"非实时 HMI 可接受"的降级标准**：

| 指标              | 门槛                   | 说明                   |
| ----------------- | ---------------------- | ---------------------- |
| 帧率              | ≥ 15 FPS（业务空闲时） | 业务阻塞时帧率可能归零 |
| 输入延迟          | ≤ 150 ms               | 依赖业务是否阻塞       |
| 单次 FFI 调用开销 | ≤ 2 μs                 | 同上                   |
| 启动时间          | ≤ 500 ms               | 同上                   |

**方案 C 下禁止宣传"实时 HMI"**。产品定位需相应调整。

### 11.3 内存指标

| 指标        | 门槛                     |
| ----------- | ------------------------ |
| RSS 增长    | 24h ≤ 10 MB              |
| LVGL 内存池 | 峰值 ≤ 配置值的 80%      |
| 句柄表大小  | 稳态后不再增长           |
| 闭包表大小  | 对象删除后归零（分支 B） |

### 11.4 稳定性指标

| 指标                   | 门槛                           |
| ---------------------- | ------------------------------ |
| 24h 连续运行           | 零崩溃、零 ASan 报告、内存收敛 |
| 100 万次事件触发       | 无崩溃、无句柄泄漏             |
| 随机操作模糊测试（2h） | 无崩溃                         |

### 11.5 基线记录要求

**所有指标必须在 `docs/benchmarks/` 记录**：日期、平台、LVGL 版本、仓颉 SDK 版本、硬件配置、原始数据。**没有基线，优化无从谈起。**

---

## 十二、MVP 路线 ★修订 G3

### 阶段 P0：跑通闭环 + 排掉阻塞性不确定项

#### 第 1 周：阻塞项清零（**不写业务代码**）

| #    | 任务                                   | 产出                            |
| ---- | -------------------------------------- | ------------------------------- |
| V1   | 仓颉外部 OS 线程能否执行闭包（§3.6.3） | 方案 A/B/C 选型结论             |
| V2   | 仓颉 FFI 回调是否支持捕获（§3.2.1）    | 分支 A/B 选型结论               |
| V3   | 亲和性探针测试（§3.6.4）               | 实测数据                        |
| V4   | ASan 与仓颉 GC 兼容性（§9.5）          | 内存检测方案 + suppression 规则 |
| V5   | cjpm `[ffi.c]` 字段可用性              | 照抄 CJQT6 模板验证             |

⚠️ **这五项不确定，P0 后续工作无法正确设计。**

#### P0 任务依赖图（修订 G3）

```
V1（线程方案）───────────┬──► 任务1（主循环）──► 任务2（display+indev）──► 任务3（对象树）──► 任务4（事件+回调）──► 任务5（样式+布局）
                         │                                                                                        │
V2（回调方案）───────────┘                                                                                        │
                                                                                                                  │
V3（亲和性探针）───────────────► 任务1 的验证                                                                      │
V4（ASan）────────────────────► 任务3/4 的内存验证                                                                │
V5（cjpm）────────────────────► 任务2 的构建验证                                                                  ▼
                                                                                                          P0 压力测试
                                                                                                     button → clicked → 删除自己 → 重建
```

**关键路径**：V1/V2 → 任务1 → 任务2 → 任务3 → 任务4 → 任务5 → 压力测试。

**V1/V2 不出结论，任务 1 不能开工。**

#### 第 2-4 周（闭环）

| #    | 任务                                         | 验收                       | 依赖   |
| ---- | -------------------------------------------- | -------------------------- | ------ |
| 1    | `lv_init` / tick / `lv_timer_handler` 主循环 | 稳定刷帧                   | V1     |
| 2    | SDL2 display flush + pointer indev           | 按钮可点，坐标正确，非花屏 | V1, V5 |
| 3    | 对象树 + 四态句柄 + 级联失效                 | 父删→子失效，无悬空        | V4     |
| 4    | 事件回调 + timer 回调                        | 注册/注销/重入安全         | V2, V4 |
| 5    | 样式最小集 + Flex 布局                       | 布局正确                   | —      |

**P0 压力测试**：`button → clicked → 删除自己 → 重建`。跑不稳就不许加控件。

### 阶段 P1：可用

- 18 个控件（§7.1）
- 样式补齐到 40+ 属性；动画 API；Group 与键盘导航
- 延迟删除队列、冒泡语义、错误模型
- 四平台 CI + ASan；`examples/widgets_demo`、`anim_demo`
- 量化指标达标（§11.2/11.3）
- **FreeType 许可评估（修订 G2）**

### 阶段 P2：工程化

- 绑定生成器（setter/getter 自动生成）
- ARM64 Linux 交叉编译 + DRM 后端
- 控件补齐到 28 个；Canvas；fs/字体/图片
- **自定义 fs 驱动（§3.13.1）**
- 模糊测试、视觉回归、性能基线入库
- `examples/hmi_panel`

### 阶段 P3：外延（需过决策门）

- OpenHarmony 后端（**先过 G2**）
- MCU / RTOS（**先过 G1**）
- `LV_EVENT_DRAW_*` 评估
- 32 位平台

---

## 十三、风险登记册

| ID   | 风险                               | 等级 | 应对                           | **Owner**    | **触发条件**         | **评估时间点** |
| ---- | ---------------------------------- | ---- | ------------------------------ | ------------ | -------------------- | -------------- |
| R1   | **仓颉无法在外部 OS 线程执行闭包** | 高   | 退回泵模式（方案 C）           | 架构负责人   | V1 结论为否          | **P0 第 1 周** |
| R2   | 仓颉轻量级线程 M:N 迁移            | 高   | 强制方案 A + 线程断言          | 架构负责人   | 探针 OS TID 变化 > 0 | **P0 第 1 周** |
| R3   | ASan 与仓颉 GC 冲突                | 中   | 用 valgrind / LVGL 内置检查    | 测试负责人   | ASan 误报或崩溃      | **P0 第 1 周** |
| R4   | 回调内异常跨越 C 边界导致崩溃      | 高   | trampoline 捕获 + 全局错误回调 | 桥接层负责人 | 单测失败             | P0 第 3 周     |
| R5   | 绘制缓冲被 GC 移动 → 花屏          | 高   | C 侧对齐分配（§3.4）           | 桥接层负责人 | 出现间歇花屏         | P0 第 2 周     |
| R6   | 父删子导致悬空句柄                 | 高   | DELETE 钩子级联失效            | 桥接层负责人 | 单测失败             | P0 第 3 周     |
| R7   | 回调中删除自身导致重入崩溃         | 高   | 延迟删除队列（§3.8）           | 桥接层负责人 | 压力测试失败         | P0 第 4 周     |
| R8   | `lv_conf` 不匹配导致隐蔽 bug       | 中   | 哈希校验（§3.5）               | 构建负责人   | 换机器构建失败       | P0 第 2 周     |
| R9   | LVGL 版本升级破坏 API              | 中   | 版本锁定 + 独立升级分支        | 维护者       | —                    | 每季度         |
| R10  | 控件 API 量大手写不完              | 中   | 生成器 + 分批交付              | API 负责人   | 进度落后 2 周        | P1 中期        |
| R11  | **仓颉运行时不适合 MCU**           | 高   | 见决策门 G1                    | 架构负责人   | 基准测试不达标       | **P2 完成后**  |
| R12  | OH 系统服务互操作不通              | 高   | 见决策门 G2；独立探针          | OH 负责人    | 探针失败             | **P2 完成后**  |
| R13  | 32 位平台 `void*` 截断             | 中   | MVP 仅 64 位；二期独立句柄表   | 架构负责人   | 出现 32 位需求       | 需求出现时     |
| R14  | LVGL 许可与 SBOM 不合规            | 中   | 许可清单前置审查（§14.1）      | 合规负责人   | 入库前审查           | 发布前         |
| R15  | **FreeType GPL 许可阻碍中文 HMI**  | 中   | 提前到 P1 前评估（§14.1）      | 合规负责人   | P1 中期字体需求落地  | **P1 前**      |

### 决策门

#### G1：是否进入 MCU / RTOS（P2 后）

| 测量项                   | 门槛                     |
| ------------------------ | ------------------------ |
| 仓颉运行时静态 footprint | ≤ 目标板 Flash 的 50%    |
| 堆峰值                   | ≤ 目标板 RAM 的 50%      |
| **GC 停顿**              | **≤ 16 ms**              |
| 启动时间                 | 满足产品冷启动要求       |
| 静态链接可行性           | 无包管理环境能否部署     |
| libc / OS 依赖           | 裸机或最小 RTOS 能否运行 |

**任一关键项不达标 → 不做 MCU 版本。** 替代方案：**C 主控渲染 + 仓颉跑业务逻辑**。

⚠️ LVGL 官方最低资源是 16 KB RAM / 64 KB Flash 级，那是 **LVGL 核心**的口径；仓颉运行时带 GC、线程、标准库，完整进程占用**完全不是一个量级**。

#### G2：是否启动 OH 后端（P2 后）

三项探针全部通过才排期：
1. 仓颉能调 OH NDK（NativeWindow / NativeVSync）
2. 仓颉能接收 OH 输入事件
3. 仓颉与 ArkTS 互操作路径明确

---

## 十四、许可、治理与发布

### 14.1 许可 ★修订 G2

| 组件              | 许可                              | 备注                                     |
| ----------------- | --------------------------------- | ---------------------------------------- |
| **LVGL 主线**     | **MIT**（v8/v9 已改）             | ⚠️ 必须锁定 commit 并核对 `LICENCE.txt`   |
| lvgl4cj 绑定层    | **Apache-2.0**                    | 与 LVGL 分离                             |
| **SDL2**          | zlib / MIT 或专有                 | 按版本核对                               |
| **libpng**        | PNG Reference Library (zlib-like) | 若启用 PNG                               |
| **libjpeg-turbo** | IJG / BSD-3 / Zlib 三选一         | 若启用 JPG                               |
| **FreeType**      | **GPLv2 或 FTL（双许可）**        | ⚠️ **GPL 风险，P1 前必须评估**（修订 G2） |
| 内置字体          | 多为 SIL OFL / Apache             | 逐个核对                                 |
| ThorVG（矢量）    | MIT                               | 若启用                                   |

**行动项**：
- `third_party/lvgl` 保留原始 `LICENCE.txt` + 版本哈希
- CI 生成 **SPDX + CycloneDX** 双格式 SBOM
- **FreeType 的 GPL 选项需在 P1 前专项评估**：用 FTL 双许可可规避 GPL；若无法规避，中文 HMI 需换字体方案

### 14.2 版本与分支

- **语义化版本** `MAJOR.MINOR.PATCH`
- 分支模型：`main` / `develop` / `feature/*` / `release/*`
- `CHANGELOG.md` 按 [Keep a Changelog] 格式
- PR 模板：变更描述 / 关联 issue / 测试证据 / 许可影响
- 提交规范：Conventional Commits

### 14.3 制品与发布

| 制品                  | 职责                              |
| --------------------- | --------------------------------- |
| `lvgl4cj-core`        | 仓颉安全 API、句柄、生命周期      |
| `lvgl4cj-sys`         | C ABI、LVGL 版本锁定、native 构建 |
| `lvgl4cj-backend-sdl` | Linux 桌面后端                    |
| `lvgl4cj-backend-drm` | ARM64 HMI（P2）                   |

准入路径：**独立原型 → SIG 孵化 → TPC → 中心仓制品**。

### 14.4 用户文档规划

| 类型             | 内容                                           |
| ---------------- | ---------------------------------------------- |
| 快速开始         | 10 分钟跑通 `hello_cj`                         |
| 教程             | 控件、布局、样式、动画、事件、Canvas           |
| **线程指南**     | §6.7 规范，独立成章                            |
| **生命周期指南** | 句柄四态、延迟删除语义                         |
| **动画语义指南** | `setVar` 与 `onExec` 的对象引用关系（§3.10.3） |
| API 参考         | 由源码注释生成                                 |
| 迁移指南         | LVGL 版本升级、绑定层版本升级                  |
| 故障排查         | 花屏/崩溃/泄漏 的定位手册                      |
| **非目标清单**   | §1.5 独立成页                                  |

---

## 十五、与现有绑定对比

| 绑定                       | 语言        | 方案                               | 可借鉴                                                       |
| -------------------------- | ----------- | ---------------------------------- | ------------------------------------------------------------ |
| **lv_binding_rust**        | Rust        | `bindgen` 生成 sys 层 + 手写安全层 | ★★★ **分层方式（sys / safe）与本方案 L1/L2 一致**            |
| **lv_binding_micropython** | MicroPython | C 模块 + 对象映射                  | ★★ 回调转 Python callable 的桥接                             |
| **lvgl-js**                | JS          | JerryScript / QuickJS 绑定         | ★ 轻量 VM 上的对象管理                                       |
| **LVGL 官方 C++ 绑定**     | C++         | RAII 包装                          | ★★ 对象生命周期 RAII 思路                                    |
| **CJQT6**                  | 仓颉        | 三层 C ABI 桥接 Qt6                | ★★★ **同语言先例**：工程结构、`close()` 语义、枚举 `.value` 约定 |

**三条关键借鉴**：

1. **`sys` / `safe` 分层**（来自 Rust 绑定）—— 与本方案 L1/L2 完全对应
2. **不追求自动生成全量 API**（Rust 绑定也是 bindgen + 大量手写）
3. **同语言的 CJQT6 经验**——`Resource` + `close()`、禁用终结器、回调用顶层 `@C func`（或 Capture 变体）

**一个差异**：Rust 绑定面临的所有权问题（borrow checker vs LVGL 对象树）在仓颉不存在（GC 语言），但换来的是"GC 可能移动/延迟释放"的新问题——这正是 §3.4 和 §3.1 要解决的。

---

## 十六、架构决策记录（ADR 摘要）★修订 D3

完整 ADR 存 `docs/adr/`，格式：背景 / 备选 / 决策 / 后果。

| ID          | 决策                                                         | 关键理由                                          |
| ----------- | ------------------------------------------------------------ | ------------------------------------------------- |
| **ADR-001** | 用句柄表（自增 ID）而非裸指针或指针地址作句柄                | 地址复用会导致"误判存活"；ID 可携带状态与调试信息 |
| **ADR-002** | 绘制缓冲在 C 侧分配                                          | GC 可能移动/回收仓颉数组                          |
| **ADR-003** | 单 LVGL OS 线程 + 任务队列                                   | LVGL 非线程安全；仓颉线程可能 M:N 迁移            |
| **ADR-004** | **LVGL 主循环必须运行在 OS 线程，不得依赖仓颉线程语义**（已确认原则，不依赖 V1） | 这是 LVGL 的硬约束                                |
| **ADR-005** | 不暴露 `lv_draw_*`，用 Canvas                                | v9 draw unit 模型 FFI 成本极高                    |
| **ADR-006** | 显式 `close()`，禁用终结器                                   | GC 时机不确定                                     |
| **ADR-007** | 延迟删除队列                                                 | 回调中同步删除会导致事件链 use-after-free         |
| **ADR-008** | `lv_conf` 哈希校验                                           | 把"莫名花屏/崩溃"前置为"启动一条明确报错"         |
| **ADR-009** | MVP 仅 64 位                                                 | 32 位 `void*` 无法承载 int64 句柄/闭包 ID         |
| **ADR-010** | 锁定 LVGL v9.x，不提供 v8 别名                               | 两套命名并存会长期增加维护成本                    |
| **ADR-011** | 静态库为默认形态                                             | 嵌入式部署无动态链接器                            |
| **ADR-012** | 手写 SDL2 flush/read 而非用内置驱动                          | 内置驱动绕过 trampoline                           |
| **ADR-013** | **【Pending】具体采用方案 A（C 侧 OS 线程）还是方案 C（泵模式），由 V1 结论决定** | V1 出结论后立即归档为正式 ADR                     |

**ADR-004 vs ADR-013 的区别（修订 D3）**：
- ADR-004 是**已确认的原则**：LVGL 主循环必须在 OS 线程（无论 A 还是 C 都满足这条——方案 C 的泵调用也在 OS 线程上）
- ADR-013 是**待定决策**：在满足 ADR-004 的前提下，是"独立 OS 线程"（A）还是"复用主线程泵"（C）
- V1 出结论后，ADR-013 立即归档，不再是 Pending

---

## 十七、第一个可交付物

**两周目标不是"框架"，是这一张图 + 六个断言：**

```
examples/hello_cj/
  → SDL2 窗口 800×480，深色背景，中间一个按钮
  → 点击按钮，文字从 "点我" 变成 "点了 N 次"
  → 窗口不崩，退出时干净释放
```

**六个断言**（= §11.1 P0 门禁指标，合并修订 H3）：

1. `lv_init` → `lv_timer_handler()` 循环稳定跑 10 分钟不崩
2. flush 回调被调用，画面正确（非花屏/非黑屏）
3. 点击命中按钮，事件回调触发，坐标正确
4. 删除父容器后，子对象句柄 `isAlive() == false`
5. 退出时 ASan 报告零泄漏
6. **回调中 `close()` 自身不崩溃，同帧内对象确实被删除**

配套前置：§12 的 V1–V5 五项不确定项结论。

**这六条过了，项目成立；过不了，先修设计，不要往前堆控件。**

---

## 附录 A：术语表

| 术语                   | 含义                                                         |
| ---------------------- | ------------------------------------------------------------ |
| **L1 / L2 / L3 / L4**  | 仓颉 API 层 / C 桥接层 / LVGL 原生层 / 后端层                |
| **句柄（handle）**     | `Int64` 自增 ID，间接引用原生对象；非指针地址                |
| **四态**               | `UNINIT` / `ALIVE` / `INVALIDATED` / `RELEASED`（§3.1）      |
| **trampoline**         | 固定 C 函数，作为 LVGL 回调入口，内部转发到仓颉闭包          |
| **closure_id**         | 仓颉闭包在 C 侧闭包表中的 `int32` 标识（分支 B）             |
| **延迟删除**           | 回调执行期间不立即删除，入队，同帧内 drain 完成（§3.8）      |
| **selector**           | v9 样式机制：`Part | State` 组合                             |
| **flush_cb**           | 显示刷新回调                                                 |
| **read_cb**            | 输入设备读取回调                                             |
| **stride**             | 每行像素字节数（含对齐填充），≠ `width × bpp`                |
| **OS 线程 / 仓颉线程** | pthread 级真实线程 / 仓颉运行时管理的轻量级线程（M:N）       |
| **G1 / G2**            | 决策门：MCU 准入 / OH 后端准入                               |
| **V1–V5**              | P0 第 1 周待确认项：线程方案 / 回调方案 / 亲和性 / ASan / cjpm |

## 附录 B：错误码表 ★修订 F1

| 码      | 名称                | 含义                                            | 仓颉异常                          |
| ------- | ------------------- | ----------------------------------------------- | --------------------------------- |
| 0       | `OK`                | 成功                                            | —                                 |
| **+1**  | **`OK_DEFERRED`**   | **成功但已延迟（非错误）**（修订 F1：改为正数） | 无异常                            |
| -1      | `INVALID_HANDLE`    | 句柄失效                                        | `LvglException(InvalidHandle)`    |
| -2      | `NOT_INITIALIZED`   | 未调用 `lv_init`                                | `LvglException(NotInitialized)`   |
| -3      | `INVALID_CONFIG`    | `lv_conf` 哈希不匹配                            | `LvglException(InvalidConfig)`    |
| -4      | `CALLBACK_THREW`    | 仓颉回调抛异常（已吞掉）                        | 触发 `onError`                    |
| -5      | `OUT_OF_MEMORY`     | LVGL 池耗尽或 malloc 失败                       | `LvglException(OutOfMemory)`      |
| -6      | `WRONG_THREAD`      | 跨线程调用 LVGL API                             | `LvglException(WrongThread)`      |
| -7      | `DEADLOCK_RISK`     | 回调内同步等待                                  | `LvglException(DeadlockRisk)`     |
| -8      | `QUEUE_FULL`        | 任务队列满                                      | `LvglException(QueueFull)`        |
| -9      | `BACKEND_FAILURE`   | 后端失败                                        | `LvglException(BackendFailure)`   |
| -10     | `INVALID_ARGUMENT`  | 参数非法                                        | `LvglException(InvalidArgument)`  |
| -11     | `CLOSURE_EXHAUSTED` | 闭包 ID 耗尽                                    | `LvglException(ClosureExhausted)` |
| -12     | `PENDING_DELETE`    | 对象处于待删除状态                              | `LvglException(PendingDelete)`    |
| -13     | `NOT_SUPPORTED`     | 该 API 在当前构建配置下不可用                   | `LvglException(NotSupported)`     |
| -14     | `VERSION_MISMATCH`  | LVGL 版本不匹配                                 | `LvglException(VersionMismatch)`  |
| **-15** | **`DEFERRED_LOOP`** | **延迟删除循环超限（新增）**                    | `LvglException(DeferredLoop)`     |

**约定（修订 F1）**：
- **0 = 成功**
- **负数 = 错误**
- **正数 = 特殊成功标记**（目前仅 `OK_DEFERRED = 1`）

---

## 附录 C：配置项表（`lv_conf.h` 关键项）

| 配置项                     | 建议值                   | 影响                             |
| -------------------------- | ------------------------ | -------------------------------- |
| `LV_COLOR_DEPTH`           | 16 或 32                 | 颜色精度                         |
| `LV_USE_LOG`               | 1（Debug）/ 0（Release） | 是否转发日志到仓颉               |
| `LV_MEM_SIZE`              | ≥ 64 KB                  | LVGL 内存池                      |
| `LV_DRAW_BUF_STRIDE_ALIGN` | 由 LVGL 默认             | ⚠️ 缓冲大小计算必须考虑（§3.4.2） |
| `LV_USE_PROFILER`          | 1                        | 性能计数器（§5.11）              |
| `LV_USE_MEM_MONITOR`       | 1                        | 内存监控                         |
| `LV_USE_ASSERT_*`          | Debug 开                 | 提前暴露问题                     |
| `LV_USE_FS_POSIX`          | 1                        | 文件系统（§3.13.1）              |
| `LV_USE_PNG/JPG`           | 按需                     | ⚠️ 引入新依赖，需入 SBOM          |
| `LV_FONT_DEFAULT`          | 内置或外部               | 字体体积影响 Flash               |
| `LV_USE_SDL`               | 0（MVP 手写）            | 见 §8.1.6                        |
| `LV_DEF_REFR_PERIOD`       | 33 ms（~30 FPS）         | 刷新周期                         |

**所有改动必须同步更新 `conf_const.cj` 并触发重新构建**（§3.5.2）。

---

## 附录 D：P0 速查卡（新增 H1）

**这一页是 P0 参与者的唯一入口。**

### D.1 第 1 周必须回答的 5 个问题

| #      | 问题                                    | 命令 / 方法                                      | 结论填写                             |
| ------ | --------------------------------------- | ------------------------------------------------ | ------------------------------------ |
| **V1** | 仓颉能否在 C 创建的 OS 线程里执行闭包？ | 查 SDK 文档 / 问社区 / 写探针                    | ⬜ A 可行 / ⬜ B 不可行 → 用方案 C     |
| **V2** | 仓颉 FFI 回调能否捕获局部变量？         | 写最小例子：`obj.on(...) { e => useLocalVar() }` | ⬜ 支持（分支 A）/ ⬜ 不支持（分支 B） |
| **V3** | 仓颉轻量级线程会迁移 OS 线程吗？        | 跑 §3.6.4 探针，打印不同 OS TID 数               | ⬜ = 1 / ⬜ > 1                        |
| **V4** | ASan 与仓颉 GC 能共存吗？               | 人为制造 C 侧泄漏，看 ASan 是否报出              | ⬜ 能 / ⬜ 不能 → 用 valgrind          |
| **V5** | `cjpm.toml` 的 `[ffi.c]` 字段可用吗？   | 照抄 CJQT6 模板编译                              | ⬜ 可用 / ⬜ 需调整                    |

### D.2 必读三章

| 章节              | 为什么必读                        |
| ----------------- | --------------------------------- |
| **§3.6 线程模型** | 方案 A/C 选型直接决定代码结构     |
| **§3.8 回调重入** | 延迟删除是 P0 压力测试的核心      |
| **§3.4 绘制缓冲** | 缓冲所有权是花屏/段错误的最大来源 |

### D.3 第一个可交付物

见 §17：`examples/hello_cj` + 六个断言。

### D.4 P0 压力测试

```
button → clicked → 删除自己 → 重建
```

跑不稳就不许加控件。

### D.5 禁止事项

| ❌ 禁止                              | 原因             |
| ----------------------------------- | ---------------- |
| V1/V2 出结论前写 §3.2 / §3.6 的代码 | 设计可能整体重写 |
| 把仓颉 `Array<UInt8>` 指针传给 LVGL | GC 移动导致花屏  |
| 回调内同步等待 `post`               | 死锁             |
| 回调内直接删对象不走延迟队列        | use-after-free   |
| Release 构建关闭线程检查            | 间歇性数据竞争   |

---

**文档结束**

> v0.3 相对 v0.2 的核心变化：**把"设计已承诺但假设未验证"的结构性张力显式化**（D 类）、**修掉三处二阶交互洞**（E 类）、**补齐 API 与工程化细节**（F/G/H 类）。
>
> 最重要的立场：**§3.2 / §3.6 / §3.8 / §3.9 的代码在 V1/V2 出结论前不动工**。这不是保守，而是避免"看起来跑通但埋下间歇性崩溃"。