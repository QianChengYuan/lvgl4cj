# lvgl4cj 设计方案

## 仓颉语言绑定 LVGL 的 GUI 框架

> 版本：**v0.4 冻结版**（含 Patch 1 + Patch 2 + 次生残留处置）
> 目标 LVGL 版本：**v9.x**（主线，锁 9.2+）
> 目标仓颉 SDK：**1.1.0**
> 定位：面向 **Linux / ARM64 Linux / OpenHarmony 的嵌入式 HMI**，桌面 SDL2 作为开发与验证环境

---

## 修订说明：v0.4 冻结版

### 前提纠正（延续 v0.4）

v0.2 从 CJQT6 的 `setOnTimeoutCapture` 推出"仓颉可能原生支持捕获回调"，方向是**反的**。`Capture` 变体的存在恰恰暗示仓颉默认不能捕获，CJQT6 必须自建闭包表。

**结论**：
- **分支 B（自建闭包表 + closure_id）为默认设计**
- 分支 A 降为备选，**且必须满足 GC 保活前提**

### v0.4 一等修订（I 类）

| #    | 问题                                          | 修订位置                              |
| ---- | --------------------------------------------- | ------------------------------------- |
| I1   | 分支 A 遗漏 GC 保活；错误声称"不需要注销闭包" | **§3.2 重写**                         |
| I2   | anim var 二元矛盾                             | **§3.10 重写**：改为 `var = anim_ctx` |
| I3   | flush_wait_cb 引入新死锁路径                  | **§8.1.2 + §3.8.4**                   |
| I4   | `lv_obj_clean` 判断方法不可实现               | **§3.3**                              |

### 冻结版 Patch（针对 v0.4 的一等缺陷）

| #      | 问题                                                         | 修订位置                                                     |
| ------ | ------------------------------------------------------------ | ------------------------------------------------------------ |
| **P1** | **anim_ctx 泄漏（I2 引入）**：动画自然结束时 ctx 谁释放？    | **§3.10 增补**：内部 deleted_cb 自动挂 + 用户回调包装 + 探针 V7 |
| **P2** | **pin 表代码示例误导（I1 引入）**：C 全局数组不在 GC 根集，不构成保活 | **§3.2.4 改写**：明确警告 + 边界检查 + 采用条件收紧          |

### 次生残留处置

| #    | 处置                                                         |
| ---- | ------------------------------------------------------------ |
| 1    | J1 降级：连续 3 帧超 64 轮 → 强制同步删除 + 报 `DEFERRED_LOOP` |
| 2    | 统一命名：`obj_delete_anim` 对外；`stop_all_anims_of_obj` 内部 |
| 3    | 方案 C 下 §8.1.2 不适用（标注）                              |
| 4    | `ANIM_CTX_LOST` 触发条件定义                                 |
| 5    | V6 依赖 V1；V1=方案 C 时 V6 跳过                             |

### 设计冻结声明

**本版 = 设计冻结版。不开 v0.5。**

| 冻结                                         | 不冻结                                      |
| -------------------------------------------- | ------------------------------------------- |
| §3.2 / §3.6 / §3.8 / §3.9 / §3.10 的**设计** | 实现细节                                    |
| §1.5 / §3.11 / §13 的**非目标与风险**        | 风险等级（V 结论后校准）                    |
| §11 的**指标框架**                           | 具体数值（待实测校准）                      |
| §16 的 ADR-001~016                           | ADR-013/014/015 具体选项（V1/V2/V7 后归档） |

**下一步不是写文档，是写探针。** V1–V7 跑完，基于**实测**一次性修订——那时候改的是事实，不是推测。

---

## 〇、一句话结论

**把项目定义为"受控 C ABI 工程"，而不是"自动生成 C 绑定的练习"。**

LVGL 是纯 C 库，ABI 层面比 Qt/C++ 简单；工程量集中在四件事：**线程亲和性、回调桥接、生命周期级联失效、绘制缓冲所有权**。这四件做对了，控件覆盖率只是时间问题。

⚠️ **关键立场**：这四件事中的前两件（线程、回调）**依赖 P0 第 1 周的验证结论 V1/V2**。V1/V2 出结论前，§3.2 / §3.6 / §3.8 / §3.9 / §3.10 的**代码不应动工**。

---

## 一、项目定位与命名

### 1.1 命名

**`lvgl4cj`**，仓颉包名 `lvgl4cj`。与 TPC 现有风格一致（`lrc4cj`、`vlayout4cj`）。

### 1.2 支持矩阵

| 维度         | MVP 支持                                  | 说明                                                     |
| ------------ | ----------------------------------------- | -------------------------------------------------------- |
| **指针宽度** | **仅 64 位**                              | 32 位平台 `void*` 与 `Int64` 不兼容（见 §3.2），二期评估 |
| 操作系统     | Linux x86_64 / ARM64、macOS（仅开发验证） | 见 §9.3 平台矩阵                                         |
| 目标场景     | 嵌入式 HMI、工业面板、智能设备            | **不含 MCU/RTOS**，见 §13 决策门 G1                      |
| LVGL 版本    | 锁定 v9.2+                                | 不承诺跨大版本兼容                                       |
| 线程模型     | 单 LVGL OS 线程                           | 见 §3.6                                                  |
| 仓颉 SDK     | 1.1.0                                     | 见 §3.6 待确认项 V1                                      |

### 1.3 设计目标（按优先级）

1. **安全**：句柄失效可检测，无悬空指针与 double-free
2. **可调试**：错误有明确上下文，崩溃可定位到仓颉调用点
3. **确定性**：线程模型与重入行为有明确定义，不出现间歇性崩溃
4. **够用**：覆盖 HMI 常见控件与交互
5. **快**：UI 线程不被阻塞，FFI 开销可控

### 1.4 非目标

| 非目标                       | 原因                                  | 替代路径                    |
| ---------------------------- | ------------------------------------- | --------------------------- |
| 裸机 / MCU / RTOS            | 仓颉运行时资源模型未验证（决策门 G1） | C 主控 + 仓颉业务           |
| 多线程并发调用 LVGL          | LVGL 非线程安全                       | 单线程 + 任务投递           |
| **`lv_draw_*` 底层绘制 API** | v9 draw unit 模型复杂                 | **用 Canvas 控件**（§3.11） |
| 自动生成全量 API             | 宏与回调必须手写                      | 生成器只覆盖 setter/getter  |
| 新的声明式 UI 框架           | 超出绑定层职责                        | —                           |
| 32 位平台                    | `void*` 截断风险                      | 二期用独立句柄表            |

### 1.5 非目标 → 替代路径对照表

| 用户想做                      | 非目标 API          | 替代路径                                   | 章节    |
| ----------------------------- | ------------------- | ------------------------------------------ | ------- |
| 自定义绘制                    | `lv_draw_*`         | `LvCanvas`                                 | §3.11.2 |
| 绘制事件拦截                  | `LV_EVENT_DRAW_*`   | 不支持；用 Canvas 重绘                     | §3.7.3  |
| 从 SPI Flash 加载图片字体     | 自定义 `lv_fs` 驱动 | MVP：POSIX 驱动；**P2 前必须补自定义驱动** | §3.13.1 |
| 在 MCU 上跑仓颉 + LVGL        | —                   | C 主控渲染 + 仓颉跑业务                    | §13 G1  |
| 32 位 ARM 设备                | —                   | 无（MVP）；P3 用独立句柄表                 | §13 R13 |
| 直接跨线程调 `btn.setWidth()` | —                   | `runtime.post { }`                         | §6.7    |

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
│      · 闭包注册表（默认分支 B）                           │
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
| 回调是 C 函数指针，仓颉闭包传不进 | trampoline + closure_id         | §3.2  |
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
- `INVALIDATED` 保留表项，让仓颉侧能给出**明确报错**，同时保留调试信息
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

#### 3.1.3 幂等性契约

| 操作         | 在 `ALIVE`               | 在 `INVALIDATED`   | 在 `RELEASED`      |
| ------------ | ------------------------ | ------------------ | ------------------ |
| `close()`    | → INVALIDATED            | 无操作（幂等）     | 无操作（幂等）     |
| `release()`  | → INVALIDATED → RELEASED | → RELEASED         | 无操作（幂等）     |
| `isAlive()`  | true                     | false              | false              |
| 任何业务 API | 正常                     | 抛 `InvalidHandle` | 抛 `InvalidHandle` |

#### 3.1.4 `close()` 内部的顺序安全

```
仓颉 obj.close()
  │
  ├─ 1. lvglcj_obj_delete(h)
  │     └─ C 侧 lv_obj_delete(obj)
  │           └─ LVGL 同步触发 LV_EVENT_DELETE
  │                 └─ lvglcj_delete_hook(e)
  │                       ├─ (a) 从闭包表移除该对象所有 closure（防泄漏）
  │                       ├─ (b) 递归 invalidate 子句柄
  │                       ├─ (c) 停止该对象所有动画（§3.10）
  │                       └─ (d) invalidate 自身句柄 → 状态 = INVALIDATED
  │
  ├─ 2. 回到仓颉侧，调 lvglcj_handle_release(h)
  │     └─ 状态 INVALIDATED → RELEASED
  │
  └─ 3. closed = true（本地标记，保证仓颉侧幂等）
```

关键点：**步骤 (a) 必须在 (b)(c)(d) 之前**。这个顺序在 C 侧 `lifecycle.c` 里写死，并加注释锁定。

#### 3.1.5 LVGL 内部删除时仓颉如何感知

**统一靠 `LV_EVENT_DELETE` 钩子**——只要对象是通过 `lvglcj_obj_create` 创建的，就一定挂了钩子。

⚠️ **例外**：LVGL 内部自己创建的对象（如 dropdown 的列表、msgbox 的按钮）不经过我们的创建入口，没有钩子。**策略**：这类对象不向仓颉暴露句柄，或在暴露前由 C 侧补挂钩子。

---

### 3.2 回调桥接（默认分支 B）★含 Patch P2

⚠️ **本章有两套设计，最终采用哪套由 P0 第 1 周的 V2 结论决定。V2 出结论前，本章代码不动工。**

#### 3.2.1 V2 的立论纠正与验证方法

**前提纠正**：v0.3 从 CJQT6 的 `setOnTimeoutCapture` 推出"仓颉原生支持捕获"。**推论方向反了**——`Capture` 变体的存在恰恰暗示仓颉默认不能捕获，CJQT6 必须自建闭包表。

**V2 验证必须拆成两步**：

| 步骤     | 问题                                          | 方法                                                         | 时间         |
| -------- | --------------------------------------------- | ------------------------------------------------------------ | ------------ |
| **V2-a** | 仓颉语言层能否把**捕获闭包**转成 C 函数指针？ | 写最小例子：`let cb: CFunc = { => useLocal() }`，看是否编译通过 | 5 分钟       |
| **V2-b** | 若能，那个指针能否被 C 侧**长期持有**？       | 注册到 C，强制 GC，再调用                                    | 半天（关键） |

**V2-b 是真正的关键**：V2-a 通过只说明"能拿到指针"，不代表"指针持久有效"。

#### 3.2.2 分支选型（默认 B）

