# lvgl4cj 设计方案
## 仓颉语言绑定 LVGL 的 GUI 框架

> 版本：v0.1（设计稿）
> 目标 LVGL 版本：**v9.x**（主线）
> 目标仓颉 SDK：**1.1.0**
> 定位：面向 **Linux/ARM64 Linux/OpenHarmony 的嵌入式 HMI**，桌面 SDL2 作为开发与验证环境

---

## 〇、一句话结论

**把项目定义为"受控 C ABI 工程"，而不是"自动生成 C 绑定的练习"。**

LVGL 是纯 C 库，ABI 层面比 Qt/C++ 简单得多；真正的工程量集中在三件事：**回调桥接、对象生命周期级联失效、绘制缓冲的所有权**。这三点做对了，控件覆盖率只是时间问题；这三点做错了，控件越多崩得越快。

一个关键利好：**LVGL 的 `lv_obj_add_event_cb(obj, cb, filter, user_data)` 原生支持 `void* user_data`**，且允许同一回调配不同 user_data 重复注册。相比之下 Qt 的信号槽没有 user_data 概念，CJQT6 被迫做复合键 `(ptr, signalId)`。**LVGL 在这点上天生更适合绑定。**

---

## 一、项目命名与定位

### 1.1 命名

**`lvgl4cj`**，仓颉包名 `lvgl4cj`，命令行/目录名 `lvgl4cj`。

理由：与 TPC 现有命名风格一致——`lrc4cj`（LRC 歌词解析）、`vlayout4cj`（虚拟布局）。进 TPC 或中心仓时辨识度高，搜索 `4cj` 可归入同一族。

### 1.2 分层定位

| | 内容 |
|---|---|
| **是** | LVGL 的仓颉绑定层：安全 API + 生命周期管理 + 驱动后端 |
| **不是** | 新的 UI 框架（不重新设计控件体系）、不是声明式 UI（不造 ArkUI 竞品） |
| **不追求** | 100% API 覆盖；先做 80% 常用路径 |
| **明确不做** | 不做渲染引擎替换；不重写 LVGL 内部机制 |

### 1.3 设计目标（按优先级）

1. **安全**：句柄失效可检测，不出现悬空指针与 double-free
2. **可调试**：错误有明确信息，崩溃可定位到仓颉调用点
3. **够用**：覆盖 HMI 常见控件与交互
4. **快**：UI 线程不被阻塞，FFI 开销可控
5. 美观的仓颉风格 API（在前四条满足后再优化）

### 1.4 非目标（明确排除，避免范围蔓延）

- ❌ 裸机 / MCU / RTOS 支持（**第二阶段才评估**，见 §10 决策门）
- ❌ 多线程并发调用 LVGL（MVP 采用单 LVGL 线程）
- ❌ 自动生成的全量 API（宏与回调部分必须手写）

---

## 二、总体架构：四层

```
┌─────────────────────────────────────────────────────────┐
│  L0  应用层（用户仓颉代码）                                │
│      lvgl4cj 示例 / 用户 HMI 程序                         │
└───────────────────┬─────────────────────────────────────┘
                    │  仓颉调用
┌───────────────────▼─────────────────────────────────────┐
│  L1  仓颉安全 API 层      src/                            │
│      LvObject / LvStyle / LvEvent / LvTimer              │
│      LvDisplay / LvIndev / LvglRuntime                    │
│      · 句柄封装，不暴露裸指针                              │
│      · 生命周期：Resource 接口 + close()                   │
│      · 样式 DSL、事件 DSL                                  │
│      · 线程调度：post { } 投递到 LVGL 线程                 │
└───────────────────┬─────────────────────────────────────┘
                    │  C ABI（扁平、稳定、无 C++ 符号）
┌───────────────────▼─────────────────────────────────────┐
│  L2  C 桥接层            native/  → liblvgl4cj_bridge    │
│      · 句柄表（obj ↔ int64 双向映射）                     │
│      · 回调 trampoline（固定 C 函数 + user_data 分发）     │
│      · 宏 → 常量函数（lv_conf 查询）                       │
│      · 绘制缓冲分配与持有                                  │
│      · 错误码 / 日志回调                                   │
└───────────────────┬─────────────────────────────────────┘
                    │  原生 C 调用
┌───────────────────▼─────────────────────────────────────┐
│  L3  LVGL 原生           third_party/lvgl                │
│      锁定版本 + 固定 lv_conf.h + 补丁                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  L4  后端层（可插拔，独立于 L1/L2）                        │
│      backend_sdl2     Linux/macOS 桌面（MVP）             │
│      backend_fbdev    Linux framebuffer                  │
│      backend_drm      Linux DRM/GBM                      │
│      backend_oh       OpenHarmony（二期）                 │
│      backend_custom   自定义 flush/read 回调              │
└─────────────────────────────────────────────────────────┘
```

