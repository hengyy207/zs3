#ifndef INITIAL_PLACEMENT_H
#define INITIAL_PLACEMENT_H

#include "placedata.h"
#include "common.h"

// ========== 初始布局模块（任务4） ==========
// 作用：给 21 万可移动单元在芯片核心区摆一个"初始位置"，
//       作为任务5 全局布局的起点。不追求精确、不要求不重叠。
// 三种方法（由易到难）：随机 / 聚类驱动 / 二次方程解析
class InitialPlacer
{
public:
    InitialPlacer(PlaceData *_db) { db = _db; }
    PlaceData *db;

    POS_2D coreLL; // 核心区左下角
    POS_2D coreUR; // 核心区右上角

    // 从 .scl 解析出的行信息推算出核心区域
    void SetupCoreRegion();

    // ① 计算 HPWL（半周长线长）—— 布局质量的核心指标
    //    所有网络：maxX-minX + maxY-minY 之和
    float CalcHPWL();

    // ② 方法①：随机布局（最简单，不存在算法，纯粹对照组）
    void RandomPlacement();

    // ③ 方法②：聚类驱动布局（分而治之：先聚类成簇，再簇内布局）
    void ClusteringPlacement();

    // ④ 方法③：二次方程解析布局（最小化二次函数 → 解 Ax=b）
    //    用 Eigen 稀疏矩阵 + BiCGSTAB 求解器，任务4 的核心方法
    void QuadraticPlacement();

    // ⑤ 密度统计：把核心区切成网格，统计"过满"网格和峰值密度
    //    衡量布局的"面积控制"能力（单元挤不挤/重不重叠）
    void ReportDensity();

    // ⑥ 获取当前布局的峰值密度（供主程序动态对比表使用）
    float GetPeakDensity();
};
#endif
