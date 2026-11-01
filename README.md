# MyPlacement

从零实现的芯片布局（Placement）工具链：解析 UCLA BookShelf 格式电路 → 三种初始布局对比 → ePlace 全局布局（WA 线长 + 静电密度 + Nesterov）→ 位图 / GDSII 可视化 → Abacus 合法化，并提供本地 Web 前端做可视化操作。

《工业软件创新训练 III / 进阶式挑战性综合项目 III》课程项目。

---

## 一、功能一览

| 阶段 | 内容 | 代码位置 |
| --- | --- | --- |
| 任务 3 | 解析 BookShelf 六件套（`.aux .scl .nodes .nets .pl .wts`），建立 Module / Pin / Net / SiteRow 数据结构并统计 | `MyPlace/myplace.cpp` |
| 任务 4 | 三种初始布局：随机撒点、连接强度聚类、二次方程解析（Eigen 稀疏 + BiCGSTAB） | `MyPlace/initial_placement.cpp` |
| 任务 5 | ePlace 全局布局：对数-和（WA）可导线长 → 网格密度建模 + Filler → DCT 求解泊松方程得电场 → Nesterov 加速梯度下降 | `MyPlace/global_placement.cpp` |
| 任务 6 | 双路可视化：CImg 输出 BMP、手写 GDSII 二进制流（KLayout 可直接打开） | `Plot/plot.cpp`、`Plot/gdsii.cpp` |
| 任务 7 | Abacus 最小位移合法化：行结构 + 障碍区间 + 3 候选行试放 | `MyPlace/legalization.cpp` |
| 附加 | Flask + 单页前端（数据 / 分析 / 结果三页），本地可视化运行 | `web/` |

---

## 二、目录结构

```
MyPlacement/
├── CMakeLists.txt              # 构建脚本（未指定构建类型时默认 Release）
├── CMakeSettings.json          # Visual Studio 的 WSL 配置（WSL-GCC-Release）
├── myplace_main.cpp            # 程序入口：参数解析 + 任务 3→7 全流程调度
├── Common/
│   ├── common.h                # POS_2D / VECTOR_3D 基础类型
│   └── objects.h               # Module / Pin / Net / SiteRow 实体
├── PlaceData/placedata.h       # 数据仓库（唯一数据源）
├── MyPlace/                    # 算法核心，与课程任务一一对应
├── Plot/                       # 可视化输出（BMP + GDSII）
├── Library/CImg/               # 第三方：CImg 图像库（已内置，无需下载）
├── third_party/eigen3/         # 第三方：Eigen 线性代数库（已内置）
├── data/                       # 示例数据：thin1（玩具电路）/ adaptec1（21 万单元）
├── web/                        # Flask 后端 + 单页前端
├── MyPlacement综合设计报告.md    # 综合设计报告
├── gen_reports.py              # 由正文内容生成 Word 版报告（需学校模板）
└── README.md                   # 本文件
```

构建产物（`build/`、`out/`）与运行输出（`output/`）不入库，见 `.gitignore`。

---

## 三、运行环境

程序**只能在 Linux / WSL 下编译运行**：绘图依赖 CImg + X11，Windows 原生（MSVC）可以编译但会自动关闭绘图。内核本身是纯 C++14，另外两个第三方库已随仓库提供，离线可用。

| 依赖 | 版本 / 说明 |
| --- | --- |
| 操作系统 | Linux x86_64，或 Windows 11 + WSL2（Ubuntu） |
| 编译器 | g++，支持 C++14 |
| 构建 | CMake ≥ 3.5（建议 3.16+）、Ninja 或 Make |
| 系统库 | `libx11-dev`（CImg 需要） |
| Web 前端 | Python 3 + Flask（仅使用网页时需装） |

Ubuntu / WSL 一次性安装：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build g++ libx11-dev python3-pip
pip3 install flask
```

---

## 四、快速开始

```bash
# 1. 编译（在工程根目录；产物为 build/myplace）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 2. 命令行跑一次小数据集，秒级验证
./build/myplace --fast data/thin1.aux

# 3. 跑默认大数据集（21 万单元，Release 约 36 秒）
./build/myplace --fast
```

启动 Web 界面：

```bash
cd web
./start.sh          # 或 python3 server.py
# 浏览器打开 http://localhost:5000
```

在 Windows 上使用 WSL 时，可直接在任意终端执行（把路径换成你放工程的目录）：

```bash
wsl bash -lc "cd /mnt/d/ideazy/zs3 && cmake -S . -B build -G Ninja && cmake --build build"
```

> 若 `build/` 里残留了别的编译器/生成器的缓存（报 "generator does not match"），先 `cmake --fresh -S . -B build ...`，或直接删掉 `build/` 重来。同一个 `build/` 目录不要混用 Debug 与 Release 配置。

---

## 五、命令行用法

```
myplace [--fast] [--out <目录>] [输入文件.aux]
```

| 参数 | 说明 |
| --- | --- |
| 不带参数 | 完整流程：解析 → 三种初始布局对比 → 全局布局 → 可视化 → 合法化 |
| `--fast` | 跳过初始布局对比与算法自检，直接 随机布局 → 全局布局 → 可视化 → 合法化（更快） |
| `--out <目录>` | 指定输出目录，默认 `output/`（Web 端为每个请求分配独立目录） |
| 末尾参数 | 输入 `.aux` 路径，默认 `data/adaptec1.aux` |

- 参数顺序任意；**没有 `--quick` 这个参数**，传了会被当成文件路径。
- 输入文件在工程内任意子目录启动都能找到：程序会自动向上查找 `data/<文件名>`（`ResolveDataFile`）。
- 输出目录相对**当前工作目录**，建议始终在工程根运行。

输出文件：

```
output/
├── global_placement_result.bmp    全局布局位图（图片查看器）
├── global_placement_result.gds    GDSII 版图（KLayout）
├── legalization_result.bmp        合法化后位图
└── legalization_result.gds        合法化后 GDSII
```

---

## 六、Web 界面

三页式（`#/data` → `#/run` → `#/result`），顶部步骤条可点击跳转，单文件零外部依赖，断网可用。