**为什么要 L2 这一层，不能直接 FFI 调 `lv_*`？**

| 直接 FFI 的问题 | L2 桥接层解决方式 |
|---|---|
| `lv_obj_t*` 裸指针，无失效检测 | 句柄表 + 存活标记 |
| 回调必须是 C 函数指针，仓颉闭包传不进去 | 固定 trampoline + user_data 分发 |
| `lv_conf.h` 是编译期宏，仓颉读不到 | 桥接层暴露查询函数 + 配置哈希 |
| 绘制缓冲必须固定地址、不被 GC 移动 | C 侧 `malloc` 并持有 |
| 父删子时子句柄变悬空 | `LV_EVENT_DELETE` 钩子级联失效 |
| 错误靠 assert，仓颉侧无感知 | 统一错误码 + 日志回调 |

---

## 三、六个核心机制设计（本方案的重点）

### 3.1 句柄表：不暴露裸指针

**所有 `lv_obj_t*` / `lv_display_t*` / `lv_indev_t*` / `lv_style_t*` 在仓颉侧一律表示为 `Int64` 句柄。**

```c
// native/handle_table.h
// 双向映射：原生指针 ↔ int64 句柄
int64_t lvglcj_handle_of(void *ptr);        // 指针 → 句柄（不存在则分配）
void   *lvglcj_ptr_of(int64_t handle);      // 句柄 → 指针（失效返回 NULL）
int     lvglcj_handle_alive(int64_t handle);
void    lvglcj_handle_invalidate(int64_t handle);
void    lvglcj_handle_release(int64_t handle);
```

设计要点：

- 句柄 ≠ 指针地址。**句柄是自增 ID**，避免把地址当句柄导致"地址复用误判存活"的经典 bug
- `ptr_of` 返回 `NULL` 时，仓颉侧抛明确的 `LvglException("对象已被删除")`，而不是崩溃
- 句柄表本身是 C 侧的哈希表，不经过仓颉 GC

仓颉侧：

```cangjie
public class LvObject <: Resource {
    private let handle: Int64
    private var closed: Bool = false

    public func isAlive(): Bool {
        lvglcj_handle_alive(this.handle) != 0
    }

    public func close(): Unit {
        if (this.closed) { return }
        if (this.isAlive()) { lvglcj_obj_delete(this.handle) }
        lvglcj_handle_release(this.handle)
        this.closed = true
    }
}
```

### 3.2 回调桥接：trampoline + user_data 分发

**这是全项目最关键的一环。** LVGL 的回调是 C 函数指针，仓颉闭包无法直接传入。

方案：**一个固定的 C trampoline + 全局闭包注册表**。

```c
// native/callback.c

// LVGL 事件回调签名：void (*)(lv_event_t *)
static void lvglcj_event_trampoline(lv_event_t *e) {
    // user_data 里塞的是 closure_id
    int64_t cid = (int64_t)(intptr_t)lv_event_get_user_data(e);
    int64_t obj_handle = lvglcj_handle_of(lv_event_get_target(e));
    int32_t code = (int32_t)lv_event_get_code(e);
    // 回调进仓颉侧
    lvglcj_dispatch_event(cid, obj_handle, code, e);
}
```

注册流程：

```
仓颉 obj.on(Event.Clicked) { e => ... }
   │
   ├─ 闭包存入全局表 gClosures，得到 closure_id (Int64)
   │
   ├─ 调 lvglcj_obj_add_event(objHandle, code, (void*)closure_id)
   │
   └─ C 侧调 lv_obj_add_event_cb(obj, lvglcj_event_trampoline, code, (void*)cid)
```

**关键细节**：

1. **仓颉侧的回调必须是顶层 `@C func`**，不能捕获局部变量——这是仓颉 FFI 的硬约束。因此 `on { ... }` 语法糖内部必须把闭包放进全局表，并传 ID。
2. **user_data 用于传 closure_id，不传裸指针**。一个对象可以注册多个事件，每个事件一个 closure_id。
3. **注销必须显式**：`lv_obj_remove_event_cb_with_user_data(obj, trampoline, (void*)cid)`。删除对象时 C 侧要注销该对象所有 closure_id，并从全局表移除闭包，否则闭包泄漏。
4. **异常不能跨越 C 边界**：仓颉闭包内抛出的异常必须在 trampoline 边界捕获，转成日志。

