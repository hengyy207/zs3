#include "myplace.h"
#include "placedata.h"
#include "initial_placement.h"
#include "global_placement.h"
#include "legalization.h"       // 任务7：布局合法化（Abacus）
#include "plot.h"               // 任务6：绘图
#include <iostream>
#include <fstream>
#include <ctime>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
using namespace std;

// 递归创建目录（等价 mkdir -p）。已存在或创建失败都直接忽略，
// 因为最常见的调用情形就是「调用方（如 Web 服务）已经建好了目录」。
static void EnsureDir(const string &path)
{
    if (path.empty()) return;
    string cur;
    for (size_t i = 0; i < path.size(); i++)
    {
        cur += path[i];
        const bool isSep = (path[i] == '/' || path[i] == '\\');
        if (!isSep && i + 1 != path.size()) continue;
        if (cur == "/" || cur == "./" || cur == ".\\") continue;
#ifdef _WIN32
        _mkdir(cur.c_str());
#else
        ::mkdir(cur.c_str(), 0755);
#endif
    }
}

// 辅助：判断文件是否存在
static bool FileExists(const string& path)
{
    ifstream f(path.c_str());
    return f.good();
}

// 辅助：程序可能从构建目录(如 out/build/WSL-GCC-Debug)启动，此时相对路径 data/ 找不到。
// 这里从当前目录逐级向上搜索 data/<文件名>，返回一个真正能读到的路径。
static string ResolveDataFile(const string& auxFile)
{
    if (FileExists(auxFile)) return auxFile;

    // 提取文件名部分：data/adaptec1.aux -> adaptec1.aux
    size_t slash = auxFile.find_last_of("/\\");
    string base = (slash == string::npos) ? auxFile : auxFile.substr(slash + 1);

    // 从当前目录向上逐层找 <目录>/data/<文件名>
    string prefix = ".";
    for (int i = 0; i < 10; i++)
    {
        string dir = (i == 0) ? "." : prefix;
        string cand = dir + "/data/" + base;
        if (FileExists(cand)) return cand;
        prefix += "/..";
    }
    return auxFile; // 找不到则原样返回，交给解析器正常报错
}

