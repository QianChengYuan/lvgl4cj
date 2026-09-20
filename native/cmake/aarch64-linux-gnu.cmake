# ============================================================================
# aarch64-linux-gnu.cmake —— 交叉编译工具链（§10.4 平台矩阵：ubuntu-arm64）
#
# 用法：
#   cmake -S native -B <build> \
#         -DCMAKE_TOOLCHAIN_FILE=native/cmake/aarch64-linux-gnu.cmake \
#         -DLVGLCJ_BUILD_SDL2=OFF -DLVGLCJ_BUILD_NULL_BACKEND=ON \
#         -DLVGLCJ_BUILD_TESTS=ON
#   cmake --build <build> -j
#
# ★ 这个文件的存在意义是**可移植性回归**，而不是「能在 arm64 上跑」：
#   交叉编译能抓到的问题包括（且仅包括）——
#     · 指针/长整型宽度假设（LP64 两侧相同，但 32 位平台不同）
#     · 结构体对齐与 sizeof（如 lvglcj_tree_dump_t 的指针后填充）
#     · 我们自己的代码里写死的 x86 假设（内联汇编、SIMD intrinsic、位序）
#     · 依赖了只有 x86 才有的头文件或类型
#   它**抓不到**：运行期行为差异、端序差异（两侧都是小端）、
#   SDL2/仓颉运行时在 arm64 上的行为。
#   因此「交叉编译通过」不能说成「arm64 已验证」——后者需要在 arm64 上跑 ctest，
#   见 docs/benchmarks/ 的平台矩阵记录。
#
# ★ 为什么默认关掉 SDL2：本机没有 aarch64 的 SDL2 开发包。
#   而 §10.4 的 CI 之所以依赖 headless 后端，正是为了避免「每个平台都要装窗口系统」——
#   headless（backend/null）是 CI 必需的，SDL2 只是桌面演示与 FPS 测量用。
# ============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_C_STANDARD 11)

# 只在目标根目录里找头文件与库，避免误用宿主机的 x86 库
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