```c
static void lvglcj_event_trampoline(lv_event_t *e) {
    int64_t cid = (int64_t)(intptr_t)lv_event_get_user_data(e);
    // ★ 仓颉侧回调在这里被调用，异常被吞掉并记录
    int rc = lvglcj_call_closure(cid, e);
    if (rc != 0) {
        LV_LOG_ERROR("仓颉回调抛异常, closure_id=%d", (int)cid);
    }
}
```

同类机制还需覆盖：`lv_timer` 回调、`lv_display` flush 回调、`lv_indev` read 回调、日志回调、动画回调。每类一个 trampoline。

### 3.3 生命周期级联失效：借 `LV_EVENT_DELETE`

**LVGL 的父对象删除会连带删除所有子对象。** 仓颉侧如果只做 RAII，父删了子句柄还"活着"，一用就崩。

方案：**对象创建时自动注册 `LV_EVENT_DELETE` 钩子，删除时递归失效子句柄。**

```c
static void lvglcj_delete_hook(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    int64_t h = lvglcj_handle_of(obj);

    // 1. 递归失效所有子对象句柄
    lvglcj_invalidate_subtree(obj);

    // 2. 注销该对象所有事件闭包（顺序：先注销回调，再失效句柄）
    lvglcj_unregister_closures(h);

    // 3. 最后失效自身句柄
    lvglcj_handle_invalidate(h);
}

// 统一的对象创建入口，自动挂钩子
int64_t lvglcj_obj_create(int64_t parent_handle) {
    lv_obj_t *parent = lvglcj_ptr_of(parent_handle);
    lv_obj_t *obj = lv_obj_create(parent);
    int64_t h = lvglcj_handle_of(obj);
    lv_obj_add_event_cb(obj, lvglcj_delete_hook, LV_EVENT_DELETE, NULL);
    return h;
}
```

**为什么这个设计重要**：它把"C 侧对象树"和"仓颉侧句柄表"两个生命周期系统对齐了。CJQT6 用 `QObject::destroyed` 信号做同样的事（qTrackObject / qIsObjectAlive），LVGL 的等价物就是 `LV_EVENT_DELETE`。

**顺序铁律**：注销回调 → 失效子句柄 → 失效自身句柄。反了会在删除过程中触发已失效的闭包。

### 3.4 绘制缓冲：必须在 C 侧分配

**LVGL v9 的 `lv_display_set_buffers(display, buf1, buf2, buf_size_byte, mode)` 接收裸缓冲区指针，且 LVGL 会在 flush 期间长期持有。**

⚠️ **绝对不能把仓颉 `Array<UInt8>` 的底层指针传给 LVGL** —— 仓颉 GC 可能移动或回收它，导致花屏或段错误。

方案：**缓冲在 C 侧 `malloc`，由桥接层持有，仓颉侧只拿句柄。**

```c
// 一步到位：创建 display 并分配缓冲
int64_t lvglcj_display_create(int32_t w, int32_t h,
                              int32_t buf_bytes, int32_t render_mode) {
    lv_display_t *disp = lv_display_create(w, h);
    uint8_t *buf1 = (uint8_t *)malloc(buf_bytes);
    // 双缓冲时 buf2 也分配
    uint8_t *buf2 = (render_mode == LV_DISPLAY_RENDER_MODE_FULL) ? malloc(buf_bytes) : NULL;

    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, render_mode);

    int64_t h_disp = lvglcj_handle_of(disp);
    // 缓冲指针与 display 句柄绑定，销毁时一起 free
    lvglcj_bind_buffer(h_disp, buf1, buf2);
    return h_disp;
}
```

推荐默认参数：`buf_bytes = w * h / 10 * bytes_per_pixel`（LVGL 官方建议至少 1/10 屏），`mode = PARTIAL`。

双缓冲（DIRECT/FULL）能显著改善流畅度，`buf2` 与 `buf1` 等大，由桥接层一并管理释放。

### 3.5 `lv_conf.h` 版本化：宏 → 查询函数

**LVGL 大量行为由编译期宏决定**（`LV_COLOR_DEPTH`、`LV_USE_*`、`LV_MEM_SIZE`、`LV_USE_LOG`…）。仓颉侧读不到宏，会造成"绑定层与 LVGL 构建配置不一致"的隐蔽 bug。

方案：**桥接层把关键宏转成常量查询函数 + 整体配置哈希。**

```c
// native/conf_probe.c
int32_t lvglcj_conf_color_depth(void)  { return LV_COLOR_DEPTH; }
int32_t lvglcj_conf_use_log(void)      { return LV_USE_LOG; }
int32_t lvglcj_conf_mem_size(void)     { return LV_MEM_SIZE; }
int32_t lvglcj_conf_default_font(void) { return LV_FONT_DEFAULT_HAS_...; }
uint32_t lvglcj_conf_hash(void);   // 所有关键宏拼接后做 FNV-1a 哈希
const char *lvglcj_version(void);  // LVGL_VERSION_MAJOR/MINOR/PATCH
```

