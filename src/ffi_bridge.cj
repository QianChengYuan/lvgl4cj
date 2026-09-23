/*
 * ffi_bridge.cj —— C ABI 的 foreign 声明（L1 ↔ L2 契约的仓颉侧）
 *
 * ===================== 为什么这个文件在根包而不是 ffi/ 子包 =====================
 * 仓颉有一条硬语言约束（本项目实测确认）：
 *   **`foreign` 声明不接受任何访问修饰符** ——
 *     `public foreign func`   → error: 'foreign' and 'public' modifiers conflict
 *     `protected foreign func` → error: 'foreign' and 'protected' modifiers conflict
 *   因此 foreign 声明只能是默认的 internal，作用域为「当前包及其子包」。
 *
 * 推论：访问级别「包及子包」决定了 foreign 声明**必须放在所有使用者的公共祖先包**里。
 *   · 若放在 lvgl4cj.ffi 子包 → 兄弟包 lvgl4cj.core / lvgl4cj.widgets 看不见（internal 对兄弟包不可见）
 *   · 放在根包 lvgl4cj → 根包本身 + 全部子包都能看见 ✓
 * 这正是本文件位于 src/ 根、且声明为 package lvgl4cj 的原因。
 * （这一点与设计文档 §四 目录结构中的 src/ffi/bridge.cj 不同，属实现细节修正；
 *   设计文档 §四 属于「不冻结」的实现细节范围。）
 *
 * 单向依赖保证无循环：子包 import 根包，根包不 import 任何子包。
 * ==============================================================================
 *
 * 其他要点：
 *   · 本文件是 native/include/lvglcj_bridge.h 的**逐条镜像**；
 *     两侧一致性由 test/ffi_contract_test.cj 断言
 *   · 所有 foreign 调用必须包在 unsafe {} 中（cangjie-ffi 技能）
 *   · 命名规则 lvglcj_<子系统>_<动作>，与 C 侧一字不差
 *   · 头文件路径**不需要**在 cjpm.toml 里声明：foreign 声明只需要函数签名
 *     （这点与设计文档 §9.1 的写法不同，见该节勘误）
 *
 * 注意 已归档：分支 A（闭包指针 + pin 表）相关的 pin/unpin 接口**不在此声明**。
 *    原因：仓颉的 CFunc Lambda 不能捕获变量，分支 A 在语言层不成立。
 */
package lvgl4cj

/* ============================================================ §5.1 运行时 */
foreign func lvglcj_init(): Int32

foreign func lvglcj_deinit(): Int32

foreign func lvglcj_set_tick_cb(): Int32

foreign func lvglcj_tick_get(): UInt32

foreign func lvglcj_tick_elaps(prev: UInt32): UInt32

foreign func lvglcj_version(): CString

foreign func lvglcj_conf_hash(): UInt32

foreign func lvglcj_conf_color_depth(): Int32

foreign func lvglcj_conf_use_log(): Int32

foreign func lvglcj_conf_mem_size(): Int32

foreign func lvglcj_conf_stride_align(): Int32

foreign func lvglcj_conf_use_fs_posix(): Int32

foreign func lvglcj_conf_def_refr_period(): Int32

foreign func lvglcj_conf_version_major(): Int32

foreign func lvglcj_conf_version_minor(): Int32

foreign func lvglcj_conf_version_patch(): Int32

foreign func lvglcj_start_thread(): Int32

foreign func lvglcj_stop_thread(shutdownMode: Int32): Int32

foreign func lvglcj_pump(): Int32

foreign func lvglcj_current_os_tid(): Int64

foreign func lvglcj_is_lvgl_thread(): Int32

foreign func lvglcj_rebind_current_thread(): Int32

foreign func lvglcj_post_task(taskId: Int64): Int32

foreign func lvglcj_post_and_wait(taskId: Int64): Int32
/* 注意：lvglcj_drain_deferred() 在 t3 实现（延迟删除队列）后才会在此声明 ——
   每个 foreign 声明都会在链接期产生符号引用，声明必须先于实现存在。 */
foreign func lvglcj_queue_set_capacity(capacity: Int32): Int32

foreign func lvglcj_queue_set_full_policy(policy: Int32): Int32