| 分支                             | 机制                                        | 定位         | 采用条件                                                     |
| -------------------------------- | ------------------------------------------- | ------------ | ------------------------------------------------------------ |
| **B（自建闭包表 + closure_id）** | 闭包存入全局表，`user_data` 装 `int32_t` ID | **默认设计** | V2-a 失败，**或** V2-b 失败，**或** 无 pin API，**或** pin API 未经实测验证 |
| A（直接持有闭包指针 + pin 表）   | `user_data` 装闭包指针，注册时 pin          | 备选         | V2-a 通过 **且** V2-b 通过 **且** 有 pin API **且** 该 API 经实测验证 |

**倾向性理由**：**B 的复杂度完全在 C 侧掌控，不依赖 GC 行为；A 的复杂度在与 GC 交互，更难验证、更难调试、更容易出间歇性崩溃。** 跨 GC 语言的成熟绑定（JNI 的 `GlobalRef`、CPython 的 `Py_INCREF`、Ruby 的 `rb_gc_register_address`）都倾向"显式句柄 + 显式保活"。

#### 3.2.3 分支 B（默认）：自建闭包表 + closure_id

```c
#include <stdint.h>

#define LVGLCJ_CLOSURE_ID_MAX  (INT32_MAX)

static inline void *lvglcj_cid_to_ptr(int32_t cid) {
    return (void *)(intptr_t)cid;       // 显式经 intptr_t
}

static inline int32_t lvglcj_ptr_to_cid(void *p) {
    return (int32_t)(intptr_t)p;
}
```

```c
static void lvglcj_event_trampoline(lv_event_t *e) {
    int32_t cid = lvglcj_ptr_to_cid(lv_event_get_user_data(e));
    int64_t evh = lvglcj_handle_of(e);
    int rc = lvglcj_call_closure(cid, evh);              // 分支 B
    if (rc != 0) {
        lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, cid, __func__);
    }
    lvglcj_handle_invalidate(evh);
    lvglcj_handle_release(evh);
}
```

配套约束：
- 闭包表使用 `int32_t` ID，分配时检查不超过 `INT32_MAX`
- **MVP 明确仅支持 64 位平台**（§1.2）

#### 3.2.4 分支 A（备选）：直接持有 + pin 表 ★含 Patch P2

**前置条件（必须全部满足）**：
1. V2-a 通过（语言层可编译）
2. V2-b 通过（C 侧可长期持有）
3. 仓颉运行时提供明确的 pin API（等价于 JNI `GlobalRef` / Ruby `rb_gc_register_address`）
4. 该 pin API 经 V2-b 实测验证

**若任一不满足，不得采用分支 A。**

```c
// ⚠️⚠️⚠️ 警告：仅把闭包指针放入 C 数组【不构成 GC 保活】⚠️⚠️⚠️
//
//   C 全局数组不在仓颉 GC 的根集里。GC 扫不到它，闭包仍会被回收。
//
//   真正的 pin 必须调用【仓颉运行时提供的 pin API】
//   （类比 JNI NewGlobalRef / CPython Py_INCREF / Ruby rb_gc_register_address）。
//
//   若仓颉运行时【没有】这类 API：
//       → 分支 A 不可实现
//       → 直接走分支 B
//
//   不要用"放进全局数组"冒充 pin。
//   这是本项目最危险的自欺：
//   照抄这段代码，V2-b 测试恰好通过（测试期间没触发 GC），
//   上线后间歇性崩溃——正是全文档最想避免的那类事故。

#define MAX_PINNED  4096

typedef struct {
    void *handle;      // 运行时 pin 句柄
    void *closure;     // 闭包指针（仅用于诊断）
} pin_entry_t;

static pin_entry_t g_pinned_entries[MAX_PINNED];
static int         g_pinned_count = 0;

// 伪代码，实际 API 名称待 V2-b 确认
void *lvglcj_pin_closure(void *closure) {
    if (g_pinned_count >= MAX_PINNED) {
        lvglcj_record_error(LVGLCJ_ERR_PIN_EXHAUSTED, 0, __func__);
        return NULL;                              // ★ 边界检查
    }
    void *handle = cangjie_gc_pin(closure);       // ★ 运行时 API，不是 C 数组
    if (!handle) return NULL;
    g_pinned_entries[g_pinned_count].handle = handle;
    g_pinned_entries[g_pinned_count].closure = closure;
    g_pinned_count++;
    return handle;
}

void lvglcj_unpin_closure(void *handle) {
    for (int i = 0; i < g_pinned_count; i++) {
        if (g_pinned_entries[i].handle == handle) {
            cangjie_gc_unpin(handle);             // ★ 运行时 API
            g_pinned_entries[i] = g_pinned_entries[--g_pinned_count];
            return;
        }
    }
}
```

⚠️ **修正 v0.3 的错误陈述**：v0.3 说分支 A"不需要注销闭包"，**这是错的**。准确表述：

> 分支 A 下，LVGL 在对象删除时会自行清理 `lv_event_dsc_t`（**不需要**显式调 `lv_obj_remove_event_cb`）；但**必须**从 pin 表移除闭包（否则 pin 表本身泄漏）。DELETE 钩子里调 `lvglcj_unpin_closure`。

#### 3.2.5 trampoline 集合（两分支共用）

| #    | trampoline                     | 对应 LVGL 回调          | user_data 装载              |
| ---- | ------------------------------ | ----------------------- | --------------------------- |
| T1   | `lvglcj_event_trampoline`      | `lv_event_cb_t`         | A: 闭包指针 / B: closure_id |
| T2   | `lvglcj_timer_trampoline`      | `lv_timer_cb_t`         | 同上                        |
| T3   | `lvglcj_anim_exec_trampoline`  | `lv_anim_exec_xcb_t`    | anim_ctx 指针（见 §3.10）   |
| T4   | `lvglcj_flush_trampoline`      | `lv_display_flush_cb_t` | 同上                        |
| T5   | `lvglcj_indev_read_trampoline` | `lv_indev_read_cb_t`    | 同上                        |
| T6   | `lvglcj_fs_trampoline`         | `lv_fs_drv_t` 各回调    | drv_id + op                 |

**异常处理（两分支共用）**：仓颉闭包内抛出的异常必须在 trampoline 边界捕获，转成日志 + 全局错误回调（§3.12.3）。

---

### 3.3 生命周期级联失效

见 §3.1.4 时序。补充：

#### 3.3.1 递归失效的顺序

**先深后浅（叶子 → 根）**，保证父对象在处理时子对象已失效。

#### 3.3.2 `lv_obj_clean()` 语义（显式标志实现）

**问题**：无法从单个 DELETE 事件判断"父是不是正在被 clean"——父对象此刻仍活着且完全有效。

**修订方案**：在 C 侧 `lvglcj_obj_clean` 包装函数里显式设置标志，**支持嵌套**：

```c
static int64_t g_cleaning_parent = 0;

int32_t lvglcj_obj_clean(int64_t parent) {
    int64_t saved = g_cleaning_parent;       // 保存-恢复，支持嵌套
    g_cleaning_parent = parent;
    lv_obj_clean(lvglcj_ptr_of(parent));
    g_cleaning_parent = saved;
    return LVGLCJ_OK;
}

static void lvglcj_delete_hook(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *parent = lv_obj_get_parent(target);

    if (parent && lvglcj_handle_of(parent) == g_cleaning_parent) {
        // 这是 clean 导致的子删除：子失效，父不失效
        lvglcj_invalidate_subtree(target);
        lvglcj_handle_invalidate(lvglcj_handle_of(target));
        return;
    }
    // 正常 delete：原逻辑
    ...
}
```

#### 3.3.3 DELETE 钩子的注册位置

统一在 `lvglcj_obj_create` 里挂，保证所有通过绑定层创建的对象都有钩子。

---

### 3.4 绘制缓冲

#### 3.4.1 为什么必须在 C 侧分配

**LVGL v9 的 `lv_display_set_buffers()` 接收裸缓冲区指针并在 flush 期间长期持有。**

⚠️ **绝不能把仓颉 `Array<UInt8>` 的底层指针传给 LVGL** —— GC 可能移动或回收，导致花屏/段错误。**缓冲必须在 C 侧分配并由桥接层持有。**

#### 3.4.2 stride 对齐

```c
// 正确算法
size_t lvglcj_calc_buf_bytes(int32_t w, int32_t h, int32_t fmt) {
    size_t stride = lv_draw_buf_width_to_stride(w, fmt);   // 用 LVGL 自己的算子
    return stride * h;
}
```

**原则：任何缓冲尺寸计算都调 LVGL 提供的 `lv_draw_buf_*` 算子，不自己算。**

#### 3.4.3 颜色格式与字节序

| 格式                                    | bpp  | 说明        |
| --------------------------------------- | ---- | ----------- |
| `LV_COLOR_FORMAT_RGB565`                | 2    | 最常见      |
| `LV_COLOR_FORMAT_RGB888`                | 3    | 无 alpha    |
| `LV_COLOR_FORMAT_ARGB8888` / `XRGB8888` | 4    | 桌面/带 GPU |

⚠️ **flush 回调签名 v9 为 `(lv_display_t*, const lv_area_t*, uint8_t* px_map)`** —— 第三参是 `uint8_t*`，**不是 v8 的 `lv_color_t*`**。

#### 3.4.4 三种渲染模式差异

| 模式      | 缓冲大小  | flush 语义             | 适用           |
| --------- | --------- | ---------------------- | -------------- |
| `PARTIAL` | ≥ 1/10 屏 | 只刷脏区，分块调用多次 | **MVP 默认**   |
| `DIRECT`  | **整屏**  | 只刷脏区               | 有足够 RAM     |
| `FULL`    | **整屏**  | 每次全刷               | 双缓冲传统模型 |

#### 3.4.5 `flush_wait_cb`（带超时）

**作用**：LVGL 通过 wait_cb 等待异步传输完成。**超时是强制要求**：

```c
// C 侧提供带超时的 wait
// 回调内实现必须：
//   - 等待主线程渲染完成信号量
//   - 超时（默认 100ms，可配置）后返回，不无限阻塞
//   - 超时时记录 BACKEND_FAILURE + 日志；画面可能撕裂，但 LVGL 线程不挂死
```

#### 3.4.6 缓存一致性与 DMA

ARM 带 cache 的平台：
- 缓冲分配考虑 cache line 对齐（≥64B）
- 传输前 `clean`，接收后 `invalidate`

#### 3.4.7 其他

| 项              | 处理                                           |
| --------------- | ---------------------------------------------- |
| **malloc 失败** | 返回 `ERR_OUT_OF_MEMORY`，不静默降级           |
| **旋转**        | 监听 `LV_EVENT_RESOLUTION_CHANGED` 重建缓冲    |
| **删除顺序**    | 先 `lv_display_delete()`，**再** `free()` 缓冲 |
| **双缓冲同步**  | LVGL 内部管理乒乓，桥接层只负责分配与最终释放  |

---

### 3.5 `lv_conf.h` 版本化与代码生成

#### 3.5.1 宏 → 查询函数

```c
int32_t lvglcj_conf_color_depth(void)  { return LV_COLOR_DEPTH; }
int32_t lvglcj_conf_use_log(void)      { return LV_USE_LOG; }
int32_t lvglcj_conf_mem_size(void)     { return LV_MEM_SIZE; }
int32_t lvglcj_conf_stride_align(void) { return LV_DRAW_BUF_STRIDE_ALIGN; }
uint32_t lvglcj_conf_hash(void);
const char *lvglcj_version(void);
```