仓颉侧启动时校验：

```cangjie
func checkCompatibility(): Unit {
    let expectedHash = readBuildTimeHash()      // 编译期写入
    let actualHash = lvglcj_conf_hash()
    if (expectedHash != actualHash) {
        throw LvglException(
            "LVGL 构建配置不匹配：绑定层基于 hash=${expectedHash}，" +
            "当前库 hash=${actualHash}。请重新编译 native 层。"
        )
    }
}
```

**价值**：把"莫名其妙花屏/崩溃"变成"启动时一条明确报错"。这是低成本高收益的设计。

### 3.6 线程模型：单 LVGL 线程 + 任务投递

LVGL 不是线程安全的，官方模型要求 `lv_timer_handler()` 在主循环定期调用，且所有 UI 操作最好在同一线程。

方案：**`LvglRuntime` 起一个专用线程跑主循环，仓颉侧 UI 操作通过队列投递。**

```cangjie
public class LvglRuntime {
    private let tickThread: Thread
    private let taskQueue: BlockingQueue<() -> Unit>
    private var running: Bool = true

    public func start(): Unit {
        lvglcj_init()                      // lv_init()
        lvglcj_set_tick_cb()               // lv_tick_set_cb(C 侧高精度时钟)
        lvglcj_set_log_cb(onLvglLog)       // 日志转发到仓颉
        // 专用线程跑主循环
        this.tickThread = spawn {
            while (this.running) {
                this.drainTasks()          // 先执行投递过来的仓颉任务
                lvglcj_timer_handler()     // lv_timer_handler()
                sleep(5.milliseconds)      // 官方示例也是 5ms
            }
        }
    }

    // 所有 UI 操作走这里
    public func post(task: () -> Unit): Unit {
        this.taskQueue.put(task)
    }

    public func stop(): Unit {
        this.running = false
        this.tickThread.join()
        lvglcj_deinit()
    }
}
```

**铁律**：
- 任何 `lv_obj_*` / `lv_style_*` 调用都必须在 LVGL 线程内，或是通过 `post { }`
- 回调（事件/timer/flush）已在 LVGL 线程内，**回调内禁止再 `post` 并同步等待** —— 会死锁
- MVP 不提供多线程直调 LVGL 的接口

---

## 四、目录结构

```
lvgl4cj/
├── README.md
├── LICENSE                        # 建议 Apache-2.0 或 MIT
├── NOTICE
├── cjpm.toml
├── src/                           # ── L1 仓颉安全 API 层 ──
│   ├── main.cj                    # 仅示例入口；库本身不强制
│   ├── lvgl.cj                    # 顶层门面：init / version / 常量
│   ├── runtime.cj                 # LvglRuntime：线程、主循环、任务投递
│   ├── handle.cj                  # 句柄封装与 Resource 语义
│   ├── error.cj                   # LvglException、错误码
│   ├── core/
│   │   ├── object.cj              # LvObject：创建/删除/父子/位置/尺寸
│   │   ├── display.cj             # LvDisplay：create/buffers/flush 回调
│   │   ├── indev.cj               # LvIndev：read 回调、指针/键盘/编码器
│   │   ├── event.cj               # LvEvent、事件码枚举、on() DSL
│   │   ├── style.cj               # LvStyle + 样式 DSL
│   │   ├── timer.cj               # LvTimer
│   │   └── theme.cj               # 内置主题
│   ├── widgets/
│   │   ├── label.cj
│   │   ├── button.cj              # v9 命名：button（v8 是 btn）
│   │   ├── slider.cj
│   │   ├── switch.cj
│   │   ├── checkbox.cj
│   │   ├── image.cj               # v9 命名：image（v8 是 img）
│   │   ├── chart.cj
│   │   ├── table.cj
│   │   ├── roller.cj
│   │   └── keyboard.cj
│   ├── layout/
│   │   ├── flex.cj
│   │   └── grid.cj
│   ├── ffi/
│   │   └── bridge.cj              # ★ 所有 foreign 声明集中在此
│   └── util/
│       ├── color.cj
│       └── mem.cj                 # lv_mem_monitor 封装
├── native/                        # ── L2 C 桥接层 ──
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── lvglcj_bridge.h
│   ├── src/
│   │   ├── handle_table.c
│   │   ├── callback.c             # trampoline 集中地
│   │   ├── lifecycle.c            # LV_EVENT_DELETE 钩子
│   │   ├── display.c
│   │   ├── indev.c
│   │   ├── obj.c
│   │   ├── style.c
│   │   ├── timer.c
│   │   ├── conf_probe.c           # 宏 → 查询函数
│   │   └── log.c
│   └── generated/                 # 由脚本生成的批量声明
├── backend/                       # ── L4 后端层 ──
│   ├── sdl2/                      # MVP：Linux/macOS 桌面
│   ├── fbdev/
│   └── oh/                        # 二期：OpenHarmony
├── third_party/
│   └── lvgl/                      # 锁定版本 + lv_conf.h 快照
├── examples/
│   ├── hello_cj/                  # 最小可跑示例
│   ├── widgets_demo/              # 控件展示
│   └── hmi_panel/                 # 模拟工业面板（综合验证）
├── test/
│   ├── ffi_contract_test.cj       # 结构体布局、字段偏移
│   ├── handle_test.cj             # 句柄失效
│   ├── callback_test.cj           # 回调注册/注销/重入
│   └── soak_test.cj               # 24h 稳定性
└── scripts/
    ├── build_native.sh
    ├── build_native.ps1
    ├── gen_bindings.py            # 头文件 → 声明生成
    └── run_debug.ps1              # Qt/SDL 运行时 PATH 处理
```

