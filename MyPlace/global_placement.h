#ifndef GLOBAL_PLACEMENT_H
#define GLOBAL_PLACEMENT_H

#include "placedata.h"
#include "common.h"
#include <map>

// ========== 全局布局模块（任务5） ==========
// ePlace 方法：静电类比密度建模 + Nesterov 加速梯度下降
// 目标函数：min f(x,y) = W(x,y) + λ·N(x,y)
//   W = 可导线长（WA 加权平均模型，步骤 A 完成）
//   N = 密度惩罚（静电系统势能，步骤 B/C 实现）
//   λ = 惩罚系数（步骤 D 动态调节）
//
// 当前阶段：任务5-B —— 密度建模（网格划分 + 面积分布 + Filler 填充）
class GlobalPlacer
{
public:
    GlobalPlacer(PlaceData *_db) { db = _db; }
    PlaceData *db;

    // 核心区域（由行信息推算，与任务4保持一致）
    POS_2D coreLL;
    POS_2D coreUR;
    void SetupCoreRegion();

    // ========== 性能优化：Module* → Nodes 下标的快速索引 ==========
    // 原实现每轮用 map<Module*,int> 在 20 万级引脚热循环里做树查找，
    // 改为按 Module::idx（解析时分配的全局编号）直接数组寻址，一次构建反复使用。
    std::vector<int> nodeIdxOf;  // nodeIdxOf[Module::idx] = 在 db->Nodes 中的下标，-1 表示非可移动
    void BuildNodeIndex();

    // ========== A. 可导线长模型（WA 加权平均，论文式6） ==========
    //
    //   We = [Σ xi·exp(xi/γ)]/[Σ exp(xi/γ)]
    //      − [Σ xi·exp(−xi/γ)]/[Σ exp(−xi/γ)]      （x 方向）
    //
    // 为什么用 WA 而不是直接用 HPWL？
    //   HPWL 在 pin 坐标处"不可导"（max/min 是折点），梯度法用不了。
    //   WA 是可导的光滑函数，γ 越小越逼近 HPWL（折点处被"磨圆"）。
    float smoothGamma;                // 平滑参数 γ：越小越接近 HPWL，但梯度越"尖"、越易震荡
    double CalcSmoothWirelength();    // 平滑线长 W(v) = Σ_nets weight·(WAx + WAy)
    void CalcWirelengthGradient(std::vector<POS_2D> &grad); // grad[i] = ∂W/∂(x,y)，对应 db->Nodes[i]
    float CalcHPWL();                 // 精确 HPWL（半周长线长，对比基准/收敛判断用）

    // 调试自检：验证 WA 模型正确性（W 逼近 HPWL、解析梯度 vs 有限差分）
    void TestWirelengthModel();

    // ========== B. 密度建模（eDensity） ==========
    // 把芯片核心区切成 m×m 的网格（bin），每个单元/填充/固定对象
    // 按其面积分摊到覆盖的 bin 上，得到"拥挤程度"分布。
    // 密度分布是求解泊松方程（步骤C）的输入。

    // Filler：往空白区塞的"假单元"（面积 = 移动单元中位数水平，不连任何网络）
    // 作用是顶住密度力，让真实单元能贴着填充区挤得更近（线长更短）。
    struct FillerCell
    {
        double x, y;   // 中心坐标
        double area;   // 面积（所有 filler 相同）
    };
    std::vector<FillerCell> fillers; // 当前所有 filler

    int gridM;                 // 网格维度 m×m（取 2 的幂，如 128）
    double binW, binH;         // 单个 bin 的宽、高（物理单位）
    double binArea;            // bin 面积 = binW × binH
    double targetDensity;      // 目标密度 τ（参考 PDF：填充到 80%）

    // 三个密度场（网格单位 = 面积 / binArea，可理解为"占了几格面积"）
    std::vector<std::vector<double>> nodeDensity;     // 可移动单元
    std::vector<std::vector<double>> fillerDensity;   // 填充单元
    std::vector<std::vector<double>> terminalDensity; // 固定单元（面积已按 τ 缩放）