#### 3.5.2 编译期哈希写入仓颉

```
构建流程：
  1. CMake 编译 native 层 + 探针小程序 probe_conf
  2. 运行 probe_conf，输出 JSON
  3. scripts/gen_conf_const.py 读取 JSON → src/generated/conf_const.cj
  4. cjpm build 编译
```

**依赖声明**：**Python 3.8+ 是构建时依赖**。若目标环境无法装 Python，提供纯 CMake `configure_file` 替代路径。

#### 3.5.3 启动校验

```cangjie
func checkCompatibility(): Unit {
    if (lvglcj_conf_hash() != CONF_HASH) {
        throw LvglException(LvglError.InvalidConfig,
            "LVGL 构建配置不匹配：绑定层 hash=0x${CONF_HASH}，" +
            "当前库 hash=0x${lvglcj_conf_hash()}。")
    }
}
```

---

### 3.6 线程模型

#### 3.6.1 问题本质

**LVGL 要求所有 `lv_*` 调用在同一个 OS 线程内。仓颉 `spawn` 创建的是轻量级线程（M:N 调度），可能被调度到不同 OS 线程。**

#### 3.6.2 三个候选方案

| 方案          | 机制                                      | 优点                        | 风险                             |
| ------------- | ----------------------------------------- | --------------------------- | -------------------------------- |
| **A（推荐）** | **C 侧 `pthread_create` OS 线程**跑主循环 | OS 线程确定，与仓颉调度解耦 | 需仓颉支持"外部 OS 线程执行闭包" |
| B             | 仓颉侧 `spawn` + 验证亲和性               | 纯仓颉                      | 亲和性不可控                     |
| C             | 泵模式：仓颉主线程定期调 `lvglcj_pump()`  | 最简单                      | UI 与业务抢线程                  |

#### 3.6.3 方案 A 的关键未知：V1（P0 阻塞项）

方案 A 要求在一个**由 C 创建、仓颉运行时不认识的 OS 线程**里执行仓颉闭包。这需要仓颉提供线程附着机制（类似 JNI `AttachCurrentThread`）。

```
C OS 线程循环：
  while (running) {
      task = queue_pop();
      if (task) {
          lvglcj_attach_cangjie();     // 需要这一步
          call_cangjie_closure(task);
          lvglcj_detach_cangjie();
      }
      lv_timer_handler();
      sleep_ms(5);
  }
```

**P0 第一周必须确认**：

| 确认项                                     | 方法                                   |
| ------------------------------------------ | -------------------------------------- |
| 仓颉是否提供外部 OS 线程附着 API           | 查 SDK 文档 / 问社区 / 读 runtime 源码 |
| 若没有，仓颉轻量级线程是否稳定绑定 OS 线程 | **探针测试**（§3.6.4）                 |
| 若也没有，闭包执行是否仅限仓颉线程         | 退回方案 C                             |

#### 3.6.4 探针测试

```cangjie
func probeThreadAffinity(): Unit {
    var tids = HashSet<Int64>()
    let t = spawn {
        for (_ in 0..10000) {
            tids.put(lvglcj_current_os_tid())
            sleep(1.milliseconds)
        }
    }
    t.join()
    println("观察到 ${tids.size} 个不同 OS 线程 ID")
}
```

**这个测试结果是方案选型的唯一依据。**

#### 3.6.5 线程断言（两套宏，Release 保留）