---

## 五、C ABI 桥接层接口设计

**命名约定**：`lvglcj_<子系统>_<动作>`，全部 `extern "C"`，返回值统一 `int32_t` 错误码（0 = 成功），输出走 out 参数或返回值。

### 5.1 生命周期与运行时

```c
int32_t lvglcj_init(void);
int32_t lvglcj_deinit(void);
int32_t lvglcj_timer_handler(void);          // 包装 lv_timer_handler()
void    lvglcj_set_tick_cb(void);            // 内部注册 lv_tick_set_cb
const char *lvglcj_version(void);
uint32_t lvglcj_conf_hash(void);
```

### 5.2 句柄

```c
int64_t lvglcj_handle_of(void *ptr);
int32_t lvglcj_handle_alive(int64_t h);
void    lvglcj_handle_invalidate(int64_t h);
void    lvglcj_handle_release(int64_t h);
```

### 5.3 Display

```c
// 创建 display 并分配绘制缓冲（缓冲由 C 侧持有）
int64_t lvglcj_display_create(int32_t w, int32_t h,
                              int32_t buf_bytes, int32_t render_mode);
int32_t lvglcj_display_set_flush_cb(int64_t disp, int64_t closure_id);
int32_t lvglcj_display_set_flush_wait_cb(int64_t disp, int64_t closure_id);
void    lvglcj_display_flush_ready(int64_t disp);
int32_t lvglcj_display_flush_is_last(int64_t disp);
int32_t lvglcj_display_set_resolution(int64_t disp, int32_t w, int32_t h);
int32_t lvglcj_display_delete(int64_t disp);
```

### 5.4 InDev

```c
typedef enum { INDEV_POINTER, INDEV_KEYPAD, INDEV_ENCODER, INDEV_BUTTON } lvglcj_indev_type_t;

int64_t lvglcj_indev_create(int32_t type);
int32_t lvglcj_indev_set_read_cb(int64_t indev, int64_t closure_id);
int32_t lvglcj_indev_set_display(int64_t indev, int64_t disp);
int32_t lvglcj_indev_delete(int64_t indev);
```

### 5.5 对象

```c
int64_t lvglcj_obj_create(int64_t parent);        // 自动挂 DELETE 钩子
int32_t lvglcj_obj_delete(int64_t obj);
int32_t lvglcj_obj_set_pos(int64_t obj, int32_t x, int32_t y);
int32_t lvglcj_obj_set_size(int64_t obj, int32_t w, int32_t h);
int32_t lvglcj_obj_set_parent(int64_t obj, int64_t parent);
int64_t lvglcj_screen_active(void);
int32_t lvglcj_screen_load(int64_t scr);
int32_t lvglcj_obj_add_flag(int64_t obj, int32_t flag);
int32_t lvglcj_obj_add_state(int64_t obj, int32_t state);
```

### 5.6 事件

```c
// 注册事件回调；closure_id 作为 user_data 传入 trampoline
int64_t lvglcj_obj_add_event(int64_t obj, int32_t code, int64_t closure_id);
int32_t lvglcj_obj_remove_event(int64_t obj, int64_t event_dsc_handle);
int32_t lvglcj_event_get_code(int64_t event_handle);
int64_t lvglcj_event_get_target(int64_t event_handle);
int64_t lvglcj_event_get_user_data(int64_t event_handle);
```

### 5.7 样式