foreign func lvglcj_queue_size(): Int32

/*
 * 日志与错误回调：**函数指针注册**，不走闭包表。
 * 原因见 lvglcj_bridge.h —— 闭包表签名 (Int64) -> Unit 装不下日志字符串，
 * 强行把 LVGL 的 buf 指针塞进 int64 会在回调返回后变成悬空指针。
 */
foreign func lvglcj_log_set_cb(cb: CFunc<(Int32, CString) -> Unit>): Int32

foreign func lvglcj_log_init(): Int32

foreign func lvglcj_error_set_cb(cb: CFunc<(CPointer<Unit>) -> Unit>): Int32

/* 分发表：闭包桥接的核心（§3.2.3，分支 B） */
foreign func lvglcj_set_dispatch(fn: CFunc<(Int32, Int64) -> Int32>): Int32

/* ============================================================ §5.2 句柄 */
foreign func lvglcj_handle_register(ptr: CPointer<Unit>, name: CString): Int64

foreign func lvglcj_ptr_of(h: Int64): CPointer<Unit>

foreign func lvglcj_handle_of(ptr: CPointer<Unit>): Int64

foreign func lvglcj_handle_alive(h: Int64): Int32

foreign func lvglcj_handle_state(h: Int64): Int32

foreign func lvglcj_handle_invalidate(h: Int64): Unit

foreign func lvglcj_handle_release(h: Int64): Unit

foreign func lvglcj_handle_pending_delete(h: Int64): Int32

foreign func lvglcj_handle_count(state: Int32): Int32

/* ============================================ 分发器与延迟删除（t3 落地） */
/* 供 L1 与测试驱动 C 侧分发路径；正式回调由 C 侧 trampoline 触发，不经过这里 */
foreign func lvglcj_call_closure(cid: Int32, arg: Int64): Int32
/* 由 LvglRuntime.start() 或首次注册闭包时安装（closure.cj 中幂等处理） */
foreign func lvglcj_drain_deferred(): Unit

/* ---------------------------------------------------------------- 错误计数 */
foreign func lvglcj_error_count(code: Int32): Int32

foreign func lvglcj_error_reset_counts(): Unit

/* ==================================================== §5.8 定时器（t3 用） */
foreign func lvglcj_timer_create(cid: Int32, periodMs: Int32): Int64

foreign func lvglcj_timer_delete(timer: Int64): Int32

foreign func lvglcj_timer_set_period(timer: Int64, ms: Int32): Int32

foreign func lvglcj_timer_pause(timer: Int64): Int32

foreign func lvglcj_timer_resume(timer: Int64): Int32

foreign func lvglcj_timer_handler(): Int32

foreign func lvglcj_timer_next_ms(): Int32

/* ============================================ §5.3 Display（t4 落地） */
/*
 * ★ 缓冲模式取值与 C 侧 lvglcj_buf_mode_t 一致：0=PARTIAL 1=DIRECT 2=FULL。
 * ★ 绘制缓冲由 C 侧分配并持有（ADR-002）—— 仓颉侧**绝不能**把
 *   Array<UInt8> 的底层指针交给 LVGL，GC 移动即花屏。
 */
foreign func lvglcj_display_create(w: Int32, h: Int32, colorFormat: Int32, bufMode: Int32, bufLines: Int32): Int64

foreign func lvglcj_display_set_flush_cb(disp: Int64, cid: Int32): Int32

foreign func lvglcj_display_set_flush_wait_cb(disp: Int64, cid: Int32): Int32

foreign func lvglcj_display_flush_ready(disp: Int64): Int32

foreign func lvglcj_display_flush_is_last(disp: Int64): Int32

foreign func lvglcj_display_get_color_format(disp: Int64): Int32

foreign func lvglcj_display_get_buf_bytes(disp: Int64): Int64

foreign func lvglcj_display_get_buf_stride(disp: Int64): Int32
/*
 * 测试支持：像素级体检。返回打包值（高位=不同像素数，低位=非零像素数），负数=出错。
 * 打包而非出参，是为了避免在仓颉侧做指针写入（见 lvglcj_bridge.h 的说明）。
 */