    void InitGrid();          // 划分网格、确定 bin 尺寸与目标密度
    void InsertFiller();      // 计算 filler 面积/数量，均匀撒入核心区
    void DistributeDensity(); // 把 cell/filler/terminal 面积分摊到 bin 网格

    // 密度统计（步骤B 自检用）：返回密度溢出率 τ（式37），并打印网格概况
    double CalcDensityOverflow();
    void ReportDensityGrid();

    // ========== C. 静电求解：泊松方程 + 密度梯度 ==========
    // 把"拥挤程度"（密度）变成"电势"和"电场力"：
    //   ① 总密度 ρ → 2D DCT → 频域解 ψ̂=ρ̂/(w²) → 逆变换 → 电势 ψ
    //   ② 电势 ψ 网格中心差分 → 电场 E = −∇ψ
    //   ③ 每个单元处插值电场 → 密度梯度 ∇N = −q·E（Lorentz 力，式16）
    std::vector<std::vector<double>> potential; // 电势 ψ 网格（bin 中心采样）
    std::vector<std::vector<double>> fieldX;    // 电场 Ex 网格
    std::vector<std::vector<double>> fieldY;    // 电场 Ey 网格
    std::vector<POS_2D> fillerGrad;             // filler 的密度梯度（步骤D 更新位置用）

    void SolvePoisson();         // 总密度 → DCT 求解泊松方程 → potential
    void ComputeElectricField(); // potential → fieldX/fieldY
    double CalcDensityGradient(std::vector<POS_2D> &grad); // 密度梯度 → grad（Nodes），返回势能 N
    double CalcPotentialEnergy();// 势能 N = ½Σq·ψ（密度惩罚，越小越均匀）

    void TestDensityModel();     // 自检：DCT 重建 + 泊松残差 + 单点电荷

    // ========== D. Nesterov 迭代 + 参数调节 ==========
    //   Algorithm 2（论文5.2）：u/v 双解序列，Lipschitz 预测步长 α=1/L
    //   λ 初始化（式35：线长力/密度力比值），迭代更新（式36：HPWL 变化率）
    //   γ 更新（式38：γ = 8·wb·10^(20/9·τ − 11/9)）
    //   τ（密度溢出率）< τ_min 时停止，或迭代次数超限
    struct NesterovState
    {
        vector<POS_2D> u;   // 主解序列（当前最佳布局）
        vector<POS_2D> v;   // 参考解序列（"探头"，比 u 多走一步）
        vector<POS_2D> fGrad;// 当前总梯度（线长 + λ×密度）
        vector<POS_2D> prevV;// 上一轮的 v（Lipschitz 预测用）
        vector<POS_2D> prevGrad; // 上一轮的梯度（Lipschitz 预测用）
        double a;            // Nesterov 参数 a_k
        double lambda;       // 惩罚系数 λ（线长 vs 密度的平衡）
        double alpha;        // 当前步长 α = 1/L
        double gamma;        // WA 平滑参数 γ
    };

    void InitLambda();           // λ 初始值 = Σ|Wgrad| / Σ|q·E|
    void UpdateLambda(double hpwlDelta); // λ 迭代更新（式36）
    void UpdateGamma();          // γ 迭代更新（式38）
    void UpdatePositions(vector<POS_2D> &coords, const vector<POS_2D> &grad, double step);
    double ComputeGradient(NesterovState &ns); // 计算总梯度 ∇f = ∇W + λ·∇N
    double NesterovStep(NesterovState &ns, double step); // 一次 Nesterov 迭代

    int iterCount;               // 已迭代轮次
    int maxIters;                // 最大迭代轮次（默认 5000）
    double tauMin;               // 停止条件：τ < τ_min（默认 0.05）
    NesterovState ns;            // 当前迭代状态

    // Nesterov 迭代的工作缓冲（成员持有、跨轮复用）。
    // 每轮要做 6 次 45 万元素级的 vector 拷贝（约 20 MB 内存流量），
    // 改用固定缓冲 + swap 后拷贝量降到接近零。
    std::vector<POS_2D> bufU, bufV;

    void RunGlobalPlacement(int iters = 5000, double tauMin = 0.05);
};

#endif