```c
int64_t lvglcj_style_create(void);
int32_t lvglcj_style_delete(int64_t style);
// v9 样式 API 已去掉 state 参数，改用 selector
int32_t lvglcj_style_set_bg_color(int64_t style, uint32_t rgb888);
int32_t lvglcj_style_set_bg_opa(int64_t style, int32_t opa);
int32_t lvglcj_style_set_radius(int64_t style, int32_t r);
int32_t lvglcj_style_set_pad_all(int64_t style, int32_t pad);
int32_t lvglcj_style_set_border_width(int64_t style, int32_t w);
int32_t lvglcj_obj_add_style(int64_t obj, int64_t style, int32_t selector);
int32_t lvglcj_obj_remove_style(int64_t obj, int64_t style, int32_t selector);
```

### 5.8 Timer / 内存 / 日志

```c
int64_t lvglcj_timer_create(int64_t closure_id, int32_t period_ms);
int32_t lvglcj_timer_delete(int64_t timer);
int32_t lvglcj_timer_pause(int64_t timer);
int32_t lvglcj_timer_resume(int64_t timer);

int32_t lvglcj_mem_monitor(uint32_t *used, uint32_t *frag, uint32_t *max_used);
int32_t lvglcj_set_log_cb(int64_t closure_id);
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

    // SDL2 后端：创建 display + 输入设备
    let disp = Sdl2Backend.createDisplay(800, 480)
    let indev = Sdl2Backend.createPointer(disp)

    // 建 UI
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

    runtime.runForever()      // 内部循环，直到调用 stop
}
```

### 6.2 样式 DSL

LVGL 的样式是命令式（`lv_style_init` + 一堆 `lv_style_set_*`）。仓颉侧包一层 builder，可读性提升明显：

```cangjie
let cardStyle = LvStyle {
    bgColor(0xFF2A2D3E)
    bgOpa(255)
    radius(12)
    padAll(16)
    borderWidth(0)
}

let dangerStyle = LvStyle {
    bgColor(0xFFE5484D)
    textColor(0xFFFFFFFF)
    radius(8)
}

card.apply(cardStyle, Part.Main)              // 默认状态
card.apply(dangerStyle, Part.Main | State.Pressed)   // 按下时
```

### 6.3 事件 DSL 与回调约束

⚠️ **仓颉 FFI 回调不能捕获局部变量**（顶层 `@C func` 约束）。`on { }` 语法糖内部处理：

```cangjie
// 用户写法（捕获 count）
btn.on(Event.Clicked) { e => count += 1 }

// 内部等价实现：
//   1. 闭包存入 gClosures，拿到 closure_id
//   2. 调 lvglcj_obj_add_event(btn.handle, code, closure_id)
//   3. trampoline 用 user_data 里的 closure_id 找回闭包执行
```

暴露给用户的 `on` 返回值是 `LvEventHandle`，用于注销：

```cangjie
let h = btn.on(Event.Clicked) { e => ... }
h.remove()        // 显式注销，闭包从全局表移除
```

**必须显式注销的原因**：闭包在全局表里，不注销就是泄漏。对象删除时 C 侧会自动注销，但手动 `remove()` 更可控。

### 6.4 布局

```cangjie
let row = LvObject.create(scr)
row.setSize(480, 200)
row.setFlexFlow(FlexFlow.Row)
row.setFlexAlign(FlexAlign.SpaceEvenly, FlexAlign.Center, FlexAlign.Center)

for (i in 0..4) {
    let item = LvButton.create(row)
    item.setSize(80, 40)
}
```

### 6.5 错误处理

统一 `LvglException`，携带错误码与发生位置：

```cangjie
public enum LvglError {
    Ok
    | InvalidHandle        // 句柄已失效（对象被删除）
    | NotInitialized       // 未调用 lv_init
    | InvalidConfig        // lv_conf 不匹配
    | CallbackThrew        // 仓颉回调抛异常（已被吞掉并记录）
    | OutOfMemory          // LVGL 内存池耗尽
    | BackendFailure       // 后端（SDL/DRM）失败
}
```

---

## 七、后端适配

MVP 只做 **SDL2**，但接口按可插拔设计。

```cangjie
public interface LvglBackend {
    func createDisplay(w: Int32, h: Int32): LvDisplay
    func createPointer(disp: LvDisplay): LvIndev
    func createKeypad(disp: LvDisplay): LvIndev
    func destroy(): Unit
}
```

### SDL2 后端要点