// ========== 主程序 ==========
// 命令行选项：
//   无参数  → 完整模式：三种初始布局 + 自检 + 全局布局 + 可视化 + 合法化
//   --fast  → 快速模式：跳过三种初始布局对比和自检，直接 随机布局→全局布局→输出
int main(int argc, char *argv[])
{
    // 关闭 stdout 缓冲，日志实时输出
    // （VS 远程调试时 stdout 不是终端、会全缓冲，日志会被憋住直到程序退出才显示）
    setvbuf(stdout, NULL, _IONBF, 0);

    string auxFile = "data/adaptec1.aux";
    string outDir  = "output";
    bool fast = false;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        if (a == "--fast") fast = true;
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i]; // 输出目录（Web 端按请求隔离用）
        else auxFile = a;
    }

    // 规范化输出目录：去掉结尾分隔符，创建后统一补 "/" 交给绘图模块。
    // 每个请求写自己的目录，避免并发分析时互相覆盖 output/*.bmp|gds。
    while (outDir.size() > 1 && (outDir[outDir.size() - 1] == '/' || outDir[outDir.size() - 1] == '\\'))
        outDir.erase(outDir.size() - 1);
    EnsureDir(outDir);
    PLOTTING::outputDir = outDir + "/";
    // 解析输入路径：VS 从构建目录启动也能自动定位到 data/
    auxFile = ResolveDataFile(auxFile);

    // 创建数据仓库和布局主类
    PlaceData *placedata = new PlaceData();
    MyPlacer *myplacer = new MyPlacer(placedata);

    // 收集各方法的实际运行结果（供动态对比表使用）
    float randHPWL = 0, clusterHPWL = 0, quadHPWL = 0;
    float randPeakDensity = 0, clusterPeakDensity = 0, quadPeakDensity = 0;
    double randTime = 0, clusterTime = 0, quadTime = 0;
    float globalHPWL = 0, globalPeakDensity = 0;
    double globalTime = 0;

    // ① 任务3：解析 6 个 BookShelf 文件
    myplacer->Run(auxFile);
    myplacer->PrintOverview();

    // ② 任务4：初始布局
    InitialPlacer *iplacer = new InitialPlacer(placedata);
    iplacer->SetupCoreRegion(); // 从行信息推算核心区域

    // 基线：解析后的初始 HPWL（单元全部堆在原点时）
    float initHPWL = iplacer->CalcHPWL();
    cout << "\n[基线] 初始 HPWL（全部堆在原点）= " << initHPWL << endl;

    // ========== 快速模式：跳过三种初始布局，直接进全局布局 ==========
    if (fast)
    {
        printf("\n===== --fast 快速模式 =====\n");
        printf("跳过：三种初始布局对比 + 5-A/5-B/5-C 自检\n");
        printf("流程：解析 → 随机布局 → Nesterov全局布局 → 可视化 → 合法化\n\n");

        // 随机初始布局（~0.03秒）
        iplacer->RandomPlacement();

        // 创建全局布局器
        GlobalPlacer *gplacer = new GlobalPlacer(placedata);
        gplacer->SetupCoreRegion();

        float preGlobalHPWL = gplacer->CalcHPWL();
        printf("[fast] 初始 HPWL（随机布局）= %.0f\n", preGlobalHPWL);

        // Nesterov 全局布局（内部自动 InitGrid + InsertFiller）
        clock_t globalT0 = clock();
        gplacer->RunGlobalPlacement(500, 0.001);
        clock_t globalT1 = clock();
        globalTime = double(globalT1 - globalT0) / CLOCKS_PER_SEC;
        globalHPWL = gplacer->CalcHPWL();
        globalPeakDensity = iplacer->GetPeakDensity();
        printf("[fast] 最终 HPWL = %.0f   改善 = %.1f%%   耗时 = %.3fs\n",
               globalHPWL, (1.0 - globalHPWL / preGlobalHPWL) * 100, globalTime);

        // 任务6：可视化
        printf("\n===== 任务6：布局结果可视化 =====\n");
        PLOTTING::plotPlacement("global_placement_result", placedata);
        PLOTTING::writeGDSII(PLOTTING::outputDir + "global_placement_result.gds", placedata);
        printf("===== 任务6 完成 =====\n");
        printf("输出文件：\n");
        printf("  - %sglobal_placement_result.bmp\n", PLOTTING::outputDir.c_str());
        printf("  - %sglobal_placement_result.gds\n\n", PLOTTING::outputDir.c_str());

        // 任务7：合法化
        printf("===== 任务7：布局合法化 =====\n");
        float hpwlBeforeLeg = gplacer->CalcHPWL();
        Legalizer *legalizer = new Legalizer(placedata);
        legalizer->Run();
        float hpwlAfterLeg = gplacer->CalcHPWL();
        printf("[合法化] HPWL: %.0f -> %.0f  退化 %.2f%%\n",
               hpwlBeforeLeg, hpwlAfterLeg,
               (hpwlAfterLeg - hpwlBeforeLeg) / hpwlBeforeLeg * 100);
        PLOTTING::plotPlacement("legalization_result", placedata);
        PLOTTING::writeGDSII(PLOTTING::outputDir + "legalization_result.gds", placedata);
        printf("===== 任务7 完成 =====\n");
        printf("输出文件：\n");
        printf("  - %slegalization_result.bmp\n", PLOTTING::outputDir.c_str());
        printf("  - %slegalization_result.gds\n\n", PLOTTING::outputDir.c_str());

        delete legalizer;
        delete gplacer;
        delete iplacer;
        delete myplacer;
        delete placedata;
        return 0;
    }

    // ========== 完整模式 ==========

    // 方法①：随机布局
    clock_t t0 = clock();
    iplacer->RandomPlacement();
    clock_t t1 = clock();
    randTime = double(t1 - t0) / CLOCKS_PER_SEC;
    randHPWL = iplacer->CalcHPWL();
    randPeakDensity = iplacer->GetPeakDensity();
    cout << "[方法①] 随机布局  HPWL = " << randHPWL
         << "  用时 = " << randTime << " 秒" << endl;

    // 方法②：聚类驱动布局
    t0 = clock();
    iplacer->ClusteringPlacement();
    t1 = clock();
    clusterTime = double(t1 - t0) / CLOCKS_PER_SEC;
    clusterHPWL = iplacer->CalcHPWL();
    clusterPeakDensity = iplacer->GetPeakDensity();
    cout << "[方法②] 聚类驱动布局 HPWL = " << clusterHPWL
         << "  用时 = " << clusterTime << " 秒" << endl;

    // 方法③：二次方程解析布局
    t0 = clock();
    iplacer->QuadraticPlacement();
    t1 = clock();
    quadTime = double(t1 - t0) / CLOCKS_PER_SEC;
    quadHPWL = iplacer->CalcHPWL();
    quadPeakDensity = iplacer->GetPeakDensity();
    cout << "[方法③] 二次方程解析布局 HPWL = " << quadHPWL
         << "  用时 = " << quadTime << " 秒" << endl;

    // 任务5 全局布局实例（任务7 的 HPWL 计算也复用 gplacer）
    GlobalPlacer *gplacer = new GlobalPlacer(placedata);
    gplacer->SetupCoreRegion();

    // ③ Nesterov 全局布局（500 轮，τ<0.1% 时停止）
    // 从随机布局开始（τ≈0%），让 Nesterov 同时优化线长和密度
    // （按精简需求：已移除 5-A 线长自检 / 5-B 密度自检 / 5-C 静电自检，
    //   全局布局器内部仍会自动 InitGrid+InsertFiller，不影响结果）
    iplacer->RandomPlacement(); // 重置为随机布局
    gplacer->SetupCoreRegion(); // 重新初始化核心区域
    float preGlobalHPWL = gplacer->CalcHPWL();
    printf("\n[任务5] 初始 HPWL（随机布局）= %.0f\n", preGlobalHPWL);
    clock_t globalT0 = clock();
    gplacer->RunGlobalPlacement(500, 0.001);
    clock_t globalT1 = clock();
    globalTime = double(globalT1 - globalT0) / CLOCKS_PER_SEC;
    globalHPWL = gplacer->CalcHPWL();
    globalPeakDensity = iplacer->GetPeakDensity(); // 复用峰值密度计算
    printf("[任务5] 最终 HPWL = %.0f   改善 = %.1f%%\n",
           globalHPWL, (1.0 - globalHPWL / preGlobalHPWL) * 100);

    // ⑦ 动态生成对比分析表（任务4 vs 任务5）
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        任务4 vs 任务5 对比分析（实际运行数据）                  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ 方法                │  HPWL           │ 峰值密度     │ 耗时        │ 特点  ║\n");
    printf("╠══════════════════════╪═════════════════╪══════════════╪═════════════╪═══════╣\n");
    printf("║ ①随机布局           │ %10.2e   │ %8.1f%%   │ %7.3f s   │ 对照组║\n",
           randHPWL, randPeakDensity * 100, randTime);
    printf("║ ②聚类驱动           │ %10.2e   │ %8.1f%%   │ %7.3f s   │ 连接好║\n",
           clusterHPWL, clusterPeakDensity * 100, clusterTime);
    printf("║ ③二次方程           │ %10.2e   │ %8.1f%%   │ %7.3f s   │ 线长优║\n",
           quadHPWL, quadPeakDensity * 100, quadTime);
    printf("║ ④Nesterov全局布局    │ %10.2e   │ %8.1f%%   │ %7.3f s   │ 平衡  ║\n",
           globalHPWL, globalPeakDensity * 100, globalTime);
    printf("╚══════════════════════╧═════════════════╧══════════════╧═════════════╧═══════╝\n");
    printf("\n");

    // 精简说明：核心结论文字与上方对比表信息重复，按需求移除，
    // 表格中的 HPWL / 峰值密度 / 耗时即对比与优化的核心数据。

    // ========== 任务6：布局结果可视化 ==========
    printf("===== 任务6：布局结果可视化 =====\n");

    // 创建输出目录（Linux/WSL）

    // 方式一：CImg 画 BMP 图片
    PLOTTING::plotPlacement("global_placement_result", placedata);

    // 方式二：输出 GDSII 文件（用 KLayout 查看）
    PLOTTING::writeGDSII(PLOTTING::outputDir + "global_placement_result.gds", placedata);

    printf("===== 任务6 完成 =====\n");
    printf("输出文件：\n");
    printf("  - %sglobal_placement_result.bmp （用图片查看器打开）\n", PLOTTING::outputDir.c_str());
    printf("  - %sglobal_placement_result.gds （用 KLayout 打开）\n", PLOTTING::outputDir.c_str());
    printf("KLayout 下载：https://www.klayout.de/\n");
    printf("\n");

    // ========== 任务7：布局合法化（选做加分项） ==========
    printf("===== 任务7：布局合法化 =====\n");

    // 合法化前 HPWL（全局布局结果）
    float hpwlBeforeLeg = gplacer->CalcHPWL();

    // 用 Abacus 算法消除标准单元重叠、对齐到行
    Legalizer *legalizer = new Legalizer(placedata);
    legalizer->Run();

    // 合法化后 HPWL（对比退化幅度）
    float hpwlAfterLeg = gplacer->CalcHPWL();
    printf("[合法化] HPWL: %.0f -> %.0f  退化 %.2f%%\n",
           hpwlBeforeLeg, hpwlAfterLeg,
           (hpwlAfterLeg - hpwlBeforeLeg) / hpwlBeforeLeg * 100);

    // 输出合法化结果图（与任务6的全局布局图对比，直观看到"收拾干净"的效果）
    PLOTTING::plotPlacement("legalization_result", placedata);
    PLOTTING::writeGDSII(PLOTTING::outputDir + "legalization_result.gds", placedata);

    printf("===== 任务7 完成 =====\n");
    printf("输出文件：\n");
    printf("  - %slegalization_result.bmp （合法化后图片）\n", PLOTTING::outputDir.c_str());
    printf("  - %slegalization_result.gds  （合法化后，用 KLayout 打开与全局布局对比）\n", PLOTTING::outputDir.c_str());
    printf("\n");

    delete legalizer;
    delete gplacer;

    delete iplacer;
    delete myplacer;
    delete placedata;
    return 0;
}