foreign func lvglcj_display_sample_buf(disp: Int64): Int64

foreign func lvglcj_display_delete(disp: Int64): Int32

/* ============================================ §5.4 InDev + Group（t4 落地） */
/*
 * 取值与 C 侧 lvglcj_indev_type_t 逐值对齐 LVGL：
 *   0=NONE 1=POINTER 2=KEYPAD 3=BUTTON 4=ENCODER
 * （早期 C 头文件曾把 BUTTON/ENCODER 写反，已修正；此处保持一致）
 */
foreign func lvglcj_indev_create(indevType: Int32): Int64

foreign func lvglcj_indev_set_read_cb(indev: Int64, cid: Int32): Int32

foreign func lvglcj_indev_set_display(indev: Int64, disp: Int64): Int32

foreign func lvglcj_indev_set_group(indev: Int64, group: Int64): Int32

foreign func lvglcj_indev_delete(indev: Int64): Int32
/*
 * 输入数据回填（T5 配套）：**只能在 indev_read 回调执行期间调用**，
 * 否则返回 NOT_SUPPORTED。理由见 lvglcj_bridge.h —— 不把 lv_indev_data_t
 * 的布局知识暴露给仓颉侧，避免字段顺序变化造成静默的输入错乱。
 */
foreign func lvglcj_indev_set_point(x: Int32, y: Int32, pressed: Int32): Int32

foreign func lvglcj_indev_set_key(key: UInt32, pressed: Int32): Int32

foreign func lvglcj_indev_set_enc_diff(diff: Int32): Int32

foreign func lvglcj_indev_set_continue(cont: Int32): Int32

foreign func lvglcj_group_create(): Int64

foreign func lvglcj_group_delete(g: Int64): Int32

foreign func lvglcj_group_add_obj(g: Int64, obj: Int64): Int32

foreign func lvglcj_group_remove_obj(obj: Int64): Int32

foreign func lvglcj_group_focus_obj(obj: Int64): Int32

foreign func lvglcj_group_get_focused(g: Int64): Int64

foreign func lvglcj_group_focus_next(g: Int64): Int32

foreign func lvglcj_group_focus_prev(g: Int64): Int32

foreign func lvglcj_group_set_default(g: Int64): Int32

/* ============================================ headless 后端（backend/null） */
/*
 * 探针结论 G1：无 display 时 lv_screen_active() 返回 0，而 lv_obj_create(NULL)
 * 会触发 LVGL 断言**直接终止进程**。因此任何建对象的测试/示例都必须先有 display，
 * headless 后端正是为此存在（不是可选优化）。
 */
foreign func lvglcj_null_init(w: Int32, h: Int32, colorFormat: Int32, bufLines: Int32): Int32

foreign func lvglcj_null_deinit(): Int32

foreign func lvglcj_null_display(): Int64

foreign func lvglcj_null_pump(frames: Int32): Int32

/* ============================================ SDL2 桌面后端（backend/sdl2） */
/*
 * ★ 本后端的 flush 走 **C 侧 sink**（不是闭包表）：像素指针只在 C 侧流转，
 *   仓颉侧永远拿不到绘制缓冲（ADR-002）。因此这里没有 flush 回调相关的声明。
 *
 * ★ lvglcj_sdl2_init 必须由**将要跑事件循环的线程**调用 ——
 *   该线程会被登记为渲染线程，用于两点：
 *     (1) 单线程时 flush 走内联渲染（否则会等死自己）
 *     (2) §3.8.4 等待图判据 3（渲染线程同步等待会与 flush 成环）
 */
foreign func lvglcj_sdl2_init(w: Int32, h: Int32, colorFormat: Int32, bufLines: Int32): Int32

foreign func lvglcj_sdl2_deinit(): Int32

foreign func lvglcj_sdl2_display(): Int64
/* 返回 1 = 收到退出请求，0 = 继续，负数 = 错误 */
foreign func lvglcj_sdl2_poll_events(): Int32
/* 把 SDL 采集的输入写入当前 indev 读取上下文（只能在 read 回调内调用） */
foreign func lvglcj_sdl2_feed_indev(): Int32

foreign func lvglcj_sdl2_flush_count(): Int32