- LVGL v9 **内置 SDL 驱动**（`lv_conf.h` 开 `LV_USE_SDL`），但**建议手写 flush/read 回调**而非直接用内置驱动——前者能验证完整的回调链路，后者绕过了最需要验证的部分
- flush 回调签名（v9）：`(lv_display_t*, const lv_area_t*, uint8_t* px_map)`
  - ⚠️ v9 第三个参数是 `uint8_t*`，**不是 v8 的 `lv_color_t*`**。像素格式取决于 `lv_display_set_color_format`，必须显式确认，否则"偶尔花屏"
  - 回调结束必须调 `lv_display_flush_ready()`
- read 回调：`(lv_indev_t*, lv_indev_data_t*)`，填 `point.x/y` 与 `state`
- tick：`lv_tick_set_cb` 注册 C 侧毫秒时钟

### 后续后端

| 后端 | 场景 | 优先级 |
|---|---|---|
| SDL2 | Linux/macOS 桌面开发验证 | **P0** |
| fbdev | Linux 无 X 环境 | P2 |
| DRM/GBM | Linux ARM64 HMI | P2 |
| OpenHarmony | OH 设备 | P3 |

---

## 八、MVP 路线

### P0：跑通闭环（验收标准是"五件事实成立"，不是控件数量）

| # | 任务 | 验收事实 |
|---|---|---|
| 1 | `lv_init` / tick / `lv_timer_handler` 主循环 | Linux 上稳定刷新一帧 |
| 2 | SDL2 display flush + pointer indev | 按钮可点击，坐标正确 |
| 3 | 对象树：`create` / `delete` / 父子关系 | 句柄失效可检测，无悬空调用 |
| 4 | 事件回调 + timer 回调 | 能注册、能注销、不崩溃 |
| 5 | 样式最小集 + Flex 布局 | 布局结果符合预期 |

**P0 的核心压力测试**：`button → clicked → 删除自己 → 重建`。这个跑不稳，加一百个控件只会放大设计缺陷。

### P1：可用

- 10~12 个常用控件：label / button / slider / switch / checkbox / image / chart / table / roller / keyboard / dropdown / bar
- `lv_conf` schema + 预编译预设 + hash 校验
- 四平台 CI + ASan/LSan
- 样式 DSL、事件 DSL
- `examples/widgets_demo`

### P2：工程化

- 绑定生成器（从 LVGL 头文件批量生成简单 setter/getter）
- 移植 ARM64 Linux + DRM 后端
- 性能基线：帧率、内存、FFI 调用开销
- `examples/hmi_panel` 综合示例

### P3：外延（需先过决策门）

- OpenHarmony 后端
- MCU / RTOS 评估（见 §10）

---

## 九、测试策略

| 层级 | 内容 | 手段 |
|---|---|---|
| **FFI 契约** | `lv_area_t` / `lv_point_t` 字段偏移与 padding；枚举值一致性 | C 侧与仓颉侧各算一次 offsetof，断言相等 |
| **句柄** | 父删后子句柄是否失效；重复 close 是否安全 | 单测 |
| **回调** | 注册/注销/重入/异常吞掉 | 单测 + 压测 |
| **生命周期** | 创建 1 万对象再删除，句柄表是否零残留 | 压测 |
| **内存** | `lv_mem_monitor` 长期曲线是否收敛 | 24h soak + ASan |
| **稳定性** | 连续运行 24 小时不崩、不涨 | soak test |
| **视觉** | flush 结果截图比对 | SDL2 截图 + 像素 diff（P2） |

**必须接入 ASan / LeakSanitizer**——FFI 边界的内存问题靠 review 看不出来。

---

## 十、风险与决策门

| 风险 | 等级 | 应对 |
|---|---|---|
| 仓颉 FFI 回调限制导致闭包方案复杂 | 中 | 全局闭包表 + user_data 传 ID（§3.2），P0 即验证 |
| 绘制缓冲被 GC 移动导致花屏 | **高** | C 侧 malloc 持有（§3.4），禁止传仓颉数组指针 |
| 父删子导致悬空句柄 | **高** | `LV_EVENT_DELETE` 钩子级联失效（§3.3） |
| `lv_conf` 配置不匹配 | 中 | 编译期 hash + 启动校验（§3.5） |
| 回调内异常跨越 C 边界 | 中 | trampoline 捕获并记录（§3.2） |
| LVGL 版本升级破坏 API | 中 | 锁定版本 + 版本快照，升级走独立分支 |
| 控件 API 数量大，手写不完 | 中 | 生成器处理 setter/getter，手写核心与回调 |
| **仓颉运行时不支持 MCU** | **高（若目标是 MCU）** | **见下方决策门** |

### 决策门 G1：是否进入 MCU/RTOS

**在 P2 完成后、投入 MCU 之前，必须先完成运行时基准测试：**

