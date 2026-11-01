# CMake 最低版本要求
cmake_minimum_required(VERSION 3.5)

# 工程名
project(MyPlacement)

# 构建类型默认 Release
# 本项目是计算密集型（DCT 泊松求解、稀疏方程迭代、21 万单元×数百轮梯度下降），
# Debug 版本慢 2~3 倍，是 Web 端"分析超时"的主要成因之一。
# 未显式指定时统一走 Release（-O3 -DNDEBUG）；多配置生成器（VS）由 IDE 下拉框控制。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Release 推荐)" FORCE)
    message(STATUS "未指定 CMAKE_BUILD_TYPE，已默认使用 Release")
endif()
message(STATUS "当前构建类型: ${CMAKE_BUILD_TYPE}")

# 导出 compile_commands.json，方便 VS Code 的 IntelliSense 读取编译参数
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 让 Visual Studio 调试启动时把工作目录固定为项目根目录
# （程序用相对路径读 data/ 并写 output/，若默认在构建目录启动会找不到数据）
# 必须写成 CACHE 变量，普通 set 不会进入 CMakeCache.txt，VS 读不到
set(VS_DEBUGGER_WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} CACHE STRING "VS debugger working directory = project root")

# 生成可执行文件 myplace
add_executable(
    myplace
    myplace_main.cpp
    MyPlace/myplace.cpp
    MyPlace/initial_placement.cpp
    MyPlace/global_placement.cpp
    MyPlace/legalization.cpp # 任务7：布局合法化（Abacus）
    Plot/plot.cpp           # 任务6：CImg 画图
    Plot/gdsii.cpp          # 任务6：GDSII 输出
)

# 头文件搜索路径
include_directories(
    ${PROJECT_SOURCE_DIR}/Common
    ${PROJECT_SOURCE_DIR}/PlaceData
    ${PROJECT_SOURCE_DIR}/MyPlace
    ${PROJECT_SOURCE_DIR}/third_party/eigen3  # Eigen 库（任务4/5 求解器用）
    ${PROJECT_SOURCE_DIR}/Library/CImg        # CImg 图像库（任务6）
    ${PROJECT_SOURCE_DIR}/Plot                # 绘图模块（任务6）
)

# 链接库
# CImg 仅在 Linux/WSL 下可用（需要 X11），Windows/MSVC 下自动降级为不绘图
if(WIN32)
    # Windows 本机编译：不链接 Linux 专用库（m/pthread/X11 均不存在于 MSVC）
    target_link_libraries(myplace)
else()
    # Linux/WSL：CImg 需要 X11，m/pthread 为系统库
    target_link_libraries(myplace m pthread X11)

    # Release 下显式开启最高优化（GCC 的 Release 默认即 -O3，此处固定下来，
    # 避免不同 CMake 版本/工具链的默认值差异导致构建产物变慢）
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(myplace PRIVATE -O3)
    endif()
endif()