foreign func lvglcj_sdl2_present_count(): Int32

foreign func lvglcj_sdl2_wait_timeout_count(): Int32

foreign func lvglcj_sdl2_is_paused(): Int32

foreign func lvglcj_sdl2_is_render_thread(): Int32

foreign func lvglcj_sdl2_inject_quit(): Int32

/* ==================================================== §5.5 对象（t3 用） */
foreign func lvglcj_obj_create(parent: Int64): Int64

foreign func lvglcj_obj_delete(obj: Int64): Int32

foreign func lvglcj_obj_clean(obj: Int64): Int32

foreign func lvglcj_obj_get_child_count(obj: Int64): Int32

foreign func lvglcj_obj_add_event(obj: Int64, code: Int32, cid: Int32): Int64

foreign func lvglcj_obj_remove_event(obj: Int64, evDsc: Int64): Int32

foreign func lvglcj_obj_remove_event_by_cid(obj: Int64, cid: Int32): Int32

foreign func lvglcj_screen_active(): Int64

/* ---------------------------------------------------- §5.6 事件查询（t3 用） */
foreign func lvglcj_event_get_code(evh: Int64): Int32

foreign func lvglcj_event_get_target(evh: Int64): Int64

foreign func lvglcj_event_get_current_target(evh: Int64): Int64

foreign func lvglcj_event_get_closure_id(evh: Int64): Int32

foreign func lvglcj_event_stop_bubbling(evh: Int64): Int32

foreign func lvglcj_event_stop_processing(evh: Int64): Int32
/* param 双路径（见 lvglcj_bridge.h §5.6 的说明：user_data 已被 cid 占用） */
foreign func lvglcj_event_get_param_handle(evh: Int64): Int64

foreign func lvglcj_event_get_param_scalar(evh: Int64): Int64

foreign func lvglcj_obj_send_event(obj: Int64, code: Int32, param: Int64): Int32

foreign func lvglcj_obj_send_event_scalar(obj: Int64, code: Int32, paramScalar: Int64): Int32

/* ==================================================== t6：几何 / 层级 / 标志 */
foreign func lvglcj_obj_set_pos(obj: Int64, x: Int32, y: Int32): Int32

foreign func lvglcj_obj_set_size(obj: Int64, w: Int32, h: Int32): Int32

foreign func lvglcj_obj_align(obj: Int64, align: Int32, xOfs: Int32, yOfs: Int32): Int32

foreign func lvglcj_obj_get_x(obj: Int64): Int32

foreign func lvglcj_obj_get_y(obj: Int64): Int32

foreign func lvglcj_obj_get_width(obj: Int64): Int32

foreign func lvglcj_obj_get_height(obj: Int64): Int32

foreign func lvglcj_obj_add_flag(obj: Int64, flag: Int32): Int32

foreign func lvglcj_obj_remove_flag(obj: Int64, flag: Int32): Int32

foreign func lvglcj_obj_get_child(obj: Int64, idx: Int32): Int64

foreign func lvglcj_obj_set_flex_flow(obj: Int64, flow: Int32): Int32

foreign func lvglcj_obj_set_flex_align(obj: Int64, mainPlace: Int32, crossPlace: Int32, trackPlace: Int32): Int32

/* ==================================================== t6：控件（P0 仅三个） */
foreign func lvglcj_label_create(parent: Int64): Int64

foreign func lvglcj_label_set_text(label: Int64, text: CString): Int32

foreign func lvglcj_label_set_long_mode(label: Int64, mode: Int32): Int32

foreign func lvglcj_button_create(parent: Int64): Int64

/* ---------------- P1 批次 1 控件（§7.1）：switch / checkbox / bar ----------------
 *
 * ★ *_is_checked 的返回值是**三态**而非错误码：
 *     1 = 已勾选，0 = 未勾选（**合法答案**），负数 = 错误。
 *   所以调用方不能写 `rc != 0` 判成功 —— 见桥接头同名说明。
 *
 * ★ bar_get_value 用**出参**：条形的值域本身可为负，
 *   "负数即错误"在那里不成立，只能把值与错误码分开放。
 *   同一个"取状态"问题在不同函数上得出相反结论，判据是**值域是否与错误码重叠**。
 *
 * ★ min/max 用 min/max 作参数名会与 Cangjie 标准库函数名混淆，
 *   这里保持与 C 侧一致的 lo/hi 语义但沿用 min/max 命名（仅参数名，作用域局部）。
 */