| 测量项 | 门槛（建议） |
|---|---|
| 仓颉运行时静态 footprint | 与目标板 Flash/ROM 对比 |
| 堆峰值 | 目标板 RAM 的 50% 以内 |
| GC 停顿 | < 16ms（否则动画卡顿） |
| 启动时间 | 满足产品冷启动要求 |
| 静态链接可行性 | 目标板无包管理时能否部署 |
| libc/OS 依赖 | 裸机或最小 RTOS 能否运行 |

**若任一关键项不达标 → 不要做 MCU 版本。** 此时更现实的方案是 **C 主控 + 仓颉跑业务逻辑**（仓颉负责数据/网络/协议，C 侧 LVGL 负责渲染），而不是强行让仓颉驱动 LVGL 上 MCU。

⚠️ **这个门必须在立项时写清楚**。LVGL 官方最低资源是 16KB RAM / 64KB Flash 级别，而仓颉运行时带 GC、线程、标准库，**完整进程占用与"LVGL 核心很轻"完全不是一个口径**。不能因为"LVGL 很轻"就推断"仓颉 + LVGL 能上 MCU"。

---

## 十一、许可与治理

- **LVGL 主线当前为 MIT 许可**（v8/v9 已改，非 GPLv3）。但必须锁定 commit 并核对 `LICENCE.txt`
- 绑定层建议 **Apache-2.0 或 MIT**，与 LVGL 分离
- 从第一天起维护：`third_party/lvgl` 保留原始许可与版本哈希；CI 生成 SPDX/CycloneDX SBOM
- 若启用 GPL 组件或选 GPLv3 历史版本，需重新评估 copyleft 义务

### 准入路径

1. GitCode 公开独立原型，明确 owner / 版本 / 许可 / 构建指南
2. 锁定 LVGL 版本 + `lv_conf` schema，提供 SDL2 示例与 CI
3. 申请进入 **Cangjie-SIG** 孵化
4. 稳定性、测试、许可、文档达标后争取纳入 **Cangjie-TPC** 并发布中心仓制品

### 制品拆分（建议三个包）

| 制品 | 职责 |
|---|---|
| `lvgl4cj-core` | 仓颉安全 API、生命周期、句柄 |
| `lvgl4cj-sys` | C ABI、LVGL 源码版本锁定、native 构建 |
| `lvgl4cj-backend-sdl` | Linux 桌面显示与输入后端 |

拆分后 MCU/DRM/OH 后端可复用同一 `core`，SDL 仅作为其中一个实现。

---

## 十二、与 CJQT6 的经验对照

| 维度 | CJQT6（C++ Qt6） | lvgl4cj（C LVGL） | 结论 |
|---|---|---|---|
| ABI 复杂度 | 需处理 name mangling、异常、虚表、信号槽 | 纯 C，无此问题 | **lvgl4cj 更简单** |
| 回调机制 | 信号槽无 user_data，需复合键 `(ptr, signalId)` | `user_data` 原生支持 | **lvgl4cj 更简单** |
| 对象生命周期 | QObject 父子 + `destroyed` 信号 | 对象树 + `LV_EVENT_DELETE` | 机制对等，可复用思路 |
| 生命周期管理 | 禁用终结器，显式 `close()` | 同样建议显式管理 | 沿用 |
| 配置 | CMake 管理 | `lv_conf.h` 编译期宏 | **lvgl4cj 更麻烦**（需 hash 校验） |
| 缓冲 | Qt 内部管理 | 需外部提供绘制缓冲 | **lvgl4cj 更麻烦**（§3.4） |
| 线程 | Qt 有自己的线程模型 | 单线程主循环 | lvgl4cj 更简单但约束更强 |

**净结论**：lvgl4cj 在 ABI 与回调上比 CJQT6 省事，但在**配置管理**与**缓冲区所有权**上多两处硬骨头。总体工程量约为 CJQT6 的 40%~60%（主要省在 C++ 桥接）。

---

## 十三、第一个可交付物

**两周内的目标不是"框架"，是这一张图 + 五个断言：**

```
examples/hello_cj/
  → SDL2 窗口 800×480，深色背景，中间一个按钮
  → 点击按钮，文字从 "点我" 变成 "点了 N 次"
  → 窗口不崩，退出时干净释放
```

配套五个断言：

1. `lv_init` → `lv_timer_handler()` 循环稳定跑 10 分钟不崩
2. flush 回调被调用，画面正确显示（非花屏/非黑屏）
3. 点击命中按钮，事件回调触发，坐标正确
4. 删除父容器后，子对象句柄 `isAlive() == false`
5. 退出时 ASan 报告零泄漏

**这五条过了，项目就成立了；过不了，先修设计，不要往前堆控件。**