1. **数据**：拖拽或选择 6 个 BookShelf 文件，实时校验 `.aux .nodes .nets .pl .scl` 是否齐备；选择「快速分析」或「完整分析」。
2. **分析**：等宽计时器 + 5 段进度轨道，显示当前阶段。
3. **结果**：最终 HPWL 与改善幅度、指标卡、布局位图、四方法对比表、运行日志，可下载 BMP / GDSII。

后端行为：

- 自动定位工程根（`web/server.py` 的上一级）与可执行文件（优先 `build/myplace`，其次 `out/build/<配置>/myplace`），可用环境变量 `MYPLACE_ROOT`、`MYPLACE_BIN` 覆盖。
- 每个请求使用独立临时输出目录，通过 `--out` 传给 C++，请求结束自动清理，多标签页并发不会互相覆盖图片。
- 超时按模式区分：快速 240 秒、完整 600 秒（`TIMEOUT_FAST` / `TIMEOUT_FULL`）。
- 端口与监听地址可用 `MYPLACE_PORT` / `MYPLACE_HOST` 覆盖；调试器默认关闭（`MYPLACE_DEBUG=1` 开启）。

---

## 七、数据格式

BookShelf 格式，一套数据是 6 个同名文件：

| 文件 | 作用 |
| --- | --- |
| `.aux` | 清单，只记录其余 5 个文件名 |
| `.nodes` | 单元名称、尺寸、是否固定（terminal） |
| `.nets` | 网络与引脚偏移 |
| `.pl` | 初始坐标（`/FIXED` 表示锁定） |
| `.scl` | 行定义（行数、行高、站点宽度） |
| `.wts` | 网络权重（可为空，默认权重 1） |

内置数据集：

| 数据集 | 规模 | 用途 |
| --- | --- | --- |
| `data/thin1.aux` | 3 个可移动单元 / 2 个网络 | 秒级功能验证 |
| `data/adaptec1.aux` | 210,904 单元 / 221,142 网络 / 944,053 引脚 / 890 行 | 性能与质量评估（`.nets` 约 35 MB） |

---

## 八、性能参考

Release 构建、单核、adaptec1（21 万单元）实测：

| 场景 | 耗时 |
| --- | --- |
| `--fast` 全流程 | ≈ 36 秒 |
| 完整流程（含三种初始布局对比） | ≈ 56 秒 |
| 其中 Nesterov 全局布局（`--fast`） | ≈ 29 秒 |
| 合法化 | ≈ 4 秒 |

典型结果（`--fast`）：HPWL 2.21476e9 → 8.35075e8，改善约 62%；密度溢出率 τ ≈ 2.8%；合法化后重叠对 0，HPWL 退化 < 0.1%。数值会随数据集与运行轮次略有差异，以程序日志为准。

Debug 构建慢 2~3 倍，也是 Web 端超时的历史原因；CMakeLists 已在未指定时默认 Release。

---

## 九、常见问题

| 现象 | 原因与处理 |
| --- | --- |
| `未找到 myplace 可执行文件` | 还没编译，按「快速开始」编译一次再启动 Web |
| `分析超时` | 换「快速分析」；确认 `build/myplace` 是 Release 版（`cmake --fresh` 重配） |
| `找不到 data/xxx` | 从工程根启动，或传 `.aux` 的完整路径 |
| cmd 里双击 `myplace` 报"不是有效的 Win32 程序" | 它是 Linux ELF，只能在 WSL / Linux 下运行 |
| 链接报 X11 相关错误 | 装 `libx11-dev`；Windows 原生编译会自动关闭绘图 |
| 网页样式是旧的 | 浏览器缓存，Ctrl+F5 强刷 |
| 5000 端口被占用 | `MYPLACE_PORT=5001 python3 server.py` |
| 生成器不匹配（generator does not match） | `build/` 里是别的编译器的缓存，`cmake --fresh` 或删目录重配 |

---

## 十、第三方组件

| 组件 | 许可 | 用途 |
| --- | --- | --- |
| [Eigen](https://eigen.tuxfamily.org/) | MPL2 | 稀疏矩阵与 BiCGSTAB 迭代求解（任务 4） |
| [CImg](https://cimg.eu/) | CeCILL-C | BMP 位图输出（任务 6） |

两者均已内置在仓库内（`third_party/eigen3/`、`Library/CImg/`），无需额外下载。