foreign func lvglcj_switch_create(parent: Int64): Int64

foreign func lvglcj_switch_set_checked(sw: Int64, checked: Int32): Int32

foreign func lvglcj_switch_is_checked(sw: Int64): Int32

foreign func lvglcj_checkbox_create(parent: Int64): Int64

foreign func lvglcj_checkbox_set_text(cb: Int64, text: CString): Int32

foreign func lvglcj_checkbox_set_checked(cb: Int64, checked: Int32): Int32

foreign func lvglcj_checkbox_is_checked(cb: Int64): Int32

foreign func lvglcj_bar_create(parent: Int64): Int64

foreign func lvglcj_bar_set_range(bar: Int64, min: Int32, max: Int32): Int32

foreign func lvglcj_bar_set_value(bar: Int64, value: Int32): Int32

foreign func lvglcj_bar_get_value(bar: Int64, out_value: CPointer<Int32>): Int32

/* ---------------- P1 批次 2 控件（§7.1）：slider / arc / led / spinner ----------------
 *
 * slider 与 arc 的「值 + 范围」形状与 bar 完全相同，get_value 同样用**出参**
 * （值域可为负，负数与错误码会撞车）——三者在 L1 与 C 侧共用同一段实现。
 *
 * led 的亮度边界必须由我们拦：LVGL 收 uint8_t，越界会静默截断
 * （设 300 与设 44 的结果一样），这种"看起来生效了"的失败最难发现。
 *
 * spinner **没有属性**：旋转由 LVGL 内部动画驱动，不经过本层。
 */
foreign func lvglcj_slider_create(parent: Int64): Int64

foreign func lvglcj_slider_set_range(slider: Int64, min: Int32, max: Int32): Int32

foreign func lvglcj_slider_set_value(slider: Int64, value: Int32): Int32

foreign func lvglcj_slider_get_value(slider: Int64, out_value: CPointer<Int32>): Int32

foreign func lvglcj_arc_create(parent: Int64): Int64

foreign func lvglcj_arc_set_range(arc: Int64, min: Int32, max: Int32): Int32

foreign func lvglcj_arc_set_value(arc: Int64, value: Int32): Int32

foreign func lvglcj_arc_get_value(arc: Int64, out_value: CPointer<Int32>): Int32

foreign func lvglcj_led_create(parent: Int64): Int64

foreign func lvglcj_led_set_color(led: Int64, color: UInt32): Int32

foreign func lvglcj_led_set_brightness(led: Int64, brightness: Int32): Int32

foreign func lvglcj_led_set_on(led: Int64, on: Int32): Int32

foreign func lvglcj_spinner_create(parent: Int64): Int64

/* ---------------- P1 批次 3：dropdown（§7.1）----------------
 *
 * ★ 只提供**会拷贝**选项字符串的形态。LVGL 另有一个 set_options_static（不拷贝），
 *   它要求那块内存一直有效，而仓颉侧传来的 CString 在调用返回后即被释放 ——
 *   用它就是立刻制造悬空指针。契约里不存在那个入口，这里也就不可能选错。
 *
 * ★ get_selected / get_option_count 直接返回数值、**负数是错误**：
 *   索引与个数都是非负的，不与错误码的负值域重叠。
 *   这与 bar/slider/arc 必须用出参的决定看似矛盾，规则其实是同一条：
 *   **值域是否与错误码重叠**。
 */
foreign func lvglcj_dropdown_create(parent: Int64): Int64

foreign func lvglcj_dropdown_set_options(dd: Int64, options: CString): Int32

foreign func lvglcj_dropdown_set_selected(dd: Int64, idx: Int32): Int32

foreign func lvglcj_dropdown_get_selected(dd: Int64): Int32

foreign func lvglcj_dropdown_get_option_count(dd: Int64): Int32

