#ifndef LEGALIZATION_H
#define LEGALIZATION_H

#include "placedata.h"
#include "common.h"

// ========== 布局合法化模块（任务7，选做加分项） ==========
// 作用：把任务5全局布局的结果"收拾干净"：
//   ① 消除标准单元之间的重叠
//   ② 把每个标准单元对齐进 .scl 规定的"行"里（行高=单元高）
//   ③ 让总位移尽可能小（尽量少动单元，不破坏全局布局的线长优化成果）
//
// 算法：Abacus（论文：Spindler et al., ISPD 2006
//   "Abacus: Fast legalization of standard cell circuits with minimal movement"）
//   核心思想：
//     - 单元按 y 坐标降序逐个处理（先放顶部，后面的单元向下找行）
//     - 对每个单元，只在"离它最近的 K 个候选行"里试放
//     - 试放 = 行内按 x 排序，从插入点开始"整体右推"消除重叠（Clustering）
//     - 选总位移最小的那行正式落位
//   效果：复杂度近线性，总位移近似最优（论文证明 ≤ 最优解的 2 倍）

class Legalizer
{
public:
    Legalizer(PlaceData *_db) : db(_db) {}

    // 主入口：执行合法化，输出统计
    void Run();

private:
    PlaceData *db;

    // ========== 内部行结构：记录该行已放置的单元（按 x 排序） ==========
    struct LegRow
    {
        double left;    // 行左边界 x
        double right;   // 行右边界 x
        double bottom;  // 行底 y
        double height;  // 行高
        vector<Module *> cells; // 已放置的单元（按左下角 x 升序）
        vector<double> cellX;   // 每个单元的左下角 x（与 cells 一一对应）
        vector<pair<double, double>> obstacles; // 该行被固定终端占用的 x 区间（排序合并）
        LegRow() { left = right = bottom = height = 0; }
    };

    vector<LegRow> rows;   // 所有行（按 bottom 升序）
    int rowCnt;            // 行数

    // 候选行数量（论文通常取 3）
    static const int CAND_ROW = 3;

    // ========== 统计 ==========
    long long overlapBefore, overlapAfter; // 重叠单元对数（合法化前/后）
    double totalDisp, maxDisp, avgDisp;    // 位移统计（净位移，结束时统一计算）
    int misplacedBefore, misplacedAfter;   // 未对齐行的单元数（前/后）

    vector<POS_2D> origCenter; // 合法化前每个可移动单元的原始中心（用于算净位移）

    // ---------- 内部函数 ----------
    void SetupRows();                       // 从 .scl 的行信息建立 LegRow 列表
    void BuildObstacles();                  // 把固定终端在每行内的占用区间标记为障碍
    void FindCandidateRows(double cy, vector<int> &cands); // 找离 cy 最近的 K 行
    double TryPlace(LegRow &row, Module *c, double &bestX); // 试放：返回位移，放不下返回-1；bestX 输出放置位置
    void CommitPlace(LegRow &row, Module *c, double x);     // 正式放置到行内 x 位置，更新单元坐标
    void PlaceCell(Module *c, double x, double centerY); // 设置单元新坐标（左下角x, 中心y）
    void CalcDisplacement();                 // 结束时用 origCenter 统一计算净位移

    // ---------- 统计工具 ----------
    long long CountOverlaps();               // 网格法统计重叠单元对数
    int CountMisaligned();                   // 统计中心 y 不在任何行内的单元数
    void PrintRowInfo();                     // 打印行统计（行数、容量、利用率）
};

#endif