```c
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

**★方案 C 下的语义修正**：方案 C 没有独立 LVGL 线程。此时 `g_lvgl_thread_id` **应初始化为"创建 runtime 的线程 ID"**，即调用 `runtime.start()` 的那条线程。检查逻辑不变——方案 C 下"跨仓颉线程调 `pump()`"会被正确拦截。

#### 3.6.6 最终模型

**方案 A（V1 通过时）**：

```cangjie
public class LvglRuntime {
    public func start(): Unit {
        lvglcj_init()
        checkCompatibility()
        lvglcj_set_tick_cb()
        lvglcj_set_log_cb(onLvglLog)
        lvglcj_start_thread()               // C 侧 pthread_create
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
```

---

### 3.7 事件语义完整模型

#### 3.7.1 target vs current_target

| 概念             | 含义                                 | FFI                               |
| ---------------- | ------------------------------------ | --------------------------------- |
| `target`         | **最初**触发事件的对象（冒泡链起点） | `lvglcj_event_get_target`         |
| `current_target` | **当前**正在处理该事件的对象         | `lvglcj_event_get_current_target` |

#### 3.7.2 冒泡与 trickle

```c
int32_t lvglcj_event_stop_bubbling(int64_t evh);
int32_t lvglcj_event_stop_trickling(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);
```

#### 3.7.3 事件码表

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

⚠️ **DRAW_* 事件注册时应直接返回 `NOT_SUPPORTED` 错误码**。

#### 3.7.4 事件分发顺序

1. 内部 `DELETE` 钩子**总是第一个注册**
2. **同一事件码的多个用户回调按注册顺序全部触发**（LVGL 原生支持，非覆盖）
3. 用户回调中调 `stop_bubbling` 只影响向父级传播

---

### 3.8 回调重入规则

#### 3.8.1 重入场景矩阵

| 场景                         | 是否允许          | 机制            |
| ---------------------------- | ----------------- | --------------- |
| 回调中修改**其他**对象属性   | ✅                 | —               |
| 回调中**创建**对象           | ✅                 | —               |
| 回调中删除**其他**对象       | ⚠️ 延迟删除        | 见下            |
| 回调中删除**自身**           | ⚠️ 延迟删除        | 见下            |
| 回调中**注销**自己的回调     | ⚠️ 延迟注销        | 见下            |
| 回调中触发新事件             | ⚠️ 深度 ≤ 8        | 超限报错        |
| 回调中 `post` 并**同步等待** | ❌ 禁止            | 死锁检测        |
| 回调中抛异常                 | ❌ 禁止跨越 C 边界 | trampoline 捕获 |

#### 3.8.2 延迟删除队列（含次生残留处置 #1）

```c
static _Atomic int g_callback_depth = 0;
static lvglcj_deferred_list_t g_deferred;
static int g_deferred_overrun_frames = 0;   // ★ 连续超限帧计数

int32_t lvglcj_obj_delete(int64_t h) {
    if (atomic_load(&g_callback_depth) > 0) {
        lvglcj_deferred_push(DEFER_DELETE, h);
        lvglcj_handle_mark_pending_delete(h);
        return LVGLCJ_OK_DEFERRED;
    }
    return lvglcj_obj_delete_now(h);
}

// 循环 drain
void lvglcj_drain_deferred(void) {
    const int MAX_ITER = 64;
    int iter = 0;
    while (!lvglcj_deferred_empty(&g_deferred)) {
        if (++iter > MAX_ITER) {
            g_deferred_overrun_frames++;
            lvglcj_record_error(LVGLCJ_ERR_DEFERRED_LOOP, 0, __func__);

            // ★ 次生残留处置 #1：连续 3 帧超限 → 强制同步删除 + 报错
            if (g_deferred_overrun_frames >= 3) {
                lvglcj_force_drain_deferred();   // 同步强制清空
                g_deferred_overrun_frames = 0;
            }
            return;   // 未超 3 帧则保留到下一帧继续
        }
        lvglcj_deferred_item_t item = lvglcj_deferred_pop(&g_deferred);
        switch (item.op) {
            case DEFER_DELETE:  lvglcj_obj_delete_now(item.h); break;
            case DEFER_UNREG:   lvglcj_unregister_closure_now(item.cid); break;
        }
    }
    g_deferred_overrun_frames = 0;   // 正常清空则重置
}
```

**语义明确**（写入用户文档）：**回调中调 `obj.close()` 不会立即生效，但对象立刻进入"待删除"状态；实际删除最迟在同一帧内完成**。极端链式场景下若超过 64 轮，剩余项保留到下一帧继续处理；连续 3 帧超限则强制同步清空，并报 `DEFERRED_LOOP`。

#### 3.8.3 回调期句柄锁定

回调执行期间，被回调引用的对象句柄标记为 `IN_CALLBACK`：
- `close()` → 转延迟删除
- 其他业务 API → 允许

#### 3.8.4 死锁检测（等待图）

**问题**：flush_wait_cb 引入新的死锁路径——主线程调 `postAndWait` 等业务线程，同时 LVGL 线程阻塞在 `flush_wait_cb` 等主线程 → 双向等待。

**修订**：扩展为等待图检测。

```c
typedef enum { WAIT_NONE, WAIT_LVGL_TASK, WAIT_MAIN_RENDER } wait_kind_t;
static _Thread_local wait_kind_t t_wait_kind = WAIT_NONE;

int32_t lvglcj_post_and_wait(int64_t task_id) {
    if (lvglcj_os_thread_self() == g_lvgl_thread_id) {
        return LVGLCJ_ERR_DEADLOCK_RISK;               // 原检查保留
    }
    if (lvglcj_main_thread_is_waiting_render()) {      // 新检查
        return LVGLCJ_ERR_DEADLOCK_RISK;
    }
    // ... 正常投递 + 等待 ...
}
```

**窗口关闭/最小化场景**：SDL 的 `WINDOWEVENT_CLOSE` / `WINDOWEVENT_MINIMIZED` 时，主线程侧主动**释放**渲染信号量并标记"暂停渲染"，让 `flush_wait_cb` 立即返回。

**用户文档写死**：

> 主线程调用 `postAndWait` 时，若 LVGL 线程正阻塞在 `flush_wait_cb`，本次调用会抛出 `DeadlockRisk`。这是保护，不是 bug。请改用 `postAsync` + 回调。

---

### 3.9 任务队列语义

#### 3.9.1 完整定义

| 属性         | 定义                                                        |
| ------------ | ----------------------------------------------------------- |
| 容量         | **有界**，默认 1024；可配置                                 |
| **满时策略** | **默认 fail-fast：抛 `QueueFull`**；可选 Block / DropOldest |
| 返回值       | `Future<Unit>`                                              |
| 关闭策略     | `Drain` / `Discard` / `DrainWithTimeout(ms)`                |

**★异常语义对照表**——两件事必须区分：

| 失败时机                           | 语义                   | 表现                                                         |
| ---------------------------------- | ---------------------- | ------------------------------------------------------------ |
| **入队失败**（队列满）             | 投递动作本身失败       | **立即抛 `QueueFull`**；不产生 `Future`                      |
| **任务执行失败**（仓颉任务抛异常） | 任务已被接受但执行出错 | 异常**存入 `Future`**；`await` 时重新抛出；未 await 则走全局错误回调 |

**为什么默认 fail-fast**：v0.2 默认"阻塞等待"会导致跨线程投递死锁。Go channel、Rust crossbeam、Disruptor 均不默认阻塞。

#### 3.9.2 接口

```cangjie
public class LvglRuntime {
    public func post(task: () -> Unit): Future<Unit>
    public func postAsync<T>(task: () -> T): Future<T>
    public func postAndWait(task: () -> Unit): Unit
    public func stop(mode: ShutdownMode): Unit
}

public enum ShutdownMode { Drain | Discard | DrainWithTimeout(Int64) }
public enum QueueFullPolicy { FailFast | Block | DropOldest }
```

#### 3.9.3 关闭时未执行任务

- `Drain`：执行完队列中所有任务再退出
- `Discard`：清空队列，未执行任务触发 `TaskDiscarded` 回调
- `DrainWithTimeout(n)`：n 毫秒内尽量执行，超时后按 `Discard` 处理

---

### 3.10 动画 API ★含 Patch P1

#### 3.10.1 LVGL v9 动画模型

```c
lv_anim_set_var(&a, obj);                    // exec_cb 的第一个参数
lv_anim_set_exec_cb(&a, exec_cb);            // void (*)(void* var, int32_t value)
```

#### 3.10.2 var 的二元矛盾

**问题**：`var` 就是 `exec_cb` 收到的第一个参数，它**只能有一个值**。v0.3 同时提供了：

- `lvglcj_anim_set_var(anim, obj)` ← `var = obj`
- "把 var 直接用作 closure_id" ← `var = closure_id`

**二选一，不可能同时成立。**

**两个选项的代价**：

| 选项                      | exec_cb 找回闭包   | 同 obj 多动画 | 自动停动画 |
| ------------------------- | ------------------ | ------------- | ---------- |
| `var = obj`，obj→cid 映射 | ❌ 一对多，无法区分 | ❌ 不支持      | ✅ 保住     |
| `var = closure_id`        | ✅                  | ✅             | ❌ 失去     |

⚠️ **v0.3 评审建议的 `var = obj` 有洞**：同一 obj 上有两个动画（同时改 width 和 height），它们的 `var` 都是同一个 obj，trampoline 收到 `(obj, v1)` 和 `(obj, v2)`，**无法区分是哪个动画在调用**。

**修订方案：`var = anim_ctx`**

```c
typedef struct {
    int64_t obj_handle;         // 动画目标对象
    int32_t closure_id;         // 仓颉闭包
    int64_t anim_handle;        // 反向引用
    int32_t user_deleted_cb;    // 用户注册的 deleted 回调（0 = 未注册）
} lvglcj_anim_ctx_t;

int32_t lvglcj_anim_set_target(int64_t anim, int64_t obj) {
    lvglcj_anim_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->obj_handle = obj;
    ctx->anim_handle = anim;
    ctx->closure_id = 0;
    ctx->user_deleted_cb = 0;

    lv_anim_set_var(lvglcj_ptr_of(anim), ctx);   // var = ctx 指针
    lvglcj_bind_anim_to_obj(obj, anim);           // 维护 obj → [anim] 列表
    return LVGLCJ_OK;
}

static void lvglcj_anim_exec_trampoline(void *var, int32_t value) {
    lvglcj_anim_ctx_t *ctx = (lvglcj_anim_ctx_t *)var;
    if (!ctx) {
        lvglcj_record_error(LVGLCJ_ERR_ANIM_CTX_LOST, 0, __func__);
        return;
    }
    int rc = lvglcj_call_closure(ctx->closure_id, value);
    if (rc != 0) lvglcj_record_error(LVGLCJ_ERR_CALLBACK_THREW, ctx->closure_id, __func__);
}
```

**取舍**：

| 方案                       | exec_cb 找回闭包 | 同 obj 多动画 | 自动停动画       |
| -------------------------- | ---------------- | ------------- | ---------------- |
| var = obj，obj→cid 映射    | ❌                | ❌             | ✅                |
| var = closure_id           | ✅                | ✅             | ❌                |
| **var = anim_ctx（推荐）** | ✅                | ✅             | ❌ 失去，需手动补 |

#### 3.10.3 anim_ctx 生命周期（★Patch P1）

**问题**：ctx 由 C 侧 malloc，挂在 `lv_anim_t.var` 上。LVGL 不会 free 它。v0.4 初稿给的释放路径只有两条（`lvglcj_anim_delete` + DELETE 钩子里的 `stop_all_anims_of_obj`），**漏了最常见的一条：动画自然结束**。

**修订方案**：`lvglcj_anim_create` 内部**自动挂一个内部 deleted_cb**，作为 ctx 的**唯一释放点**：

```c
// anim.c —— 内部 deleted_cb，lvglcj_anim_create 时自动挂
static void lvglcj_anim_internal_deleted_cb(lv_anim_t *a) {
    lvglcj_anim_ctx_t *ctx = (lvglcj_anim_ctx_t *)lv_anim_get_var(a);
    if (!ctx) return;

    // 用户回调（若注册）先执行
    if (ctx->user_deleted_cb != 0) {
        lvglcj_call_closure(ctx->user_deleted_cb, ctx->anim_handle);
    }

    lvglcj_unregister_closure(ctx->closure_id);
    lvglcj_unbind_anim_from_obj(ctx->obj_handle, ctx->anim_handle);
    free(ctx);                                    // ★ 唯一释放点
    lv_anim_set_var(a, NULL);                     // 防重入
}

int64_t lvglcj_anim_create(void) {
    // ... 创建 lv_anim_t ...
    lv_anim_set_deleted_cb(a, lvglcj_anim_internal_deleted_cb);   // ★ 自动挂
    // ...
}
```

**配套修订**：

| 项                                | 修订                                                         |
| --------------------------------- | ------------------------------------------------------------ |
| `lvglcj_anim_create`              | 内部自动 `lv_anim_set_deleted_cb(a, lvglcj_anim_internal_deleted_cb)` |
| §5.9 `lvglcj_anim_set_deleted_cb` | 改为**注册用户回调**，不再直接挂到 LVGL；用户回调存在 `ctx->user_deleted_cb` |
| §3.10 删除钩子                    | `stop_all_anims_of_obj` 内的 `free(ctx)` **删除**——统一由 internal deleted_cb 释放，避免双重 free |
| 用户文档                          | 明确"用户不需要（也不应）手动 free ctx"                      |

⚠️ **必须实测（探针 V7）**：LVGL v9 的 `deleted_cb` 在三条路径下是否都触发——

1. **自然结束**（repeat 次数到 / duration 到）
2. **手动删除**（`lv_anim_delete`）
3. **对象删除**（`lv_obj_delete` 触发内部停动画）

**若三条不全触发**：Patch P1 的"internal deleted_cb 作唯一释放点"不成立，需要兜底方案：

- **anim 句柄表**：桥接层维护 `anim_handle → ctx` 映射
- **对账机制**：定期调 `lv_anim_count_running()` 与句柄表数量对账，差值 > 0 则扫描句柄表，清理"LVGL 已无但句柄表仍在"的 ctx
- **触发时机**：每 N 帧（如 60 帧）或每次 `anim_delete` 后

**代价补偿（自动停动画）**：因 `var != obj`，LVGL 原生"对象删除自动停动画"失效。在 DELETE 钩子里手动补：

```c
static void lvglcj_delete_hook(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    int64_t h = lvglcj_handle_of(obj);

    // ... 原有 (a)(b) ...
    // ★ (c) 停止该 obj 的所有动画（内部函数）
    lvglcj_stop_all_anims_of_obj(h);   // 内部名，遍历 obj → [anim] 列表，逐个 lv_anim_delete
                                        // free(ctx) 由 internal deleted_cb 负责
    // ... (d) ...
}
```

#### 3.10.4 FFI 设计

```c
int64_t lvglcj_anim_create(void);                                 // 自动挂 internal deleted_cb
int32_t lvglcj_anim_set_target(int64_t anim, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t anim, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t anim, int32_t cid);       // 填 ctx->closure_id
int32_t lvglcj_anim_set_path(int64_t anim, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t anim, int32_t cnt);
int32_t lvglcj_anim_set_playback(int64_t anim, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t anim, int32_t cid);    // 存 ctx->user_deleted_cb
int32_t lvglcj_anim_start(int64_t anim);
int32_t lvglcj_anim_delete(int64_t anim);
int32_t lvglcj_obj_delete_anim(int64_t obj);                      // 对外 API
int32_t lvglcj_anim_count_running(void);
```

**命名统一**（次生残留处置 #2）：
- `lvglcj_obj_delete_anim` = **对外 API**（用户用）
- `lvglcj_stop_all_anims_of_obj` = **内部函数**（DELETE 钩子用），不对外暴露

#### 3.10.5 仓颉 DSL

```cangjie
LvAnim {
    target(btn)
    values(0, 100)
    duration(300)
    path(AnimPath.EaseOut)
    onExec { v => btn.setWidth(v) }    // 目标引用由闭包持有
}.start()
```

---

### 3.11 自定义绘制策略

#### 3.11.1 决策：`lv_draw_*` 为非目标

LVGL v9 的绘制架构是 **draw unit 模型**，FFI 暴露需要桥接整个 draw 上下文、图层、任务分发链，**复杂度与风险远超收益**。

#### 3.11.2 替代路径：Canvas 控件

```c
int64_t lvglcj_canvas_create(int64_t parent);
int32_t lvglcj_canvas_set_buffer(int64_t canvas, int32_t w, int32_t h);
int32_t lvglcj_canvas_set_palette(int64_t canvas, int32_t idx, uint32_t color);
int32_t lvglcj_canvas_draw_point(int64_t canvas, int32_t x, int32_t y, uint32_t color);
int32_t lvglcj_canvas_draw_line(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_rect(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_arc(int64_t canvas, ...);
int32_t lvglcj_canvas_draw_polygon(int64_t canvas, ...);
int32_t lvglcj_canvas_fill_bg(int64_t canvas, uint32_t color);
```

---

### 3.12 错误模型

#### 3.12.1 错误码表

见附录 B。

#### 3.12.2 错误上下文

```c
typedef struct {
    int32_t  code;
    int64_t  handle;
    int32_t  closure_id;
    const char *func;
    const char *msg;
} lvglcj_error_t;
```

#### 3.12.3 全局错误回调

```cangjie
LvglRuntime.onError { err =>
    println("[lvgl4cj] ${err.code} @ ${err.nativeFunc}, handle=${err.handle}")
}
```

---

### 3.13 文件系统 / 图片 / 字体 / Group

#### 3.13.1 文件系统（`lv_fs`）

**MVP 策略**：不暴露自定义 fs 驱动注册，改为在 C 侧预置 **"POSIX" 驱动**。

```c
int32_t lvglcj_fs_init_posix(const char *root);
```

⚠️ **重要说明**：嵌入式场景常有 SPI Flash、eMMC、自定义存储，**POSIX 驱动在 MVP 只是"能跑通"**。**自定义 fs 驱动是 P2 前必须补的能力**，不是可选项。

#### 3.13.2 图片解码

| 格式       | 方案               | 许可              |
| ---------- | ------------------ | ----------------- |
| 内置解码器 | LVGL 自带          | MIT               |
| PNG        | `lv_libpng`        | libpng: zlib-like |
| JPG        | `lv_libjpeg-turbo` | IJG / BSD         |

#### 3.13.3 字体

```c
int64_t lvglcj_font_load(const char *path);
int32_t lvglcj_obj_set_style_text_font(int64_t obj, int64_t font, int32_t selector);
```

#### 3.13.4 Group

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
│       └── conf_const.cj
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
│       ├── probe_conf.c
│       ├── probe_v1_thread_attach.c     # 新增：V1 探针
│       ├── probe_v2_capture.c           # 新增：V2 探针
│       ├── probe_v3_affinity.c          # 新增：V3 探针
│       ├── probe_v6_sdl_thread.c        # 新增：V6 探针
│       └── probe_v7_anim_deleted.c      # 新增：V7 探针
├── backend/
│   ├── sdl2/
│   ├── fbdev/
│   ├── drm/
│   ├── oh/
│   └── null/
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
│   └── P0_QUICKSTART.md
└── .github/
    ├── ISSUE_TEMPLATE/
    ├── PULL_REQUEST_TEMPLATE.md
    └── workflows/ci.yml
```

---

## 五、C ABI 接口清单

**约定**：`lvglcj_<子系统>_<动作>`，全 `extern "C"`，返回 `int32_t` 错误码（0 成功，负数错误，正数特殊标记）。

### 5.1 运行时与线程

```c
int32_t lvglcj_init(void);
int32_t lvglcj_deinit(void);
void    lvglcj_set_tick_cb(void);
int32_t lvglcj_set_log_cb(int32_t cid);
const char *lvglcj_version(void);
uint32_t lvglcj_conf_hash(void);

uint32_t lvglcj_tick_get(void);
uint32_t lvglcj_tick_elaps(uint32_t prev);

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
// ★ buf_lines 约束：
//   - PARTIAL 模式：buf_lines 有效，决定部分缓冲的行数
//   - DIRECT / FULL 模式：buf_lines 被忽略，缓冲必须整屏
//     传入非零值时不报错，但记录 WARN 日志

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

### 5.5 对象

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

int32_t lvglcj_obj_move_foreground(int64_t obj);
int32_t lvglcj_obj_move_background(int64_t obj);
int32_t lvglcj_obj_move_to_index(int64_t obj, int32_t index);
int32_t lvglcj_obj_swap(int64_t obj1, int64_t obj2);
```

### 5.6 事件

```c
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int32_t cid);
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t ev_dsc);
int32_t lvglcj_obj_remove_event_by_cid(int64_t obj, int32_t cid);
int32_t lvglcj_obj_send_event(int64_t obj, int32_t code, int64_t param);
// ★ param 语义：支持两条路径
//   路径 1（句柄）：param 是句柄表注册的对象句柄
//     接收侧用 lvglcj_event_get_user_data(evh) 取回
//   路径 2（标量）：param 经 intptr_t 编码的标量（如 &btn_id）
//     接收侧用 lvglcj_event_get_param_scalar(evh) 取回
//   两条路径由注册侧决定，接收侧必须用对应 API

int32_t lvglcj_event_set_user_data(int64_t evh, int64_t handle);
int32_t lvglcj_event_get_code(int64_t evh);
int64_t lvglcj_event_get_target(int64_t evh);
int64_t lvglcj_event_get_current_target(int64_t evh);
int64_t lvglcj_event_get_user_data(int64_t evh);
int64_t lvglcj_event_get_param_scalar(int64_t evh);
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
int64_t lvglcj_anim_create(void);                                 // 自动挂 internal deleted_cb
int32_t lvglcj_anim_set_target(int64_t a, int64_t obj);
int32_t lvglcj_anim_set_values(int64_t a, int32_t from, int32_t to);
int32_t lvglcj_anim_set_time(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_delay(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_exec_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_set_path(int64_t a, int32_t path);
int32_t lvglcj_anim_set_repeat(int64_t a, int32_t cnt);
int32_t lvglcj_anim_set_playback(int64_t a, int32_t ms);
int32_t lvglcj_anim_set_deleted_cb(int64_t a, int32_t cid);       // 存 ctx->user_deleted_cb
int32_t lvglcj_anim_set_start_cb / ready_cb(int64_t a, int32_t cid);
int32_t lvglcj_anim_start(int64_t a);
int32_t lvglcj_anim_delete(int64_t a);
int32_t lvglcj_obj_delete_anim(int64_t obj);                      // 对外 API
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

### 5.11 调试 / 可观测性

```c
typedef struct {
    int64_t *handles;
    int32_t *depths;
    char   **names;
    int32_t  count;
} lvglcj_tree_dump_t;

int32_t lvglcj_debug_dump_tree(int64_t root, int32_t max_depth,
                               lvglcj_tree_dump_t *out);
void    lvglcj_debug_free_tree_dump(lvglcj_tree_dump_t *dump);

int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag,
                           uint32_t *max_used, uint32_t *free_size);

typedef struct {
    uint32_t fps;
    uint32_t cpu_percent;
    uint32_t refr_time_ms;
    uint32_t draw_time_ms;
    uint32_t obj_count;
} lvglcj_perf_t;
int32_t lvglcj_perf_sample(lvglcj_perf_t *out);

int32_t lvglcj_handle_count(int32_t state);
```

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
    radius(12)
    padAll(16)
    shadowWidth(12)
    shadowColor(0x000000)
    textFont(theme.fontBody)
    textColor(0xFFFFFF)
}

card.apply(cardStyle, Part.Main)
card.apply(pressedStyle, Part.Main | State.Pressed)
```

### 6.3 事件 DSL

```cangjie
let h = btn.on(Event.Clicked) { e =>
    println("clicked")
}
h.remove()

// 同一事件码多个回调，全部触发
btn.on(Event.Clicked) { e => println("A") }
btn.on(Event.Clicked) { e => println("B") }

// 冒泡
container.addFlag(ObjFlag.EventBubble)
container.on(Event.Clicked) { e =>
    let origin = e.target
    let current = e.currentTarget
    println("来自 ${origin} 的事件冒泡到 ${current}")
}

// 停止冒泡
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

### 7.1 控件清单（分批交付）

| 批次   | 控件                                                         | 说明     |
| ------ | ------------------------------------------------------------ | -------- |
| **P0** | `obj`(base) `label` `button`                                 | 最小闭环 |
| **P1** | `slider` `switch` `checkbox` `image` `bar` `arc` `led` `line` `spinner` `dropdown` | 常用交互 |
| **P1** | `chart` `table` `roller` `textarea` `keyboard`               | 复合控件 |
| **P2** | `tabview` `menu` `msgbox` `win` `tileview` `scale` `calendar` `animimg` `span` `imgbtn` | 高级容器 |
| **P3** | `3dtexture` `flex/grid 布局对象` 其余                        | 按需     |

### 7.2 样式属性覆盖

见 §5.7，共 **40+ 属性**。

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

---

## 八、后端适配

### 8.1 SDL2 后端详解

#### 8.1.1 组件

| SDL 对象       | 用途         |
| -------------- | ------------ |
| `SDL_Window`   | 宿主窗口     |
| `SDL_Renderer` | 渲染器       |
| `SDL_Texture`  | 像素缓冲载体 |
| `SDL_Event`    | 输入事件源   |

#### 8.1.2 线程关系与 flush_wait 同步

> ⚠️ **本节仅适用于方案 A（C 侧独立 OS 线程）。方案 C 下 LVGL 与 SDL 渲染同在主线程，无 flush_wait 双线程同步需求，本节整体不适用。**

**SDL 事件循环必须在创建窗口的线程**（多数平台要求主线程）。而 LVGL 主循环在 C 侧 OS 线程。两者**必须分离**：

```
主线程（OS main thread）
  └─ SDL_Init / CreateWindow / CreateRenderer
  └─ while: SDL_PollEvent → 转成 LVGL 输入状态
     └─ 收到"渲染请求"事件 → SDL_RenderCopy + SDL_RenderPresent
        └─ 完成后置信号量
     └─ 窗口关闭/最小化 → 释放信号量 + 标记暂停渲染

LVGL OS 线程
  └─ while: drain deferred → drain tasks → lv_timer_handler() → sleep 5ms
     └─ flush_cb: SDL_UpdateTexture
        └─ 投递"渲染请求"到主线程（SDL_PushEvent）
     └─ flush_wait_cb: 等待主线程完成信号量（带超时）
```

**★强制要求**：

1. **`flush_wait_cb` 必须实现**：主线程完成 `SDL_RenderPresent` 后置信号量；`flush_wait_cb` 等待该信号量
2. **`flush_wait_cb` 必须带超时**（默认 100ms）：超时后返回，记录 `BACKEND_FAILURE` + 日志；画面可能撕裂，但 LVGL 线程不挂死
3. **窗口关闭/最小化场景**：主线程侧主动释放渲染信号量并标记"暂停渲染"，让 `flush_wait_cb` 立即返回（不等超时）
4. **死锁检测扩展**（§3.8.4）：主线程调 `postAndWait` 时检查是否形成等待环

#### 8.1.3 像素格式转换

| `lv_display` 格式 | SDL 纹理格式               |
| ----------------- | -------------------------- |
| `RGB565`          | `SDL_PIXELFORMAT_RGB565`   |
| `RGB888`          | `SDL_PIXELFORMAT_RGB888`   |
| `ARGB8888`        | `SDL_PIXELFORMAT_ARGB8888` |
| `XRGB8888`        | `SDL_PIXELFORMAT_XRGB8888` |

**必须一一对应**，否则花屏。

#### 8.1.4 输入映射

| SDL 事件                            | LVGL indev          |
| ----------------------------------- | ------------------- |
| `SDL_MOUSEMOTION` / `BUTTONDOWN/UP` | pointer             |
| `SDL_MOUSEWHEEL`                    | encoder             |
| `SDL_KEYDOWN/UP`                    | keypad              |
| `SDL_TEXTINPUT`                     | textarea 输入       |
| `SDL_WINDOWEVENT_CLOSE`             | 触发 runtime.stop() |

#### 8.1.5 多窗口

MVP **仅支持单窗口**。

#### 8.1.6 决策：手写 flush/read 而非用内置 SDL 驱动

1. 内置驱动绕过 trampoline，跳过了最需要验证的回调链路
2. 手写能控制线程关系（§8.1.2）
3. 内置驱动行为在版本间可能变化

### 8.2 fbdev / DRM

| 后端    | 要点                                                         |
| ------- | ------------------------------------------------------------ |
| fbdev   | 打开 `/dev/fb0`，`mmap`，flush 直接 `memcpy`；需处理 stride 与像素格式 |
| DRM/GBM | `drmModeSetCrtc`、dumb buffer 或 GBM bo、双缓冲 + page flip  |

### 8.3 OpenHarmony 后端可行性分析

| 环节             | OH 机制                                     | 约束与风险                            |
| ---------------- | ------------------------------------------- | ------------------------------------- |
| **窗口**         | `NativeWindow` (OH_NativeWindow)            | 仓颉能否直接调用 OH NDK？**待确认**   |
| **渲染**         | `OH_NativeWindow_NativeWindowRequestBuffer` | 与 LVGL flush_cb 模型契合             |
| **Vsync**        | `OH_NativeVSync`                            | 可用 Vsync 驱动 `lv_timer_handler`    |
| **输入**         | `OH_Input` / ArkTS 侧事件                   | **最大不确定性**                      |
| **NAPI**         | OH 原生扩展机制                             | 仓颉与 OH NAPI 的互操作能力**需实测** |
| **ArkTS 互操作** | 若仓颉 UI 嵌在 ArkTS 页面内                 | 需仓颉 ⇄ ArkTS 桥                     |

**结论：OH 后端的风险不在 LVGL，在仓颉与 OH 系统服务的互操作能力。** 列为决策门 G2。

---

## 九、构建系统与工程化

### 9.1 cjpm 与 CMake 协作

```
构建顺序：
  1. CMake 构建 LVGL → liblvgl.a
  2. CMake 构建桥接层 → liblvgl4cj_bridge.a/.so
  3. 运行 probe_conf → conf.json
  4. python gen_conf_const.py → src/generated/conf_const.cj
  5. cjpm build → 链接 → 可执行文件
```

**构建前置依赖**：**Python 3.8+**。目标环境无法装 Python 时，用纯 CMake `configure_file` 生成 `.cj`。

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

⚠️ **P0 第一步应照抄 `CJQT6/cjpm.toml`**。

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

**headless 运行**（CI 必需）：`backend_null`。

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

### 9.5 ASan 与仓颉 GC 兼容性

**隔离策略**：

1. **分层启用**：C 桥接层 + LVGL 用 ASan 编译；仓颉侧不启用
2. **suppression 文件**：`scripts/asan_suppressions.txt`，屏蔽仓颉运行时符号
3. ⚠️ **符号命名需实证确认**：P0 阶段用 `nm` 检查实际符号，补充规则
4. **验证 ASan 有效性**：先人为制造一个 C 侧泄漏，确认 ASan 能报出
5. **备选**：若冲突，退回 `valgrind --tool=memcheck` 或 LVGL 内置内存检查

---

## 十、测试策略与 CI

### 10.1 测试分层

| 层级     | 框架             | 内容                          | CI   |
| -------- | ---------------- | ----------------------------- | ---- |
| C 侧单测 | 自写断言 + CTest | 句柄表四态、状态机、延迟队列  | ✅    |
| FFI 契约 | 仓颉单测         | 结构体 offset/padding、枚举值 | ✅    |
| 仓颉单测 | 仓颉测试框架     | 句柄、回调、事件、动画        | ✅    |
| 集成测试 | headless 后端    | 建 UI 树 → 模拟输入 → 断言    | ✅    |
| 视觉回归 | SDL2 + 截图 diff | P2                            | ⚠️    |
| Soak     | 24h 长跑         | 内存/句柄/稳定性              | 夜间 |
| 模糊测试 | P2               | 随机 UI 操作序列              | 夜间 |
| 性能基准 | `scripts/bench`  | 帧率/延迟/内存                | 夜间 |

### 10.2 FFI 契约测试

```cangjie
@Test
func testStructLayout(): Unit {
    // ★ 所有与 C 交互的结构体必须用 @C 声明
    let cOffsets = lvglcj_probe_area_offsets()
    let cjOffsets = areaOffsetsFromCangjie()
    @Assert(cOffsets == cjOffsets)
}
```

### 10.3 关键测试用例清单

| 用例              | 断言                                                         |
| ----------------- | ------------------------------------------------------------ |
| `handle_test`     | 四态转移正确；`close()` 重复调用安全；`release()` 幂等       |
| `callback_test`   | 注册→触发→注销；对象删除时闭包自动清理；回调抛异常被吞并记录 |
| `event_test`      | `target` ≠ `current_target`；`stopBubbling` 生效；多回调按序全部触发 |
| `thread_test`     | **亲和性探针**；跨线程调用抛 `WrongThread`                   |
| `anim_test`       | 动画执行、停止、对象删除时清理；**同 obj 多动画各自正确回调**；**自然结束不泄漏 ctx（Patch P1）** |
| `deferred_test`   | 回调中 `close()` 不崩，**同帧内**删除生效；**链式删除不泄漏；连续 3 帧超限强制清空** |
| `lifecycle_test`  | 父删→子句柄失效；`lv_obj_clean` 后父仍存活；**clean 嵌套安全** |
| `flush_wait_test` | 超时返回；窗口关闭立即返回                                   |
| `deadlock_test`   | 主线程 postAndWait 与 flush_wait 环路被检测                  |
| `leak_test`       | 创建 1 万对象再删除，`handleCount(ALIVE)` 与 `objCount` 差值恒定 |
| `soak_test`       | 24h：RSS 收敛、句柄收敛、无崩溃                              |

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

## 十一、量化验收指标

### 11.1 P0 门禁指标

| 指标                    | 门槛                                                  | 测量方式 |
| ----------------------- | ----------------------------------------------------- | -------- |
| **线程亲和性**          | **若走方案 B 则必须 = 0；方案 A 下此指标 N/A**        | §3.6.4   |
| **句柄泄漏**            | 1 万次 create/delete 后 `handleCount(ALIVE)` 增量 = 0 | §7.3     |
| **句柄与对象一致**      | `handleCount(ALIVE) - objCount` 恒定                  | soak     |
| **回调异常**            | 回调抛异常不导致进程崩溃，错误回调被调用              | 单测     |
| **延迟删除**            | 回调中 `close()` 自身不崩溃，**同帧内**对象确实删除   | 单测     |
| **ASan**                | 零报告（且已验证 ASan 工具本身有效）                  | CI       |
| **flush_wait 超时**     | 窗口关闭时 `flush_wait_cb` 立即返回，不挂死           | 单测     |
| **anim 自然结束不泄漏** | **repeat(1) 动画跑完后 ctx 被释放（Patch P1）**       | 单测     |

### 11.2 P1 性能指标

⚠️ **性能指标分两档，取决于 V1 结论。不许用方案 C 的实现去对方案 A 的指标。**

#### 方案 A（C 侧 OS 线程，推荐）

| 指标              | 门槛                                              | 测量条件                    |
| ----------------- | ------------------------------------------------- | --------------------------- |
| 帧率              | ≥ 30 FPS（800×480，局部刷新）                     | SDL2，~50 对象              |
| 帧率（全屏动画）  | ≥ 20 FPS                                          | 800×480 全屏过渡            |
| 输入延迟          | ≤ 50 ms（点击到回调触发）                         | 事件时间戳差                |
| 单次 FFI 调用开销 | **≤ 2 μs（★待实测校准的目标值，首次测量后修订）** | 10 万次 `lv_obj_set_x` 均值 |
| 事件回调延迟      | ≤ 200 μs                                          | 探针计时                    |
| 启动时间          | ≤ 500 ms                                          | 冷启动测量                  |

#### 方案 C（泵模式，回退）

⚠️ **方案 C 下 UI 与业务抢同一线程**。以下指标**条件式**：

| 指标              | 条件式门槛                                                   | 说明                   |
| ----------------- | ------------------------------------------------------------ | ---------------------- |
| 帧率              | 业务循环周期 ≤ 10 ms 时，≥ 15 FPS                            | 业务阻塞时帧率可能归零 |
| 输入延迟          | **业务循环周期 ≤ X ms 时，延迟 ≤ 1 个周期**（X 由用户根据场景选型） | 依赖业务是否阻塞       |
| 单次 FFI 调用开销 | ≤ 2 μs（待实测校准）                                         | 同上                   |
| 启动时间          | ≤ 500 ms                                                     | 同上                   |

**方案 C 下禁止宣传"实时 HMI"**。

### 11.3 内存指标

| 指标              | 门槛                                        |
| ----------------- | ------------------------------------------- |
| RSS 增长          | 24h ≤ 10 MB                                 |
| LVGL 内存池       | 峰值 ≤ 配置值的 80%                         |
| 句柄表大小        | 稳态后不再增长                              |
| 闭包表大小        | 对象删除后归零（分支 B）                    |
| pin 表大小        | 对象删除后归零（分支 A）                    |
| **anim_ctx 数量** | **稳态后与 `lv_anim_count_running()` 一致** |

### 11.4 稳定性指标

| 指标                   | 门槛                           |
| ---------------------- | ------------------------------ |
| 24h 连续运行           | 零崩溃、零 ASan 报告、内存收敛 |
| 100 万次事件触发       | 无崩溃、无句柄泄漏             |
| 随机操作模糊测试（2h） | 无崩溃                         |

### 11.5 基线记录要求

**所有指标必须在 `docs/benchmarks/` 记录**：日期、平台、LVGL 版本、仓颉 SDK 版本、硬件配置、原始数据。

---

## 十二、MVP 路线

### 阶段 P0：跑通闭环 + 排掉阻塞性不确定项

#### 第 1 周：阻塞项清零（**不写业务代码**）

| #      | 任务                                                      | 产出                      | 依赖                    |
| ------ | --------------------------------------------------------- | ------------------------- | ----------------------- |
| V1     | 仓颉外部 OS 线程能否执行闭包                              | 方案 A / C                | —                       |
| V2-a   | 捕获闭包 → C 函数指针（编译）                             | 分支 A 是否可能           | —                       |
| V2-b   | C 侧长期持有 + GC 保活 + pin API 明确性                   | 分支 A / B                | V2-a                    |
| V3     | 仓颉轻量级线程 OS 亲和性                                  | 方案 B 是否可行           | —                       |
| V4     | ASan × 仓颉 GC                                            | 内存检测方案              | —                       |
| V5     | `cjpm.toml [ffi.c]`                                       | 构建可行                  | —                       |
| V6     | SDL2 flush_wait 双线程同步 + 超时                         | §8.1.2 可行性             | **V1**（方案 C 下跳过） |
| **V7** | **`deleted_cb` 三路径触发（自然结束/手动删除/对象删除）** | **anim_ctx 释放是否完备** | —                       |

**V7 是 Patch P1 的必要前提**：若三条路径不全触发，Patch P1 的"internal deleted_cb 作唯一释放点"不成立，需兜底（anim 句柄表 + 对账）。

#### P0 任务依赖图

```
V1（线程方案）───────────┬──► 任务1（主循环）──► 任务2（display+indev）──► 任务3（对象树）──► 任务4（事件+回调）──► 任务5（样式+布局）
                         │                                                                                        │
V2（回调方案）───────────┘                                                                                        │
                                                                                                                  │
V3（亲和性探针）───────────────► 任务1 的验证                                                                      │
V4（ASan）────────────────────► 任务3/4 的内存验证                                                                │
V5（cjpm）────────────────────► 任务2 的构建验证                                                                  │
V6（SDL2 线程）────────────────► 任务2 的同步验证（方案 C 跳过）                                                  │
V7（anim deleted）────────────► 任务4 的动画验证                                                                  ▼
                                                                                                          P0 压力测试
                                                                                                     button → clicked → 删除自己 → 重建
```

**关键路径**：V1/V2 → 任务1 → 任务2 → 任务3 → 任务4 → 任务5 → 压力测试。

**V1/V2 不出结论，任务 1 不能开工。**

#### 第 2-4 周（闭环）

| #    | 任务                                         | 验收                                | 依赖       |
| ---- | -------------------------------------------- | ----------------------------------- | ---------- |
| 1    | `lv_init` / tick / `lv_timer_handler` 主循环 | 稳定刷帧                            | V1         |
| 2    | SDL2 display flush + pointer indev           | 按钮可点，坐标正确，非花屏          | V1, V5, V6 |
| 3    | 对象树 + 四态句柄 + 级联失效                 | 父删→子失效，无悬空；clean 语义正确 | V4         |
| 4    | 事件回调 + timer 回调                        | 注册/注销/重入安全；动画不泄漏      | V2, V4, V7 |
| 5    | 样式最小集 + Flex 布局                       | 布局正确                            | —          |

**P0 压力测试**：`button → clicked → 删除自己 → 重建`。跑不稳就不许加控件。

### 阶段 P1：可用

- 18 个控件（§7.1）
- 样式补齐到 40+ 属性；动画 API；Group 与键盘导航
- 延迟删除队列、冒泡语义、错误模型
- 四平台 CI + ASan；`examples/widgets_demo`、`anim_demo`
- 量化指标达标（§11.2/11.3）
- **FreeType 许可评估**

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

| ID      | 风险                                        | 等级   | 应对                                                         | Owner        | 触发条件             | 评估时间点     |
| ------- | ------------------------------------------- | ------ | ------------------------------------------------------------ | ------------ | -------------------- | -------------- |
| R1      | **仓颉无法在外部 OS 线程执行闭包**          | 高     | 退回泵模式（方案 C）                                         | 架构负责人   | V1 结论为否          | **P0 第 1 周** |
| R2      | 仓颉轻量级线程 M:N 迁移                     | 高     | 强制方案 A + 线程断言                                        | 架构负责人   | 探针 OS TID 变化 > 0 | **P0 第 1 周** |
| R3      | ASan 与仓颉 GC 冲突                         | 中     | 用 valgrind / LVGL 内置检查                                  | 测试负责人   | ASan 误报或崩溃      | **P0 第 1 周** |
| R4      | 回调内异常跨越 C 边界导致崩溃               | 高     | trampoline 捕获 + 全局错误回调                               | 桥接层负责人 | 单测失败             | P0 第 3 周     |
| R5      | 绘制缓冲被 GC 移动 → 花屏                   | 高     | C 侧对齐分配                                                 | 桥接层负责人 | 间歇花屏             | P0 第 2 周     |
| R6      | 父删子导致悬空句柄                          | 高     | DELETE 钩子级联失效                                          | 桥接层负责人 | 单测失败             | P0 第 3 周     |
| R7      | 回调中删除自身导致重入崩溃                  | 高     | 延迟删除队列                                                 | 桥接层负责人 | 压力测试失败         | P0 第 4 周     |
| R8      | `lv_conf` 不匹配导致隐蔽 bug                | 中     | 哈希校验                                                     | 构建负责人   | 换机器构建失败       | P0 第 2 周     |
| R9      | LVGL 版本升级破坏 API                       | 中     | 版本锁定 + 独立升级分支                                      | 维护者       | —                    | 每季度         |
| R10     | 控件 API 量大手写不完                       | 中     | 生成器 + 分批交付                                            | API 负责人   | 进度落后 2 周        | P1 中期        |
| R11     | **仓颉运行时不适合 MCU**                    | 高     | 见决策门 G1                                                  | 架构负责人   | 基准测试不达标       | **P2 完成后**  |
| R12     | OH 系统服务互操作不通                       | 高     | 见决策门 G2；独立探针                                        | OH 负责人    | 探针失败             | **P2 完成后**  |
| R13     | 32 位平台 `void*` 截断                      | 中     | MVP 仅 64 位；二期独立句柄表                                 | 架构负责人   | 出现 32 位需求       | 需求出现时     |
| R14     | LVGL 许可与 SBOM 不合规                     | 中     | 许可清单前置审查                                             | 合规负责人   | 入库前审查           | 发布前         |
| R15     | FreeType GPL 许可阻碍中文 HMI               | 中     | 提前到 P1 前评估                                             | 合规负责人   | P1 中期字体需求落地  | **P1 前**      |
| R16     | **分支 A 的 GC 保活失败（间歇性崩溃）**     | **高** | **默认走分支 B；分支 A 仅在 V2-b 通过且 pin API 明确时采用** | 架构负责人   | V2-b 结论为否        | **P0 第 1 周** |
| R17     | flush_wait 与 postAndWait 死锁              | 高     | 超时 + 等待图检测 + 窗口关闭释放信号量                       | 后端负责人   | 死锁测试失败         | P0 第 2 周     |
| **R18** | **anim_ctx 自然结束泄漏（Patch P1 引入）**  | **高** | **内部 deleted_cb 自动挂；V7 实测三路径；不达标则加 anim 句柄表兜底** | 桥接层负责人 | V7 结论为否          | **P0 第 1 周** |
| **R19** | **延迟删除队列连续超限增长（次生残留 #1）** | **中** | **连续 3 帧超 64 轮 → 强制同步清空**                         | 桥接层负责人 | deferred_test 失败   | P0 第 4 周     |

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

**任一关键项不达标 → 不做 MCU 版本。**

#### G2：是否启动 OH 后端（P2 后）

三项探针全部通过才排期：
1. 仓颉能调 OH NDK（NativeWindow / NativeVSync）
2. 仓颉能接收 OH 输入事件
3. 仓颉与 ArkTS 互操作路径明确

---

## 十四、许可、治理与发布

### 14.1 许可

| 组件              | 许可                              | 备注                                 |
| ----------------- | --------------------------------- | ------------------------------------ |
| **LVGL 主线**     | **MIT**（v8/v9 已改）             | 必须锁定 commit 并核对 `LICENCE.txt` |
| lvgl4cj 绑定层    | **Apache-2.0**                    | 与 LVGL 分离                         |
| **SDL2**          | zlib / MIT 或专有                 | 按版本核对                           |
| **libpng**        | PNG Reference Library (zlib-like) | 若启用 PNG                           |
| **libjpeg-turbo** | IJG / BSD-3 / Zlib 三选一         | 若启用 JPG                           |
| **FreeType**      | **GPLv2 或 FTL（双许可）**        | **GPL 风险，P1 前必须评估**          |
| 内置字体          | 多为 SIL OFL / Apache             | 逐个核对                             |
| ThorVG（矢量）    | MIT                               | 若启用                               |

**行动项**：
- `third_party/lvgl` 保留原始 `LICENCE.txt` + 版本哈希
- CI 生成 **SPDX + CycloneDX** 双格式 SBOM
- **FreeType 的 GPL 选项需在 P1 前专项评估**

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
| **动画语义指南** | `anim_ctx` 方案与自动停动画的手动补偿（§3.10） |
| **死锁规避指南** | postAndWait 与 flush_wait 的相互作用（§3.8.4） |
| API 参考         | 由源码注释生成                                 |
| 迁移指南         | LVGL 版本升级、绑定层版本升级                  |
| 故障排查         | 花屏/崩溃/泄漏 的定位手册                      |
| **非目标清单**   | §1.5 独立成页                                  |

---

## 十五、与现有绑定对比

| 绑定                       | 语言        | 方案                               | 可借鉴                                               |
| -------------------------- | ----------- | ---------------------------------- | ---------------------------------------------------- |
| **lv_binding_rust**        | Rust        | `bindgen` 生成 sys 层 + 手写安全层 | **分层方式（sys / safe）与本方案 L1/L2 一致**        |
| **lv_binding_micropython** | MicroPython | C 模块 + 对象映射                  | 回调转 Python callable 的桥接                        |
| **lvgl-js**                | JS          | JerryScript / QuickJS 绑定         | 轻量 VM 上的对象管理                                 |
| **LVGL 官方 C++ 绑定**     | C++         | RAII 包装                          | 对象生命周期 RAII 思路                               |
| **CJQT6**                  | 仓颉        | 三层 C ABI 桥接 Qt6                | **同语言先例**：工程结构、`close()` 语义、闭包表实现 |

**三条关键借鉴**：

1. **`sys` / `safe` 分层**——与本方案 L1/L2 完全对应
2. **不追求自动生成全量 API**
3. **同语言的 CJQT6 经验**——`Resource` + `close()`、禁用终结器、`setOnTimeoutCapture` 证明闭包表方案可行

**一个差异**：Rust 绑定面临的所有权问题在仓颉不存在（GC 语言），但换来的是"GC 可能移动/延迟释放"的新问题——这正是 §3.4 和 §3.1 要解决的。

---

## 十六、架构决策记录（ADR 摘要）

| ID          | 决策                                                         | 关键理由                                             |
| ----------- | ------------------------------------------------------------ | ---------------------------------------------------- |
| **ADR-001** | 用句柄表（自增 ID）而非裸指针或指针地址作句柄                | 地址复用会导致"误判存活"                             |
| **ADR-002** | 绘制缓冲在 C 侧分配                                          | GC 可能移动/回收仓颉数组                             |
| **ADR-003** | 单 LVGL OS 线程 + 任务队列                                   | LVGL 非线程安全；仓颉线程可能 M:N 迁移               |
| **ADR-004** | **LVGL 主循环必须运行在 OS 线程，不得依赖仓颉线程语义**（已确认原则） | LVGL 硬约束                                          |
| **ADR-005** | 不暴露 `lv_draw_*`，用 Canvas                                | v9 draw unit 模型 FFI 成本极高                       |
| **ADR-006** | 显式 `close()`，禁用终结器                                   | GC 时机不确定                                        |
| **ADR-007** | 延迟删除队列                                                 | 回调中同步删除会导致事件链 use-after-free            |
| **ADR-008** | `lv_conf` 哈希校验                                           | 把"莫名花屏/崩溃"前置为"启动一条明确报错"            |
| **ADR-009** | MVP 仅 64 位                                                 | 32 位 `void*` 无法承载 int64 句柄/闭包 ID            |
| **ADR-010** | 锁定 LVGL v9.x，不提供 v8 别名                               | 两套命名并存增加维护成本                             |
| **ADR-011** | 静态库为默认形态                                             | 嵌入式部署无动态链接器                               |
| **ADR-012** | 手写 SDL2 flush/read 而非用内置驱动                          | 内置驱动绕过 trampoline                              |
| **ADR-013** | **【Pending】方案 A（C 侧 OS 线程）还是方案 C（泵模式），由 V1 结论决定** | V1 出结论后归档                                      |
| **ADR-014** | **回调桥接默认走分支 B（自建闭包表）；分支 A 仅在 V2-b 通过且 pin API 明确时采用** | GC 保活的复杂度与不确定性高于闭包表                  |
| **ADR-015** | **动画 var 存 anim_ctx 指针，不用 obj 或 closure_id**        | 同 obj 多动画场景需唯一区分；自动停动画手动补偿      |
| **ADR-016** | **flush_wait_cb 强制超时 + 等待图死锁检测**                  | 防止主线程/LVGL 线程双向等待；窗口关闭场景必须能解锁 |
| **ADR-017** | **anim_ctx 由内部 deleted_cb 唯一释放；用户回调包装到 ctx->user_deleted_cb**（Patch P1） | 自然结束是最常见路径，必须统一释放点                 |
| **ADR-018** | **pin 表必须调用运行时 API；C 全局数组不构成 GC 保活**（Patch P2） | 防止"放进全局数组"冒充 pin 导致上线间歇崩溃          |

**ADR-004 vs ADR-013 的区别**：
- ADR-004 是**已确认原则**：LVGL 主循环必须在 OS 线程
- ADR-013 是**待定决策**：A（独立 OS 线程）还是 C（复用主线程泵）

---

## 十七、第一个可交付物

**两周目标不是"框架"，是这一张图 + 八个断言：**

```
examples/hello_cj/
  → SDL2 窗口 800×480，深色背景，中间一个按钮
  → 点击按钮，文字从 "点我" 变成 "点了 N 次"
  → 窗口不崩，退出时干净释放
```

**八个断言**（= §11.1 P0 门禁指标）：

1. `lv_init` → `lv_timer_handler()` 循环稳定跑 10 分钟不崩
2. flush 回调被调用，画面正确（非花屏/非黑屏）
3. 点击命中按钮，事件回调触发，坐标正确
4. 删除父容器后，子对象句柄 `isAlive() == false`
5. 退出时 ASan 报告零泄漏
6. **回调中 `close()` 自身不崩溃，同帧内对象确实被删除**
7. **窗口关闭时 `flush_wait_cb` 立即返回，不挂死**
8. **★ `repeat(1)` 动画跑完后 ctx 被释放（Patch P1）**

配套前置：§12 的 V1–V7 七项不确定项结论。

**这八条过了，项目成立；过不了，先修设计，不要往前堆控件。**

---

## 附录 A：术语表

| 术语                    | 含义                                                         |
| ----------------------- | ------------------------------------------------------------ |
| **L1 / L2 / L3 / L4**   | 仓颉 API 层 / C 桥接层 / LVGL 原生层 / 后端层                |
| **句柄（handle）**      | `Int64` 自增 ID，间接引用原生对象；非指针地址                |
| **四态**                | `UNINIT` / `ALIVE` / `INVALIDATED` / `RELEASED`              |
| **trampoline**          | 固定 C 函数，作为 LVGL 回调入口，内部转发到仓颉闭包          |
| **closure_id**          | 仓颉闭包在 C 侧闭包表中的 `int32` 标识（分支 B）             |
| **pin 表**              | 分支 A 下保存闭包指针的**运行时 pin 句柄**表，防止 GC 回收   |
| **anim_ctx**            | 动画执行上下文，存 `var` 指针，含 obj/closure_id/anim_handle/user_deleted_cb |
| **internal deleted_cb** | 桥接层内部挂到 `lv_anim_t` 的回调，ctx 的唯一释放点          |
| **延迟删除**            | 回调执行期间不立即删除，入队，同帧内 drain 完成              |
| **selector**            | v9 样式机制：`Part | State` 组合                             |
| **flush_cb**            | 显示刷新回调                                                 |
| **read_cb**             | 输入设备读取回调                                             |
| **stride**              | 每行像素字节数（含对齐填充）                                 |
| **OS 线程 / 仓颉线程**  | pthread 级真实线程 / 仓颉运行时管理的轻量级线程（M:N）       |
| **G1 / G2**             | 决策门：MCU 准入 / OH 后端准入                               |
| **V1–V7**               | P0 第 1 周待确认项                                           |

## 附录 B：错误码表

| 码     | 名称                | 含义                                                         | 仓颉异常                          |
| ------ | ------------------- | ------------------------------------------------------------ | --------------------------------- |
| 0      | `OK`                | 成功                                                         | —                                 |
| **+1** | **`OK_DEFERRED`**   | **成功但已延迟（非错误）**                                   | 无异常                            |
| -1     | `INVALID_HANDLE`    | 句柄失效                                                     | `LvglException(InvalidHandle)`    |
| -2     | `NOT_INITIALIZED`   | 未调用 `lv_init`                                             | `LvglException(NotInitialized)`   |
| -3     | `INVALID_CONFIG`    | `lv_conf` 哈希不匹配                                         | `LvglException(InvalidConfig)`    |
| -4     | `CALLBACK_THREW`    | 仓颉回调抛异常（已吞掉）                                     | 触发 `onError`                    |
| -5     | `OUT_OF_MEMORY`     | LVGL 池耗尽或 malloc 失败                                    | `LvglException(OutOfMemory)`      |
| -6     | `WRONG_THREAD`      | 跨线程调用 LVGL API                                          | `LvglException(WrongThread)`      |
| -7     | `DEADLOCK_RISK`     | 回调内同步等待 / 等待图成环                                  | `LvglException(DeadlockRisk)`     |
| -8     | `QUEUE_FULL`        | 任务队列满（入队失败）                                       | `LvglException(QueueFull)`        |
| -9     | `BACKEND_FAILURE`   | 后端失败（含 flush_wait 超时）                               | `LvglException(BackendFailure)`   |
| -10    | `INVALID_ARGUMENT`  | 参数非法                                                     | `LvglException(InvalidArgument)`  |
| -11    | `CLOSURE_EXHAUSTED` | 闭包 ID 耗尽                                                 | `LvglException(ClosureExhausted)` |
| -12    | `PENDING_DELETE`    | 对象处于待删除状态                                           | `LvglException(PendingDelete)`    |
| -13    | `NOT_SUPPORTED`     | 该 API 在当前构建配置下不可用                                | `LvglException(NotSupported)`     |
| -14    | `VERSION_MISMATCH`  | LVGL 版本不匹配                                              | `LvglException(VersionMismatch)`  |
| -15    | `DEFERRED_LOOP`     | 延迟删除循环超限（连续 3 帧触发强制清空）                    | `LvglException(DeferredLoop)`     |
| -16    | `PIN_EXHAUSTED`     | pin 表满（分支 A）                                           | `LvglException(PinExhausted)`     |
| -17    | `ANIM_CTX_LOST`     | **动画上下文丢失**：`exec_cb` 收到 `ctx` 但句柄表查不到 `ctx->anim_handle`（ctx 已释放但 trampoline 仍被调用，属异常状态） | `LvglException(AnimCtxLost)`      |

**约定**：
- **0 = 成功**
- **负数 = 错误**
- **正数 = 特殊成功标记**

---

## 附录 C：配置项表（`lv_conf.h` 关键项）

| 配置项                     | 建议值                   | 影响                  |
| -------------------------- | ------------------------ | --------------------- |
| `LV_COLOR_DEPTH`           | 16 或 32                 | 颜色精度              |
| `LV_USE_LOG`               | 1（Debug）/ 0（Release） | 是否转发日志到仓颉    |
| `LV_MEM_SIZE`              | ≥ 64 KB                  | LVGL 内存池           |
| `LV_DRAW_BUF_STRIDE_ALIGN` | 由 LVGL 默认             | 缓冲大小计算必须考虑  |
| `LV_USE_PROFILER`          | 1                        | 性能计数器            |
| `LV_USE_MEM_MONITOR`       | 1                        | 内存监控              |
| `LV_USE_ASSERT_*`          | Debug 开                 | 提前暴露问题          |
| `LV_USE_FS_POSIX`          | 1                        | 文件系统              |
| `LV_USE_PNG/JPG`           | 按需                     | 引入新依赖，需入 SBOM |
| `LV_FONT_DEFAULT`          | 内置或外部               | 字体体积影响 Flash    |
| `LV_USE_SDL`               | 0（MVP 手写）            | 见 §8.1.6             |
| `LV_DEF_REFR_PERIOD`       | 33 ms（~30 FPS）         | 刷新周期              |

---

## 附录 D：P0 速查卡

**这一页是 P0 参与者的唯一入口。**

### D.1 第 1 周必须回答的 7 个问题

| #        | 问题                                                         | 命令 / 方法                                     | 结论填写                                                     |
| -------- | ------------------------------------------------------------ | ----------------------------------------------- | ------------------------------------------------------------ |
| **V1**   | 仓颉能否在 C 创建的 OS 线程里执行闭包？                      | 查 SDK 文档 / 问社区 / 写探针                   | ⬜ A 可行 / ⬜ 不可行 → 用方案 C                               |
| **V2-a** | 仓颉能否编译"捕获闭包 → C 函数指针"？                        | 写最小例子：`let cb: CFunc = { => useLocal() }` | ⬜ 能 / ⬜ 不能（→ 分支 B）                                    |
| **V2-b** | 若能，C 侧能否长期持有（GC 保活）？                          | 注册到 C，强制 GC，再调用                       | ⬜ 能 **且** 有 pin API（→ 分支 A） / ⬜ 不能或 pin API 不明确（→ 分支 B） |
| **V3**   | 仓颉轻量级线程会迁移 OS 线程吗？                             | 跑 §3.6.4 探针                                  | ⬜ = 1 / ⬜ > 1                                                |
| **V4**   | ASan 与仓颉 GC 能共存吗？                                    | 人为制造 C 侧泄漏，看 ASan 是否报出             | ⬜ 能 / ⬜ 不能 → 用 valgrind                                  |
| **V5**   | `cjpm.toml` 的 `[ffi.c]` 字段可用吗？                        | 照抄 CJQT6 模板编译                             | ⬜ 可用 / ⬜ 需调整                                            |
| **V6**   | SDL2 flush_wait 与主线程渲染同步可行吗？                     | 写最小 SDL2 双线程例子，验证信号量 + 超时       | ⬜ 可行 / ⬜ 需调整方案（**方案 C 下跳过**）                   |
| **V7**   | `deleted_cb` 三路径（自然结束/手动删除/对象删除）是否都触发？ | 写最小 LVGL 例子，三条路径分别打印              | ⬜ 全触发 / ⬜ 不全 → **加 anim 句柄表兜底**                   |

**V2 三选项说明**：
- V2-a 不通过 → 分支 B
- V2-a 通过但 V2-b 不通过 → 分支 B
- V2-a + V2-b 通过但 pin API 不明确 → 分支 B
- V2-a + V2-b 通过且 pin API 明确 → 分支 A（备选）

### D.2 必读五章

| 章节                 | 为什么必读                                     |
| -------------------- | ---------------------------------------------- |
| **§3.2 回调桥接**    | 分支 B 默认；分支 A 的 pin 表机制与警告        |
| **§3.6 线程模型**    | 方案 A/C 选型直接决定代码结构                  |
| **§3.8 重入 + 死锁** | 延迟删除 + 等待图检测是 P0 压力测试核心        |
| **§3.10 动画**       | anim_ctx 方案 + internal deleted_cb 唯一释放点 |
| **§3.4 绘制缓冲**    | 缓冲所有权是花屏/段错误的最大来源              |

### D.3 第一个可交付物

见 §17：`examples/hello_cj` + 八个断言。

### D.4 P0 压力测试

```
button → clicked → 删除自己 → 重建
```

跑不稳就不许加控件。

### D.5 禁止事项

| ❌ 禁止                                | 原因                                        |
| ------------------------------------- | ------------------------------------------- |
| V1/V2 出结论前写 §3.2 / §3.6 的代码   | 设计可能整体重写                            |
| 把仓颉 `Array<UInt8>` 指针传给 LVGL   | GC 移动导致花屏                             |
| 回调内同步等待 `post`                 | 死锁                                        |
| 回调内直接删对象不走延迟队列          | use-after-free                              |
| Release 构建关闭线程检查              | 间歇性数据竞争                              |
| **分支 A 下把闭包指针给 C 却不 pin**  | **GC 回收导致崩溃**                         |
| **把闭包指针放进 C 全局数组冒充 pin** | **C 数组不在 GC 根集，闭包仍会被回收**      |
| **flush_wait_cb 无限阻塞**            | **窗口关闭时挂死**                          |
| **用户手动 free anim_ctx**            | **内部 deleted_cb 是唯一释放点，双重 free** |

---

**文档结束**

> **v0.4 冻结版** = v0.4 + Patch P1（anim_ctx 泄漏）+ Patch P2（pin 表警告）+ 次生残留处置。
>
> 核心立场：
> 1. **§3.2 / §3.6 / §3.8 / §3.9 / §3.10 的代码在 V1/V2/V7 出结论前不动工**
> 2. **本版冻结，不开 v0.5**——零可运行代码下的纯设计迭代收敛速度递减
> 3. **下一步是写探针（V1–V7），不是写文档**——用实测替代假设，再一次性修订
>
> 最重要的元判断：**每轮能发现的都是想得到的，留下的都是想得到但没想到后果的。** v0.3 修 E3 引入死锁、v0.4 修 I2 引入 ctx 泄漏，这不是巧合，是纯设计迭代的固有衰减。让探针终结这个循环。