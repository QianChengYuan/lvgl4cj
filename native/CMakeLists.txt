# ============================================================================
# lvgl4cj —— L2 C 桥接层构建脚本
#
# 用法（在 WSL 内）：
#   cmake -S native -B "$HOME/lvgl4cj-build" -DCMAKE_BUILD_TYPE=Debug
#   cmake --build "$HOME/lvgl4cj-build" -j
# 或直接用封装脚本：bash scripts/build_native.sh
#
# 为什么以 native/ 作为 CMake 根目录（而不是工程根）：
#   把构建配置收敛在桥接层目录内，工程根保持「仓颉 + 脚本」的干净结构，
#   构建产物全部进 BUILD_DIR，不污染工作区。第三方用绝对路径引入。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------ 工程根目录（native/ 的上一级）
get_filename_component(LVGLCJ_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

project(lvgl4cj_bridge LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- 构建选项
option(LVGLCJ_BUILD_PROBES "构建 V1–V7 探针可执行文件（不进入默认构建）" OFF)
option(LVGLCJ_BUILD_TESTS "构建 C 侧单元测试（CTest）" OFF)
option(LVGLCJ_BUILD_SDL2 "构建 SDL2 桌面后端" ON)
option(LVGLCJ_BUILD_NULL_BACKEND "构建 headless 后端（CI 必需）" ON)
option(LVGLCJ_BUILD_SHARED "构建动态库而非静态库（默认静态，ADR-011）" OFF)
option(LVGLCJ_ASAN "启用 AddressSanitizer（仅 C 桥接层与 LVGL，仓颉侧不启用 §9.5）" OFF)

set(LVGLCJ_LVGL_DIR "${LVGLCJ_ROOT}/third_party/lvgl")
set(LVGLCJ_CONF_H "${LVGLCJ_ROOT}/native/include/lv_conf.h")
set(LVGLCJ_NATIVE_INCLUDE "${LVGLCJ_ROOT}/native/include")

if(NOT EXISTS "${LVGLCJ_LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR
        "未找到 LVGL 源码：${LVGLCJ_LVGL_DIR}\n"
        "请先执行： bash scripts/fetch_lvgl.sh")
endif()

if(NOT EXISTS "${LVGLCJ_CONF_H}")
    message(FATAL_ERROR
        "未找到 ${LVGLCJ_CONF_H}\n"
        "请先执行： bash scripts/gen_lv_conf.sh")
endif()

# ---------------------------------------------------------------- 输出目录
# 所有库落在 BUILD_DIR/lib，便于 build_native.sh 统一复制到工程根的 libs/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------- 编译选项
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    # 栈保护：§cangjie-ffi 技能建议 C 代码启用
    add_compile_options(-fstack-protector-strong)
    if(LVGLCJ_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

# ============================================================================
# 1. LVGL（L3）
# ============================================================================
# ★ LV_CONF_PATH 指向我们自己的 lv_conf.h：
#   既让 LVGL 用上固定配置，又**不必把 lv_conf.h 放进 third_party/**（保持第三方纯净）。
#   custom.cmake 会把 LV_CONF_DIR 加入 include 路径并 PUBLIC 传递 LV_CONF_PATH 定义。
set(LV_CONF_PATH "${LVGLCJ_CONF_H}" CACHE STRING "lvgl4cj 固定配置头" FORCE)

# 关掉用不到的重型目标：thorvg 是 C++ 矢量库，examples/demos 会显著拖慢构建
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

add_subdirectory("${LVGLCJ_LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)

# ============================================================================
# 2. C 桥接层（L2）→ liblvgl4cj_bridge
# ============================================================================
# 用 GLOB + CONFIGURE_DEPENDS：P0 期间源文件集合持续增长，
# 显式列举会让每次新增文件都要改 CMake；CONFIGURE_DEPENDS 保证增删文件后自动重配。
file(GLOB_RECURSE LVGLCJ_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

if(LVGLCJ_SRC STREQUAL "")
    message(FATAL_ERROR "native/src 下没有找到任何 .c 文件")
endif()

if(LVGLCJ_BUILD_SHARED)
    add_library(lvgl4cj_bridge SHARED ${LVGLCJ_SRC})
else()
    add_library(lvgl4cj_bridge STATIC ${LVGLCJ_SRC})
endif()

target_include_directories(lvgl4cj_bridge PUBLIC "${LVGLCJ_NATIVE_INCLUDE}")
target_link_libraries(lvgl4cj_bridge PUBLIC lvgl)

set_target_properties(lvgl4cj_bridge PROPERTIES OUTPUT_NAME lvgl4cj_bridge)

# §9.2 符号可见性：仅对动态库收敛符号，避免误导出内部实现；
# 静态库不需要（静态链接时可见性不影响符号解析），且能减少 P0 期的链接干扰。
if(LVGLCJ_BUILD_SHARED AND NOT MSVC)
    set_target_properties(lvgl4cj_bridge PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

# ============================================================================
# 3. 后端（L4，可插拔）
# ============================================================================
if(LVGLCJ_BUILD_NULL_BACKEND)
    set(LVGLCJ_NULL_SRC "${LVGLCJ_ROOT}/backend/null/null_backend.c")
    if(EXISTS "${LVGLCJ_NULL_SRC}")
        add_library(lvgl4cj_backend_null STATIC "${LVGLCJ_NULL_SRC}")
        target_include_directories(lvgl4cj_backend_null PUBLIC "${LVGLCJ_NATIVE_INCLUDE}")
        target_link_libraries(lvgl4cj_backend_null PUBLIC lvgl4cj_bridge)
    else()
        message(STATUS "lvgl4cj: headless 后端尚未实现，跳过（backend/null/null_backend.c）")
    endif()
endif()

if(LVGLCJ_BUILD_SDL2)
    # SDL2 通过 pkg-config 探测；未安装则降级跳过（不阻塞其余部分构建）
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()

    set(LVGLCJ_SDL2_SRC "${LVGLCJ_ROOT}/backend/sdl2/sdl2_backend.c")
    if(SDL2_FOUND AND EXISTS "${LVGLCJ_SDL2_SRC}")
        add_library(lvgl4cj_backend_sdl2 STATIC "${LVGLCJ_SDL2_SRC}")
        target_include_directories(lvgl4cj_backend_sdl2 PUBLIC
            "${LVGLCJ_NATIVE_INCLUDE}"
            ${SDL2_INCLUDE_DIRS})
        target_link_libraries(lvgl4cj_backend_sdl2 PUBLIC lvgl4cj_bridge ${SDL2_LIBRARIES})
        target_link_directories(lvgl4cj_backend_sdl2 PUBLIC ${SDL2_LIBRARY_DIRS})
        message(STATUS "lvgl4cj: SDL2 ${SDL2_VERSION} 已启用（桌面后端）")
    elseif(NOT SDL2_FOUND)
        message(WARNING
            "lvgl4cj: 未找到 SDL2，跳过桌面后端。\n"
            "  安装： sudo apt-get install -y libsdl2-dev\n"
            "  SDL2 后端是 P0 的首要验证目标（§8），缺失将无法跑通 hello_cj。")
    else()
        message(STATUS "lvgl4cj: SDL2 ${SDL2_VERSION} 可用，但桌面后端尚未实现（backend/sdl2/sdl2_backend.c）")
    endif()
endif()

# ============================================================================
# 4. 探针（V1–V7，§12 第 1 周）—— 默认不构建，避免拖慢日常编译
# ============================================================================
if(LVGLCJ_BUILD_PROBES)
    # probe_support.c 是「库」而非可执行文件（没有 main），
    # 供纯 C 探针与 probe/ 下的仓颉探针工程共用，因此单独编成 liblvgl4cj_probe.a
    add_library(lvgl4cj_probe STATIC "${CMAKE_CURRENT_LIST_DIR}/probe/probe_support.c")
    target_include_directories(lvgl4cj_probe PUBLIC "${LVGLCJ_NATIVE_INCLUDE}")
    target_link_libraries(lvgl4cj_probe PUBLIC lvgl4cj_bridge pthread m)

    file(GLOB LVGLCJ_PROBE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/probe/*.c")
    list(FILTER LVGLCJ_PROBE_SRC EXCLUDE REGEX "probe_support\\.c$")

    foreach(probe_src ${LVGLCJ_PROBE_SRC})
        get_filename_component(probe_name "${probe_src}" NAME_WE)
        add_executable(${probe_name} "${probe_src}")
        target_include_directories(${probe_name} PRIVATE "${LVGLCJ_NATIVE_INCLUDE}")
        target_link_libraries(${probe_name} PRIVATE lvgl4cj_probe pthread m)
        if(SDL2_FOUND)
            target_include_directories(${probe_name} PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(${probe_name} PRIVATE ${SDL2_LIBRARIES})
            target_link_directories(${probe_name} PRIVATE ${SDL2_LIBRARY_DIRS})
        endif()
    endforeach()
endif()

# ============================================================================
# 5. C 侧单元测试（CTest，§10.1）
# ============================================================================
if(LVGLCJ_BUILD_TESTS)
    enable_testing()
    file(GLOB LVGLCJ_TEST_SRC CONFIGURE_DEPENDS "${LVGLCJ_ROOT}/test/native/test_*.c")

    # ★ SDL2 缺失时**整个排除**依赖它的用例，而不是让它在运行时跳过。
    #   两种情况要分开处理：
    #     · test_sdl2_backend  —— 被测对象就是 SDL2 后端，没有它会永远「跳过」，
    #       那是一条不起作用的用例（噪声），不如不构建。
    #     · 其它用例（如 test_fullscreen_refresh）—— 主体逻辑与后端无关，
    #       只有其中一个场景（真实 sink 路径）需要 SDL2，用宏在**源码内**分段即可。
    #   这条规则直接来自交叉编译实践：aarch64 上没有 SDL2，若不做区分，
    #   整个交叉构建会因链接失败而中断 —— 而那会把「可移植性回归」一起挡掉。
    if(NOT TARGET lvgl4cj_backend_sdl2)
        list(FILTER LVGLCJ_TEST_SRC EXCLUDE REGEX "test_sdl2_backend\\.c$")
        message(STATUS "  测试：SDL2 后端未构建 → 排除 test_sdl2_backend")
    endif()

    foreach(test_src ${LVGLCJ_TEST_SRC})
        get_filename_component(test_name "${test_src}" NAME_WE)
        add_executable(${test_name} "${test_src}")
        target_include_directories(${test_name} PRIVATE "${LVGLCJ_NATIVE_INCLUDE}")
        target_link_libraries(${test_name} PRIVATE lvgl4cj_bridge pthread m)
        # 后端库按需链接：测试需要 headless（G1 硬前置），部分断言需要 SDL2。
        # 用 TARGET 判定而不是变量，这样后端被跳过时不会产生链接错误。
        if(TARGET lvgl4cj_backend_null)
            target_link_libraries(${test_name} PRIVATE lvgl4cj_backend_null)
        endif()
        if(TARGET lvgl4cj_backend_sdl2)
            target_link_libraries(${test_name} PRIVATE lvgl4cj_backend_sdl2)
            # 让源码能感知后端是否存在，从而只编译它真正支持的场景
            target_compile_definitions(${test_name} PRIVATE LVGLCJ_TEST_HAVE_SDL2=1)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ============================================================================
# 配置摘要
# ============================================================================
message(STATUS "================ lvgl4cj native 配置 ================")
message(STATUS "  工程根        : ${LVGLCJ_ROOT}")
message(STATUS "  LVGL          : ${LVGLCJ_LVGL_DIR}")
message(STATUS "  lv_conf.h     : ${LVGLCJ_CONF_H}")
message(STATUS "  构建类型      : ${CMAKE_BUILD_TYPE}")
if(LVGLCJ_BUILD_SHARED)
    message(STATUS "  库形态        : shared")
else()
    message(STATUS "  库形态        : static（ADR-011）")
endif()
message(STATUS "  探针          : ${LVGLCJ_BUILD_PROBES}")
message(STATUS "  C 单测        : ${LVGLCJ_BUILD_TESTS}")
message(STATUS "  ASan          : ${LVGLCJ_ASAN}")
message(STATUS "  输出目录      : ${CMAKE_BINARY_DIR}/lib")
message(STATUS "=====================================================")