/* ---------------- P1 批次 4：line（§7.1）----------------
 *
 * ★★ line 是**第二类所有权**：LVGL **不拷贝点数组**，只保存我们给的地址
 *   （文档原文："Only the address is saved, so the array needs to be alive
 *   while the line exists"）。因此 C 侧会把坐标复制进一块自己的缓冲、
 *   随对象释放 —— 调用方传入的数组可以随即销毁。
 *   这与 Canvas 的像素缓冲是同一套模式（LVGL 持指针 + 我们负责生命周期）。
 *
 * ★ xy 是**扁平坐标对**（x0,y0,x1,y1,...），point_count 是**点的个数**而非元素个数。
 */
foreign func lvglcj_line_create(parent: Int64): Int64

foreign func lvglcj_line_set_points(line: Int64, xy: CPointer<Int32>, point_count: Int32): Int32

foreign func lvglcj_line_set_y_invert(line: Int64, on: Int32): Int32

/* ---------------- P1 批次 5：image（§7.1）----------------
 *
 * ★ 路径字符串由 LVGL **拷贝**（实现里是 lv_strdup，旧源由它自己 lv_free），
 *   所以这里收 CString 是安全的 —— 与 dropdown 的选项同性质，
 *   而不是 canvas/line 那种"LVGL 只存指针、由我们持有"的类型。
 *
 * ★ 契约里不提供图像描述符（lv_image_dsc_t*）形态：LVGL 对 VARIABLE 分支
 *   只执行 img->src = src（不拷贝），那是第三类所有权问题，
 *   而且还要把描述符的内存布局一并冻结进契约。本批只做文件路径。
 *
 * ★ set_scale 的单位**不是百分比**：256 = 100%，128 = 一半，512 = 两倍。
 */
foreign func lvglcj_image_create(parent: Int64): Int64

foreign func lvglcj_image_set_src(img: Int64, path: CString): Int32

foreign func lvglcj_image_set_offset(img: Int64, x: Int32, y: Int32): Int32

foreign func lvglcj_image_set_scale(img: Int64, zoom: Int32): Int32

/* ---------------- P1 批次 6：roller ---------------- */
foreign func lvglcj_roller_create(parent: Int64): Int64

foreign func lvglcj_roller_set_options(roller: Int64, options: CString, mode: Int32): Int32

foreign func lvglcj_roller_set_selected(roller: Int64, sel: Int32): Int32

foreign func lvglcj_roller_get_selected(roller: Int64): Int32

foreign func lvglcj_roller_set_visible_row_count(roller: Int64, rows: Int32): Int32

/* 模式取值，与 native/include/lvglcj_bridge.h 的 LVGLCJ_ROLLER_MODE_* 对应 */
public let LVGLCJ_ROLLER_MODE_NORMAL: Int32 = 0
public let LVGLCJ_ROLLER_MODE_INFINITE: Int32 = 1

/* ---------------- Canvas（§3.11.2）：自定义绘制的唯一出口 ----------------
 *
 * ★ 颜色参数的**解释方式在不同函数上不同**，调用方必须看清：
 *     · 绘制类与 fill_bg ：0xRRGGBB（高 8 位不解释），透明度走单独的 opa 参数
 *     · set_palette      ：0xAARRGGBB（调色板项本身是带 alpha 的 32 位色）
 *   这是 ABI 层面的不一致，不是笔误；统一会悄悄改变已声明签名的语义。
 *   详见 native/src/canvas.c 文件头。
 */
foreign func lvglcj_canvas_create(parent: Int64): Int64

foreign func lvglcj_canvas_set_buffer(canvas: Int64, w: Int32, h: Int32): Int32

foreign func lvglcj_canvas_set_palette(canvas: Int64, idx: Int32, color: UInt32): Int32

foreign func lvglcj_canvas_fill_bg(canvas: Int64, color: UInt32, opa: Int32): Int32

foreign func lvglcj_canvas_draw_point(canvas: Int64, x: Int32, y: Int32, color: UInt32): Int32

// 以下三条的参数个数由**已冻结的 C 契约**决定（桥接头就是这么声明的），
// foreign 声明必须与之逐参数对应 —— 少一个参数就不再是同一个函数。
// 因此 G.FUN.01 只能靠**同行尾注释**抑制（独立成行的写法实测无效），
// 下面的注释不是装饰，去掉它门禁就会红。
foreign func lvglcj_canvas_draw_line(canvas: Int64, x1: Int32, y1: Int32, x2: Int32, y2: Int32, color: UInt32): Int32 // cjlint-ignore !G.FUN.01

foreign func lvglcj_canvas_draw_rect(canvas: Int64, x: Int32, y: Int32, w: Int32, h: Int32, color: UInt32): Int32 // cjlint-ignore !G.FUN.01

// ★ 这里的形参名用缩写（cv/a0/a1/c），只为让整条声明连同抑制注释落在**同一行**：
//   G.FUN.01 的抑制必须与告警所在行同行（独立成行实测无效），
//   而 cjfmt 会按行宽把过长的声明重排成两行 —— 一旦重排，尾注释就落到第二行，
//   抑制随即失效。形参名不影响 ABI（C 只认符号名与类型），所以缩短是安全的。
foreign func lvglcj_canvas_draw_arc(cv: Int64, cx: Int32, cy: Int32, r: Int32, a0: Int32, a1: Int32, c: UInt32): Int32 // cjlint-ignore !G.FUN.01

/* ==================================================== 动画（§5.9，t7） */
/*
 * ★ var = anim_ctx（ADR-015）：因此**没有**「把动画绑到对象」的 var 参数，
 *   目标对象只通过 set_target 记录，用于「对象删除时停掉动画」的补偿路径。
 * ★ 释放时机不可协商：ctx 由 C 侧 internal deleted_cb 唯一释放，
 *   仓颉侧只需在动画「自然结束 / 被删除 / 对象被删」之后看到句柄失效即可。
 */
foreign func lvglcj_anim_create(): Int64

foreign func lvglcj_anim_set_target(a: Int64, obj: Int64): Int32

foreign func lvglcj_anim_set_values(a: Int64, fromValue: Int32, toValue: Int32): Int32

foreign func lvglcj_anim_set_time(a: Int64, ms: Int32): Int32

foreign func lvglcj_anim_set_delay(a: Int64, ms: Int32): Int32

foreign func lvglcj_anim_set_exec_cb(a: Int64, cid: Int32): Int32

foreign func lvglcj_anim_set_path(a: Int64, path: Int32): Int32

foreign func lvglcj_anim_set_repeat(a: Int64, cnt: Int32): Int32

foreign func lvglcj_anim_set_playback(a: Int64, ms: Int32): Int32

foreign func lvglcj_anim_set_deleted_cb(a: Int64, cid: Int32): Int32

foreign func lvglcj_anim_set_start_cb(a: Int64, cid: Int32): Int32

foreign func lvglcj_anim_start(a: Int64): Int32

foreign func lvglcj_anim_delete(a: Int64): Int32

foreign func lvglcj_obj_delete_anim(obj: Int64): Int32

foreign func lvglcj_anim_count_running(): Int32

foreign func lvglcj_anim_ctx_count(): Int32

/* ==================================================== 字体（§5.10） */
/*
 * 内置字体：0 = 默认（Montserrat，**无中文**），1 = SIMSUN 16 CJK。
 * ★ 后者**不是**常用汉字表：官方描述为 "1000 most common CJK radicals"，
 *   实测是简体/繁体/日文异体混合清单且有缺字（问/调/窗/显 缺失）。
 *   详见 src/font.cj 文件头的「坑 2」与 native/probe/probe_font_coverage.c。
 */
foreign func lvglcj_font_builtin(which: Int32): Int64

/*
 * 字形覆盖查询。★ 返回值是**三态**而非错误码：
 *   1 = 有字形，0 = 没有字形（合法答案），负数 = 错误码。
 * 因此调用方不能写 `rc != 0` 判断成功 —— 见桥接头的详细说明。
 */
foreign func lvglcj_font_has_glyph(font: Int64, codepoint: Int32): Int32

foreign func lvglcj_font_load(path: CString): Int64

foreign func lvglcj_font_delete(font: Int64): Int32

/* ==================================================== t6：样式（§5.7 全量声明） */
/*
 * 这里把 53 个 setter **全部声明**，即使 L1 的 DSL 只包装其中一部分。
 * 两个好处：
 *   1. 声明即链接引用 —— 任何 C 侧命名写错都会在链接期暴露，而不是等到有人用到
 *   2. 后续补 DSL 方法时不需要再动本文件
 * 注意 v9 的 setter **不带 selector**（selector 只在 objAddStyle 给出）。
 */
foreign func lvglcj_style_create(): Int64

foreign func lvglcj_style_delete(style: Int64): Int32

foreign func lvglcj_style_set_bg_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_bg_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_bg_grad_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_bg_grad_dir(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_border_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_border_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_border_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_radius(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_all(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_top(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_bottom(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_left(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_right(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_row(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_pad_column(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_height(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_min_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_max_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_min_height(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_max_height(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_text_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_text_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_text_font(s: Int64, font: Int64): Int32

foreign func lvglcj_style_set_text_align(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_text_letter_space(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_text_line_space(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_shadow_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_shadow_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_shadow_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_shadow_offset_x(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_shadow_offset_y(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_outline_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_outline_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_outline_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_outline_pad(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_translate_x(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_translate_y(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_line_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_line_dash_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_line_dash_gap(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_arc_width(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_arc_color(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_arc_rounded(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_image_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_image_recolor(s: Int64, color: UInt32): Int32

foreign func lvglcj_style_set_image_recolor_opa(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_blend_mode(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_layout(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_base_dir(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_clip_corner(s: Int64, v: Int32): Int32

foreign func lvglcj_style_set_rotary_sensitivity(s: Int64, v: Int32): Int32

foreign func lvglcj_obj_add_style(obj: Int64, style: Int64, selector: Int32): Int32

foreign func lvglcj_obj_remove_style(obj: Int64, style: Int64, selector: Int32): Int32

foreign func lvglcj_obj_remove_style_all(obj: Int64): Int32
/* 注意：参数名不能叫 prop —— 那是仓颉的关键字（属性声明），改用 propId */
foreign func lvglcj_obj_get_style_prop(obj: Int64, propId: Int32, selector: Int32): Int64

foreign func lvglcj_obj_set_style_pad_all(obj: Int64, v: Int32, selector: Int32): Int32

foreign func lvglcj_obj_set_style_pad_gap(obj: Int64, v: Int32, selector: Int32): Int32

/* ==================================================== 调试 / 可观测性（§7.3） */
/*
 * ★ 这些出参在 C 侧是**裸指针**（`uint32_t*` / 结构体指针）。
 *   仓颉侧的传参方式见 src/debug.cj 的说明：用长度 1 的 Array 承载，
 *   经 acquireArrayRawData 取其数据地址 —— 这样不必自己做 LibC.malloc，
 *   也不会误把 GC 堆上的对象地址交出去。
 * ★ lvglcj_debug_dump_tree 会**分配**数组，必须配对调用
 *   lvglcj_debug_free_tree_dump，否则每次 dump 都泄漏（C 侧无 GC）。
 */
foreign func lvglcj_obj_count(): Int32

foreign func lvglcj_mem_monitor(used: CPointer<UInt32>, frag: CPointer<UInt32>, maxUsed: CPointer<UInt32>,
    freeSize: CPointer<UInt32>): Int32

foreign func lvglcj_perf_sample(out: CPointer<CPerf>): Int32

foreign func lvglcj_debug_dump_tree(root: Int64, maxDepth: Int32, out: CPointer<CTreeDump>): Int32

foreign func lvglcj_debug_free_tree_dump(dump: CPointer<CTreeDump>): Unit

/*
 * root 为 UTF-8 路径字符串。
 * ★ C 侧签名是 `const char*` 且接受 NULL（表示「用编译期根路径」），
 *   但仓颉的 CString 无法表示 NULL，因此仓颉侧**总是传字符串** ——
 *   「不指定」用编译期默认值 "." 表达（LV_FS_POSIX_PATH 就是 "."，两者等价）。
 *   这样既用上了类型安全的 CString，也不必为了传 NULL 去构造裸指针。
 */
foreign func lvglcj_fs_init_posix(root: CString): Int32
