#include "global_placement.h"
#include <cfloat>
#include <cstdlib>

// ================================================================
// 任务5 全局布局（ePlace 方法）—— 步骤 A：可导线长模型（WA）
// ================================================================

// ----------------------------------------------------------------
// 核心区域：所有行（SiteRow）的外接矩形（与任务4保持一致）
// ----------------------------------------------------------------
void GlobalPlacer::SetupCoreRegion()
{
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (size_t i = 0; i < db->SiteRows.size(); i++)
    {
        SiteRow &r = db->SiteRows[i];
        minX = min(minX, (float)r.start.x);
        minY = min(minY, (float)r.start.y);
        maxX = max(maxX, (float)r.end.x);
        maxY = max(maxY, (float)(r.bottom + r.height));
    }
    coreLL = POS_2D(minX, minY);
    coreUR = POS_2D(maxX, maxY);
}

// ----------------------------------------------------------------
// HPWL（半周长线长）：与任务4完全相同的精确指标，作对比基准
// ----------------------------------------------------------------
float GlobalPlacer::CalcHPWL()
{
    double total = 0;
    for (size_t i = 0; i < db->Nets.size(); i++)
    {
        Net *net = db->Nets[i];
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;

        for (size_t j = 0; j < net->netPins.size(); j++)
        {
            Pin *pin = net->netPins[j];
            if (pin->module == NULL) continue; // 防御：找不到单元的引脚跳过
            float x = pin->module->center.x + pin->offset.x;
            float y = pin->module->center.y + pin->offset.y;
            minX = min(minX, x);
            maxX = max(maxX, x);
            minY = min(minY, y);
            maxY = max(maxY, y);
        }
        total += (double)(maxX - minX) + (double)(maxY - minY);
    }
    return (float)total;
}

// ----------------------------------------------------------------
// 一条网络在某个方向上的 WA（加权平均）线长模型（论文式6）
// ----------------------------------------------------------------
// 数学原理（以 x 方向为例）：
//   We = [Σ xi·exp(xi/γ)]/[Σ exp(xi/γ)]   （加权平均"靠向最大侧"）
//      − [Σ xi·exp(−xi/γ)]/[Σ exp(−xi/γ)] （加权平均"靠向最小侧"）
//   γ→0 时，第一项 → max(xi)，第二项 → min(xi)，We → 跨度 = HPWL 分量。
// 数值稳定性：
//   exp(xi/γ) 在 xi 很大时可能溢出。因此把指数参数整体减去最大值/最小值，
//   保证参数 ≤ 0，只影响公共因子（分子分母约掉），结果不变。
// ----------------------------------------------------------------
// 参数：
//   pins  - 该网络的所有引脚
//   isX   - true 算 x 方向，false 算 y 方向
//   gamma - 平滑参数 γ
//   wPlus/wMinus - 输出：每个 pin 的 exp 权重（供梯度复用，避免重复计算）
//   denPlus/denMinus - 输出：两组权重之和（同上）
//   aPlusOut/aMinusOut - 输出：A+ = Σv·wPlus/denPlus，A− = Σv·wMinus/denMinus（梯度修正项用）
// 返回：该方向上的平滑线长 = A+ − A−
static double WaSmooth(const vector<Pin *> &pins, bool isX, float gamma,
                       vector<double> &wPlus, vector<double> &wMinus,
                       double &denPlus, double &denMinus,
                       double &aPlusOut, double &aMinusOut)
{
    size_t np = pins.size();
    // 取该方向上所有 pin 坐标的最大值/最小值
    double maxv = -DBL_MAX, minv = DBL_MAX;
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = isX ? (p->module->center.x + p->offset.x)
                       : (p->module->center.y + p->offset.y);
        if (v > maxv) maxv = v;
        if (v < minv) minv = v;
    }

    // 退化情况：单 pin 或所有 pin 挤在一点 → 该方向线长为 0
    if (np < 2 || gamma <= 1e-9f || maxv - minv < 1e-9)
    {
        wPlus.assign(np, 0.0);
        wMinus.assign(np, 0.0);
        denPlus = denMinus = 1.0;
        aPlusOut = aMinusOut = 0.0;
        return 0.0;
    }

    // 计算权重（指数参数始终 ≤ 0，不会溢出）
    wPlus.resize(np);
    wMinus.resize(np);
    denPlus = 0.0;
    denMinus = 0.0;
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = isX ? (p->module->center.x + p->offset.x)
                       : (p->module->center.y + p->offset.y);
        wPlus[i]  = exp((v - maxv) / gamma);  // 最大 pin 处权重 = 1
        wMinus[i] = exp((minv - v) / gamma);  // 最小 pin 处权重 = 1
        denPlus  += wPlus[i];
        denMinus += wMinus[i];
    }

    // A+ = Σv·wPlus/denPlus：用 maxv 还原坐标，避免大数相减的精度损失
    // A− = Σv·wMinus/denMinus：注意是"减"下面这一项（易错点！）
    double aPlus = maxv, aMinus = minv;
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = isX ? (p->module->center.x + p->offset.x)
                       : (p->module->center.y + p->offset.y);
        aPlus  += (v - maxv) * wPlus[i]  / denPlus;   // maxv + Σ(v-max)·w/den = Σv·w/den ✓
        aMinus -= (minv - v) * wMinus[i] / denMinus;  // minv − Σ(min-v)·w/den = Σv·w/den ✓
    }
    aPlusOut = aPlus;
    aMinusOut = aMinus;
    return aPlus - aMinus;
}

// ----------------------------------------------------------------
// 平滑线长 W(v) = Σ_所有网络 (WAx + WAy)
// ----------------------------------------------------------------
double GlobalPlacer::CalcSmoothWirelength()
{
    double total = 0;
    // 【性能优化】权重缓冲移到循环外复用：原来每个网络都新建 4 个 vector，
    // WaSmooth 内部的 resize 会触发堆分配，22 万网络 × 128 轮累计上亿次
    // malloc/free，是全局布局最主要的隐性开销之一。容量只会增长不会收缩，
    // 几轮之后即可稳定覆盖最宽的网络，后续零分配。
    vector<double> wpx, wmx, wpy, wmy;
    for (size_t i = 0; i < db->Nets.size(); i++)
    {
        Net *net = db->Nets[i];
        if (net->netPins.size() < 2) continue;
        double dpx, dmx, dpy, dmy, apx, amx, apy, amy;
        double wax = WaSmooth(net->netPins, true,  smoothGamma, wpx, wmx, dpx, dmx, apx, amx);
        double way = WaSmooth(net->netPins, false, smoothGamma, wpy, wmy, dpy, dmy, apy, amy);
        total += wax + way;
    }
    return total;
}

// ----------------------------------------------------------------
// 线长梯度 grad[i] = ∂W/∂(x_i, y_i)，对应 db->Nodes[i]
// ----------------------------------------------------------------
// 由 WA 公式完整求导（pin 绝对坐标 = center + offset，offset 恒定）：
//   ∂We/∂v_j = [wPlus[j] + (v_j − A+)·wPlus[j]/γ] / denPlus
//             − [wMinus[j] − (v_j − A−)·wMinus[j]/γ] / denMinus
// 注意：除了"主导权重" w/den，还有 (v_j − A±)/γ 的修正项。
// 当 γ 远大于网络坐标跨度时（弱平滑），两组权重大致抵消，
// 修正项反而主导，因此不可省略。
// 一条网络内，同一单元出现多个引脚时，各自贡献累加。
// 固定单元（Terminal）不移动，不写入梯度。
// ----------------------------------------------------------------
void GlobalPlacer::BuildNodeIndex()
{
    nodeIdxOf.assign(db->moduleCount > 0 ? db->moduleCount : 0, -1);
    for (int i = 0; i < (int)db->Nodes.size(); i++)
    {
        const int gid = db->Nodes[i]->idx;
        if (gid >= 0 && gid < (int)nodeIdxOf.size()) nodeIdxOf[gid] = i;
    }
}

void GlobalPlacer::CalcWirelengthGradient(vector<POS_2D> &grad)
{
    int N = (int)db->Nodes.size();
    grad.assign(N, POS_2D());
    if (N == 0) return;

    // Module → Nodes 下标的映射：改用按全局编号直接寻址的数组（只构建一次）。
    // 原实现每轮都建 map<Module*,int>（20 万次红黑树插入），并在内层引脚循环
    // 里做 O(log n) 查找——本函数每轮调用一次，是纯浪费。
    if ((int)nodeIdxOf.size() != db->moduleCount) BuildNodeIndex();

    const double g = (double)smoothGamma;
    // 【性能优化】同 CalcSmoothWirelength：权重缓冲复用，消除每网络 4 次堆分配
    vector<double> wpx, wmx, wpy, wmy;
    for (size_t i = 0; i < db->Nets.size(); i++)
    {
        Net *net = db->Nets[i];
        const vector<Pin *> &pins = net->netPins;
        size_t np = pins.size();
        if (np < 2) continue;

        // 一次性求出 x/y 两个方向的权重与 A±，供梯度复用
        double dpx, dmx, dpy, dmy, apx, amx, apy, amy;
        WaSmooth(pins, true,  smoothGamma, wpx, wmx, dpx, dmx, apx, amx);
        WaSmooth(pins, false, smoothGamma, wpy, wmy, dpy, dmy, apy, amy);

        for (size_t j = 0; j < np; j++)
        {
            Module *m = pins[j]->module;
            if (m == NULL || m->isFixed) continue;
            const int gid = m->idx;
            if (gid < 0 || gid >= (int)nodeIdxOf.size()) continue;
            const int idx = nodeIdxOf[gid];
            if (idx < 0) continue; // 该单元不是可移动 Node

            // x 方向：完整链式法则梯度
            double vx = (double)m->center.x + pins[j]->offset.x;
            double gx = (wpx[j] + (vx - apx) * wpx[j] / g) / dpx
                      - (wmx[j] - (vx - amx) * wmx[j] / g) / dmx;
            // y 方向
            double vy = (double)m->center.y + pins[j]->offset.y;
            double gy = (wpy[j] + (vy - apy) * wpy[j] / g) / dpy
                      - (wmy[j] - (vy - amy) * wmy[j] / g) / dmy;

            grad[idx].x += (float)gx;
            grad[idx].y += (float)gy;
        }
    }
}

// ----------------------------------------------------------------
// 高精度单网线长：可选覆盖第 ovIdx 个引脚的绝对坐标（供有限差分用）
// ----------------------------------------------------------------
// 与 WaSmooth 唯一区别：坐标来源是 double 参数，不经过 float 的中心存储，
// 因此扰动可以足够小，差分不会被 float 舍入噪声淹没。
static double WaForNetDiff(const vector<Pin *> &pins, bool isX, float gamma,
                           int ovIdx, double ovCoord)
{
    size_t np = pins.size();
    if (np < 2) return 0.0;

    double maxv = -DBL_MAX, minv = DBL_MAX;
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = (i == (size_t)ovIdx)
                       ? ovCoord
                       : (isX ? (p->module->center.x + p->offset.x)
                              : (p->module->center.y + p->offset.y));
        if (v > maxv) maxv = v;
        if (v < minv) minv = v;
    }
    if (maxv - minv < 1e-9) return 0.0;

    double aPlus = maxv, aMinus = minv, denP = 0, denM = 0;
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = (i == (size_t)ovIdx)
                       ? ovCoord
                       : (isX ? (p->module->center.x + p->offset.x)
                              : (p->module->center.y + p->offset.y));
        double wP = exp((v - maxv) / gamma);
        double wM = exp((minv - v) / gamma);
        denP += wP;
        denM += wM;
    }
    for (size_t i = 0; i < np; i++)
    {
        Pin *p = pins[i];
        double v = (i == (size_t)ovIdx)
                       ? ovCoord
                       : (isX ? (p->module->center.x + p->offset.x)
                              : (p->module->center.y + p->offset.y));
        double wP = exp((v - maxv) / gamma);
        double wM = exp((minv - v) / gamma);
        aPlus  += (v - maxv) * wP / denP;
        aMinus -= (minv - v) * wM / denM;  // 注意是"减"（与 WaSmooth 一致）
    }
    return aPlus - aMinus;
}

// ----------------------------------------------------------------
// 自检：验证 WA 模型实现是否正确
// ----------------------------------------------------------------
// ① 平滑线长 W(γ) 应 ≥ HPWL，且 γ 越小越逼近 HPWL；
// ② 解析梯度 应 ≈ 中心差分得到的数值梯度。
//    注意：差分在"单条网络的单个引脚坐标"上做（纯 double 路径），
//    因为整体 W 的量级是 1e8，float 中心存储的舍入噪声（~1）远大于
//    真实差分信号（~1e-2），直接对 center 做差分会被噪声淹没。
void GlobalPlacer::TestWirelengthModel()
{
    cout << "\n===== 任务5-A 自检：可导线长模型（WA） =====" << endl;
    float hpwl = CalcHPWL();
    cout << "HPWL 基准 = " << hpwl << endl;

    // ① 不同 γ 下平滑线长与 HPWL 的关系
    float gs[] = {800.f, 400.f, 200.f, 100.f, 50.f, 20.f};
    for (size_t k = 0; k < sizeof(gs) / sizeof(gs[0]); k++)
    {
        smoothGamma = gs[k];
        double w = CalcSmoothWirelength();
        printf("  γ=%7.1f  W=%.6e  超HPWL比例=%+.4f%%\n",
               (double)smoothGamma, w, (w / hpwl - 1.0) * 100.0);
    }

    // ② 解析梯度 vs 有限差分（对单条网络的单个引脚坐标做高精度差分）
    smoothGamma = 200.0f;
    int pass = 0, fail = 0;
    double maxRelErr = 0;
    srand(20260831);
    int tries = 0;
    while (pass + fail < 20 && tries < 2000)
    {
        tries++;
        Net *net = db->Nets[rand() % (int)db->Nets.size()];
        const vector<Pin *> &pins = net->netPins;
        size_t np = pins.size();
        if (np < 2) continue;

        int j = rand() % (int)np;
        Module *m = pins[j]->module;
        if (m == NULL || m->isFixed) continue;

        // 只验证"该单元在此网络内恰好只有 1 个引脚"的情况，
        // 这样解析梯度 = 单引脚的贡献，与单引脚差分严格对应
        int cnt = 0;
        for (size_t k = 0; k < np; k++)
            if (pins[k]->module == m) cnt++;
        if (cnt != 1) continue;

        // 解析梯度（完整公式，复用 WaSmooth 的权重与 A± 输出）
        vector<double> wpx, wmx, wpy, wmy;
        double dpx, dmx, dpy, dmy, apx, amx, apy, amy;
        WaSmooth(pins, true,  smoothGamma, wpx, wmx, dpx, dmx, apx, amx);
        WaSmooth(pins, false, smoothGamma, wpy, wmy, dpy, dmy, apy, amy);
        double vx = (double)m->center.x + pins[j]->offset.x;
        double vy = (double)m->center.y + pins[j]->offset.y;
        double g = (double)smoothGamma;
        double gx = (wpx[j] + (vx - apx) * wpx[j] / g) / dpx
                  - (wmx[j] - (vx - amx) * wmx[j] / g) / dmx;
        double gy = (wpy[j] + (vy - apy) * wpy[j] / g) / dpy
                  - (wmy[j] - (vy - amy) * wmy[j] / g) / dmy;

        // 数值梯度（中心差分，纯 double 路径）
        const double eps = 1e-3;
        double numX = (WaForNetDiff(pins, true,  smoothGamma, j, vx + eps)
                     - WaForNetDiff(pins, true,  smoothGamma, j, vx - eps)) / (2.0 * eps);
        double numY = (WaForNetDiff(pins, false, smoothGamma, j, vy + eps)
                     - WaForNetDiff(pins, false, smoothGamma, j, vy - eps)) / (2.0 * eps);

        double relX = fabs(gx - numX) / max(1e-9, fabs(numX));
        double relY = fabs(gy - numY) / max(1e-9, fabs(numY));
        maxRelErr = max(maxRelErr, max(relX, relY));
        bool ok = (relX < 1e-2 && relY < 1e-2);
        if (ok) pass++;
        else
        {
            fail++;
            if (fail <= 3)
                printf("  差分校验失败: 单元=%s 解析=(%.6f,%.6f) 数值=(%.6f,%.6f) net=%s\n",
                       m->name.c_str(), gx, gy, numX, numY, net->name.c_str());
        }
    }
    printf("梯度有限差分校验: 通过 %d/20，最大相对误差 = %.6f%%\n", pass, maxRelErr * 100);
    cout << "===== 任务5-A 自检结束 =====\n" << endl;
}

// ================================================================
// 任务5 全局布局 —— 步骤 B：密度建模（eDensity）
// ================================================================

// ----------------------------------------------------------------
// 网格划分：m×m（论文要求 2 的幂），并确定 bin 尺寸与目标密度
// ----------------------------------------------------------------
void GlobalPlacer::InitGrid()
{
    double W = coreUR.x - coreLL.x;
    double H = coreUR.y - coreLL.y;

    // 网格维度取 2 的幂。adaptec1 核心区约 10700×10700，
    // m=128 时 bin 宽约 84，能分辨单元级拥挤；越大越精细但 DCT 越慢。
    // m=64 时 bin 宽约 167，速度更快但精度略低（实验显示 m=64 效果更好）。
    gridM = 128; // 默认值，可调优
    binW = W / gridM;
    binH = H / gridM;
    binArea = binW * binH;
    targetDensity = 0.8; // 目标密度 τ：填充后单元平均占核心区 80%

    nodeDensity.assign(gridM, vector<double>(gridM, 0.0));
    fillerDensity.assign(gridM, vector<double>(gridM, 0.0));
    terminalDensity.assign(gridM, vector<double>(gridM, 0.0));
}

// ----------------------------------------------------------------
// Filler 填充（论文式13 + §3.2）
//   filler 总面积 A_fc = τ·A_ws − A_m
//     A_m  = 可移动单元总面积
//     A_ws = 空白面积 = 核心区面积 − 固定单元面积
//   filler 尺寸 = 中间 80% 可移动单元的平均面积（剔除最大/最小 10% 噪声）
//   filler 均匀撒入核心区（每格中心 + 小抖动），最后从布局中移除
// ----------------------------------------------------------------
void GlobalPlacer::InsertFiller()
{
    // ① 可移动单元总面积
    double Am = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++)
        Am += db->Nodes[i]->area;

    // ② 空白面积 = 核心区面积 − 固定单元面积
    double W = coreUR.x - coreLL.x, H = coreUR.y - coreLL.y;
    double coreArea = W * H;
    double Af = 0;
    for (size_t i = 0; i < db->Terminals.size(); i++)
        Af += db->Terminals[i]->area;
    double A_ws = coreArea - Af;

    // ③ filler 总面积（式13）
    double A_fc = targetDensity * A_ws - Am;
    if (A_fc < 0) A_fc = 0;

    // ④ filler 尺寸 = 中间 80% 可移动单元的平均面积
    vector<double> areas;
    areas.reserve(db->Nodes.size());
    for (size_t i = 0; i < db->Nodes.size(); i++)
        areas.push_back(db->Nodes[i]->area);
    sort(areas.begin(), areas.end());
    int lo = (int)(areas.size() * 0.1);
    int hi = (int)(areas.size() * 0.9);
    if (hi <= lo) hi = lo + 1;
    double sum = 0;
    for (int k = lo; k < hi; k++) sum += areas[k];
    double fillerSize = sum / (hi - lo);

    // ⑤ filler 数量
    int Nfc = (int)(A_fc / fillerSize);
    if (Nfc < 0) Nfc = 0;

    // ⑥ 均匀撒入核心区：按长宽比切成 cols×rows 格，每格中心 + 小抖动
    fillers.clear();
    fillers.reserve(Nfc);
    int cols = (int)sqrt(Nfc * W / H);
    if (cols < 1) cols = 1;
    int rows = Nfc / cols;
    if (rows < 1) rows = 1;
    while (cols * rows < Nfc) rows++;
    double cellW = W / cols, cellH = H / rows;
    srand(20260901);
    int placed = 0;
    for (int r = 0; r < rows && placed < Nfc; r++)
    {
        for (int c = 0; c < cols && placed < Nfc; c++)
        {
            FillerCell f;
            double jitter = 0.3;
            f.x = coreLL.x + cellW * (c + 0.5) + ((rand() / (double)RAND_MAX) - 0.5) * jitter * cellW;
            f.y = coreLL.y + cellH * (r + 0.5) + ((rand() / (double)RAND_MAX) - 0.5) * jitter * cellH;
            f.x = max((double)coreLL.x, min((double)coreUR.x, f.x));
            f.y = max((double)coreLL.y, min((double)coreUR.y, f.y));
            f.area = fillerSize;
            fillers.push_back(f);
            placed++;
        }
    }

    printf("[密度] 可移动面积 Am=%.1f  空白 A_ws=%.1f  filler总面积 A_fc=%.1f\n",
           Am, A_ws, A_fc);
    printf("[密度] filler尺寸=%.2f  数量=%d  目标密度τ=%.2f\n",
           fillerSize, Nfc, targetDensity);
}

// ----------------------------------------------------------------
// 面积分布：把 cell / filler / terminal 按重叠面积分摊到 bin 网格
// ----------------------------------------------------------------
// 密度单位 = 面积 / binArea（相当于"这个 bin 被占了几个格子的面积"）。
// 可移动单元不缩放；filler 不缩放；固定单元面积 ×τ（密度缩放，论文§3.2）。
// ----------------------------------------------------------------
void GlobalPlacer::DistributeDensity()
{
    for (int i = 0; i < gridM; i++)
    {
        fill(nodeDensity[i].begin(), nodeDensity[i].end(), 0.0);
        fill(fillerDensity[i].begin(), fillerDensity[i].end(), 0.0);
        fill(terminalDensity[i].begin(), terminalDensity[i].end(), 0.0);
    }

    // 内部工具：把某个矩形 [x0,x1]×[y0,y1] 按重叠面积分摊到某个密度场
    // lambda 避免重复代码（C++14 支持）
    const double fieldRight = coreLL.x + gridM * binW;
    const double fieldTop   = coreLL.y + gridM * binH;
    auto scatter = [&](double x0, double y0, double x1, double y1,
                       vector<vector<double>> &field, double scale)
    {
        int i0 = (int)floor((x0 - coreLL.x) / binW);
        int i1 = (int)floor((x1 - coreLL.x) / binW);
        int j0 = (int)floor((y0 - coreLL.y) / binH);
        int j1 = (int)floor((y1 - coreLL.y) / binH);

        // 【性能优化】快速路径：矩形整个落在核心区内、且完全含于单个 bin。
        // 标准单元（约 27×12）与 filler（边长约 17）都远小于 bin（约 84），
        // 绝大多数对象命中这条分支，可跳过边界 clamp、嵌套循环和逐 bin 求交，
        // 重叠面积直接等于矩形面积。算术表达式与原路径保持一致，结果不变。
        if (i0 == i1 && j0 == j1 &&
            x0 >= coreLL.x && x1 <= fieldRight &&
            y0 >= coreLL.y && y1 <= fieldTop)
        {
            field[i0][j0] += scale * (x1 - x0) * (y1 - y0) / binArea;
            return;
        }

        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 >= gridM) i1 = gridM - 1;
        if (j1 >= gridM) j1 = gridM - 1;
        for (int i = i0; i <= i1; i++)
        {
            double bx0 = coreLL.x + i * binW, bx1 = bx0 + binW;
            for (int j = j0; j <= j1; j++)
            {
                double by0 = coreLL.y + j * binH, by1 = by0 + binH;
                double ovX = min(x1, bx1) - max(x0, bx0);
                double ovY = min(y1, by1) - max(y0, by0);
                if (ovX > 0 && ovY > 0)
                    field[i][j] += scale * ovX * ovY / binArea;
            }
        }
    };

    // ① 可移动单元（不缩放）
    for (size_t k = 0; k < db->Nodes.size(); k++)
    {
        Module *m = db->Nodes[k];
        scatter(m->center.x - m->width / 2, m->center.y - m->height / 2,
                m->center.x + m->width / 2, m->center.y + m->height / 2,
                nodeDensity, 1.0);
    }

    // ② filler（不缩放，视为正方形，边长 sqrt(area)）
    for (size_t k = 0; k < fillers.size(); k++)
    {
        FillerCell &f = fillers[k];
        double s = sqrt(f.area) / 2;
        scatter(f.x - s, f.y - s, f.x + s, f.y + s, fillerDensity, 1.0);
    }

    // ③ 固定单元（面积 ×τ 密度缩放）
    for (size_t k = 0; k < db->Terminals.size(); k++)
    {
        Module *t = db->Terminals[k];
        scatter(t->center.x - t->width / 2, t->center.y - t->height / 2,
                t->center.x + t->width / 2, t->center.y + t->height / 2,
                terminalDensity, targetDensity);
    }
}

// ----------------------------------------------------------------
// 密度溢出率 τ（论文式37）：可移动单元超出目标密度的总"超额面积"
// 占全部可移动面积的比。≈0 表示单元分布均匀，是收敛判据。
// ----------------------------------------------------------------
double GlobalPlacer::CalcDensityOverflow()
{
    double Am = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++)
        Am += db->Nodes[i]->area;
    if (Am <= 0) return 0;

    double excess = 0;
    for (int i = 0; i < gridM; i++)
        for (int j = 0; j < gridM; j++)
            if (nodeDensity[i][j] > targetDensity)
                excess += (nodeDensity[i][j] - targetDensity) * binArea;
    return excess / Am;
}

// ----------------------------------------------------------------
// 密度网格概况（步骤B 自检）：守恒校验 + 统计 + ASCII 热力图
// ----------------------------------------------------------------
void GlobalPlacer::ReportDensityGrid()
{
    // 守恒校验：各场面积和应分别 ≈ Am / A_fc / τ·Af
    double sumNode = 0, sumFiller = 0, sumTerm = 0;
    double maxNode = 0;
    int overfull = 0;
    for (int i = 0; i < gridM; i++)
    {
        for (int j = 0; j < gridM; j++)
        {
            sumNode += nodeDensity[i][j] * binArea;
            sumFiller += fillerDensity[i][j] * binArea;
            sumTerm += terminalDensity[i][j] * binArea;
            if (nodeDensity[i][j] > maxNode) maxNode = nodeDensity[i][j];
            if (nodeDensity[i][j] > targetDensity) overfull++;
        }
    }
    double Am = 0, AfCore = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++) Am += db->Nodes[i]->area;
    // 固定面积只统计"落在核心区内"的部分（与 scatter 的范围一致，便于守恒对比）
    for (size_t i = 0; i < db->Terminals.size(); i++)
    {
        Module *t = db->Terminals[i];
        double x0 = max((double)t->center.x - t->width / 2, (double)coreLL.x);
        double x1 = min((double)t->center.x + t->width / 2, (double)coreUR.x);
        double y0 = max((double)t->center.y - t->height / 2, (double)coreLL.y);
        double y1 = min((double)t->center.y + t->height / 2, (double)coreUR.y);
        if (x1 > x0 && y1 > y0) AfCore += (x1 - x0) * (y1 - y0);
    }
    double sumFillerExpected = 0;
    for (size_t i = 0; i < fillers.size(); i++) sumFillerExpected += fillers[i].area;

    double overflow = CalcDensityOverflow();
    printf("\n===== 任务5-B 密度网格概况（m=%d） =====\n", gridM);
    printf("  可移动面积: 分布=%.1f  理论=%.1f  误差=%.3f%%\n",
           sumNode, Am, fabs(sumNode - Am) / max(1.0, Am) * 100);
    printf("  filler面积 : 分布=%.1f  理论=%.1f  误差=%.3f%%\n",
           sumFiller, sumFillerExpected,
           fabs(sumFiller - sumFillerExpected) / max(1.0, sumFillerExpected) * 100);
    printf("  固定面积×τ: 分布=%.1f  理论=%.1f\n", sumTerm, targetDensity * AfCore);
    printf("  平均可移动密度=%.3f  峰值=%.3f  过满bin(%d/%d)\n",
           sumNode / (gridM * gridM * binArea), maxNode, overfull, gridM * gridM);
    printf("  密度溢出率 τ = %.3f%%\n", overflow * 100);

    // ASCII 热力图（可移动密度，缩到 48×24，字符分级）
    printf("  可移动密度热力图（'.'=稀疏  'o'=中  '+'=偏密  'X'=过密  '#'=严重过密）：\n");
    int gw = 48, gh = 24;
    for (int r = 0; r < gh; r++)
    {
        printf("    ");
        for (int c = 0; c < gw; c++)
        {
            int i = (int)((c + 0.5) / gw * gridM);
            int j = (int)((r + 0.5) / gh * gridM);
            double d = nodeDensity[i][j] / max(1.0, targetDensity); // 相对目标密度
            char ch;
            if (d < 0.15) ch = '.';
            else if (d < 0.45) ch = 'o';
            else if (d < 0.9) ch = '+';
            else if (d < 1.5) ch = 'X';
            else ch = '#';
            printf("%c", ch);
        }
        printf("\n");
    }
    printf("===== 任务5-B 结束 =====\n");
}

// ================================================================
// 任务5 全局布局 —— 步骤 C：静电求解（DCT 泊松方程）
// ================================================================

// ----------------------------------------------------------------
// 1D DCT-II（正交归一化形式）
// ----------------------------------------------------------------
// 正变换（forward）：out[u] = c(u)·Σ_i in[i]·cos(π·u·(i+0.5)/m)
// 逆变换（inverse）：out[i] = Σ_u c(u)·in[u]·cos(π·u·(i+0.5)/m)
//   c(0) = sqrt(1/m)，c(u≠0) = sqrt(2/m)
//
// 【性能优化】余弦表预计算
//   原实现在二重循环最内层直接调用 cos()：一次 2D 变换 = 2m³ 次超越函数
//   （m=128 时约 4.2×10⁶ 次），而 SolvePoisson 每轮要做正、逆两次变换，
//   全局布局 500 轮上限下累计上亿次 cos 调用，是最大热点。
//   这里把 c(u)·cos(π·u·(i+0.5)/m) 预计算成两张 m×m 表，均按“输出下标”
//   分行存储，使内积变成连续内存的乘加；数值上与原来等价（仅浮点结合
//   顺序不同，误差在 1e-15 量级，不影响 τ 收敛判据）。
//   表只与网格尺寸 m 有关，全局缓存一次，后续所有轮次直接复用。
// ----------------------------------------------------------------
static int g_cosTabM = -1;          // 当前表对应的网格尺寸
static vector<double> g_cosFwd;     // 正变换表：g_cosFwd[u*m + i]
static vector<double> g_cosInv;     // 逆变换表：g_cosInv[i*m + u]（转置存储）

static void BuildCosTable(int m)
{
    if (g_cosTabM == m && (int)g_cosFwd.size() == m * m) return;

    const double pi = 3.14159265358979323846;
    g_cosFwd.resize((size_t)m * m);
    g_cosInv.resize((size_t)m * m);
    for (int u = 0; u < m; u++)
    {
        const double c = (u == 0) ? sqrt(1.0 / m) : sqrt(2.0 / m);
        for (int i = 0; i < m; i++)
        {
            const double v = c * cos(pi * u * (i + 0.5) / m);
            g_cosFwd[(size_t)u * m + i] = v; // 正变换：按 u 分行
            g_cosInv[(size_t)i * m + u] = v; // 逆变换：按 i 分行（等价转置）
        }
    }
    g_cosTabM = m;
}

static void DCT1(const vector<double> &in, vector<double> &out, int m, bool forward)
{
    BuildCosTable(m);
    out.resize(m);

    const vector<double> &tab = forward ? g_cosFwd : g_cosInv;
    if (forward)
    {
        // out[u] = Σ_i in[i]·T[u][i]，行连续访问
        for (int u = 0; u < m; u++)
        {
            const double *row = &tab[(size_t)u * m];
            double s = 0.0;
            for (int i = 0; i < m; i++) s += in[i] * row[i];
            out[u] = s;
        }
    }
    else
    {
        // out[i] = Σ_u in[u]·T[i][u]，行连续访问
        for (int i = 0; i < m; i++)
        {
            const double *row = &tab[(size_t)i * m];
            double s = 0.0;
            for (int u = 0; u < m; u++) s += in[u] * row[u];
            out[i] = s;
        }
    }
}

// ----------------------------------------------------------------
// 2D DCT-II：行列分离（先对每行做 1D，再对每列做 1D）
// forward=true 正变换；false 逆变换。
// 【性能优化】中间缓冲改为静态复用，且 out 尺寸相符时不再重新分配/清零：
//   原实现每次调用都新建 m×m 的 tmp 与 4 个 m 长临时向量，每轮全局布局
//   调用两次，500 轮累计产生上千次堆分配；现在只分配一次。
// ----------------------------------------------------------------
static void DCT2(const vector<vector<double>> &in, vector<vector<double>> &out, int m, bool forward)
{
    static vector<vector<double>> tmp;
    static vector<double> rowIn, rowOut, colIn, colOut;

    if ((int)tmp.size() != m)
    {
        tmp.assign(m, vector<double>(m, 0.0));
        rowIn.resize(m);
        rowOut.resize(m);
        colIn.resize(m);
        colOut.resize(m);
    }

    // 行变换（正变换：输出 u 列；逆变换：输出 i 行）
    for (int i = 0; i < m; i++)
    {
        const vector<double> &src = in[i];
        for (int j = 0; j < m; j++) rowIn[j] = src[j];
        DCT1(rowIn, rowOut, m, forward);
        vector<double> &dst = tmp[i];
        for (int k = 0; k < m; k++) dst[k] = rowOut[k];
    }

    // 列变换：直接写入 out（尺寸不符时才分配；下方会覆盖全部元素，无需清零）
    if ((int)out.size() != m) out.assign(m, vector<double>(m, 0.0));
    for (int k = 0; k < m; k++)
    {
        for (int i = 0; i < m; i++) colIn[i] = tmp[i][k];
        DCT1(colIn, colOut, m, forward);
        vector<double> &dst = out[k];
        for (int j = 0; j < m; j++) dst[j] = colOut[j];
    }
}

// ----------------------------------------------------------------
// 双线性插值：取网格点 (x,y) 处的场值
// 网格点 i 对应物理 x = coreLL.x + (i+0.5)·binW（bin 中心采样）
// ----------------------------------------------------------------
static double BilerpGrid(const vector<vector<double>> &g, double x, double y,
                         const POS_2D &ll, double binW, double binH, int m)
{
    double fx = (x - ll.x) / binW - 0.5;
    double fy = (y - ll.y) / binH - 0.5;
    fx = max(0.0, min((double)(m - 1), fx));
    fy = max(0.0, min((double)(m - 1), fy));
    int i0 = (int)floor(fx), j0 = (int)floor(fy);
    int i1 = min(i0 + 1, m - 1), j1 = min(j0 + 1, m - 1);
    double tx = fx - i0, ty = fy - j0;
    return (1 - tx) * (1 - ty) * g[i0][j0] + tx * (1 - ty) * g[i1][j0]
         + (1 - tx) * ty * g[i0][j1] + tx * ty * g[i1][j1];
}

// ----------------------------------------------------------------
// 泊松方程求解（论文式17-23）
//   ∇²ψ = −ρ，Neumann 边界（∂ψ/∂n=0），∫ψ = ∫ρ = 0
// 数值解：
//   ① 总密度 ρ = node + filler + terminal，先减均值使 ∫ρ=0
//   ② DCT → 系数 a_{u,v}
//   ③ 频域解 ψ̂_{u,v} = a_{u,v} / (w_u² + w_v²)，直流项(u=v=0)置 0
//      w_u² + w_v² 用离散拉普拉斯本征值：
//        4·sin²(πu/2m)/binW² + 4·sin²(πv/2m)/binH²
//   ④ 逆 DCT → 电势网格
// ----------------------------------------------------------------
void GlobalPlacer::SolvePoisson()
{
    // 总密度场
    vector<vector<double>> rho(gridM, vector<double>(gridM, 0.0));
    for (int i = 0; i < gridM; i++)
        for (int j = 0; j < gridM; j++)
            rho[i][j] = nodeDensity[i][j] + fillerDensity[i][j] + terminalDensity[i][j];

    // 减均值 → ∫ρ = 0（式19），直流项才没有定义矛盾
    double mean = 0;
    for (int i = 0; i < gridM; i++)
        for (int j = 0; j < gridM; j++)
            mean += rho[i][j];
    mean /= (gridM * gridM);
    for (int i = 0; i < gridM; i++)
        for (int j = 0; j < gridM; j++)
            rho[i][j] -= mean;

    // DCT → 频域系数
    vector<vector<double>> coeff;
    DCT2(rho, coeff, gridM, true);

    // 频域解（式23 的离散形式）
    const double pi = 3.14159265358979323846;
    for (int u = 0; u < gridM; u++)
    {
        for (int v = 0; v < gridM; v++)
        {
            if (u == 0 && v == 0) { coeff[u][v] = 0; continue; } // 直流项（积分=0）
            double su = sin(pi * u / (2.0 * gridM));
            double sv = sin(pi * v / (2.0 * gridM));
            double w2 = 4.0 * su * su / (binW * binW) + 4.0 * sv * sv / (binH * binH);
            coeff[u][v] /= w2; // ψ̂ = ρ̂ / (w_u²+w_v²)
        }
    }

    // 逆 DCT → 电势网格
    DCT2(coeff, potential, gridM, false);
}

// ----------------------------------------------------------------
// 电场：E = −∇ψ，网格中心差分（边界用单侧差分，对应 Neumann 边界）
// ----------------------------------------------------------------
void GlobalPlacer::ComputeElectricField()
{
    fieldX.assign(gridM, vector<double>(gridM, 0.0));
    fieldY.assign(gridM, vector<double>(gridM, 0.0));
    for (int i = 0; i < gridM; i++)
    {
        for (int j = 0; j < gridM; j++)
        {
            int im = max(i - 1, 0), ip = min(i + 1, gridM - 1);
            int jm = max(j - 1, 0), jp = min(j + 1, gridM - 1);
            double dpsiX = (potential[ip][j] - potential[im][j]) / (binW * (ip - im));
            double dpsiY = (potential[i][jp] - potential[i][jm]) / (binH * (jp - jm));
            fieldX[i][j] = -dpsiX; // E = −∇ψ
            fieldY[i][j] = -dpsiY;
        }
    }
}

// ----------------------------------------------------------------
// 势能 N = ½ Σ q·ψ（密度惩罚，越小表示分布越均匀）
// ----------------------------------------------------------------
double GlobalPlacer::CalcPotentialEnergy()
{
    double N = 0;
    // 可移动单元
    for (size_t k = 0; k < db->Nodes.size(); k++)
    {
        Module *m = db->Nodes[k];
        double psi = BilerpGrid(potential, m->center.x, m->center.y,
                                coreLL, binW, binH, gridM);
        N += 0.5 * m->area * psi;
    }
    // filler
    for (size_t k = 0; k < fillers.size(); k++)
    {
        FillerCell &f = fillers[k];
        double psi = BilerpGrid(potential, f.x, f.y, coreLL, binW, binH, gridM);
        N += 0.5 * f.area * psi;
    }
    return N;
}

// ----------------------------------------------------------------
// 密度梯度（Lorentz 力，论文式16）：∇N_i = −q_i·E(x_i, y_i)
//   grad[k] 对应 db->Nodes[k]（可移动单元）
//   fillerGrad[k] 对应 fillers[k]（filler 也要参与迭代移动）
// 返回势能 N（顺带算出来，供报告）
// ----------------------------------------------------------------
double GlobalPlacer::CalcDensityGradient(vector<POS_2D> &grad)
{
    const int N = (int)db->Nodes.size();
    grad.assign(N, POS_2D());
    fillerGrad.assign(fillers.size(), POS_2D());

    // 【性能优化】电势与电场插值合并到一次遍历。
    // 原实现先遍历一遍算电场梯度，末尾再调 CalcPotentialEnergy() 遍历第二遍
    // 去求电势，同一批 45 万个对象要走两趟、且两趟都重复做同样的坐标换算。
    // 合并后每对象只算一次插值定位，遍历次数减半，数值完全不变
    // （三个 BilerpGrid 都是同一坐标上的独立采样，累加顺序也与原来一致）。
    double potentialEnergy = 0.0;

    // 可移动单元
    for (int k = 0; k < N; k++)
    {
        Module *m = db->Nodes[k];
        const double cx = m->center.x, cy = m->center.y;
        const double ex = BilerpGrid(fieldX, cx, cy, coreLL, binW, binH, gridM);
        const double ey = BilerpGrid(fieldY, cx, cy, coreLL, binW, binH, gridM);
        const double psi = BilerpGrid(potential, cx, cy, coreLL, binW, binH, gridM);
        grad[k].x = (float)(-m->area * ex);
        grad[k].y = (float)(-m->area * ey);
        potentialEnergy += 0.5 * m->area * psi;
    }

    // filler
    for (size_t k = 0; k < fillers.size(); k++)
    {
        FillerCell &f = fillers[k];
        const double fx = f.x, fy = f.y;
        const double ex = BilerpGrid(fieldX, fx, fy, coreLL, binW, binH, gridM);
        const double ey = BilerpGrid(fieldY, fx, fy, coreLL, binW, binH, gridM);
        const double psi = BilerpGrid(potential, fx, fy, coreLL, binW, binH, gridM);
        fillerGrad[k].x = (float)(-f.area * ex);
        fillerGrad[k].y = (float)(-f.area * ey);
        potentialEnergy += 0.5 * f.area * psi;
    }

    return potentialEnergy;
}

// ----------------------------------------------------------------
// 自检：验证静电求解是否正确
// ----------------------------------------------------------------
// ① DCT 正交性：随机密度 → 变换 → 逆变换，应还原（误差 ~1e-12）
// ② 泊松残差：对解出的 ψ 重新算 ∇²ψ，应 ≈ −ρ
// ③ 单点电荷：中心放一个高密度 bin，电势中心最高、向四周递减；
//    cell 密度梯度应把它"推出"高密度区（梯度指向低密度方向）
void GlobalPlacer::TestDensityModel()
{
    cout << "\n===== 任务5-C 自检：静电求解（DCT 泊松方程） =====" << endl;

    // ① DCT 正交性：随机场还原
    {
        vector<vector<double>> in(gridM, vector<double>(gridM, 0.0));
        srand(7);
        for (int i = 0; i < gridM; i++)
            for (int j = 0; j < gridM; j++)
                in[i][j] = rand() / (double)RAND_MAX;
        vector<vector<double>> coef, back;
        DCT2(in, coef, gridM, true);
        DCT2(coef, back, gridM, false); // 正变换后逆变换，应还原
        double maxErr = 0;
        for (int i = 0; i < gridM; i++)
            for (int j = 0; j < gridM; j++)
                maxErr = max(maxErr, fabs(back[i][j] - in[i][j]));
        printf("① DCT 正交还原：最大误差 = %.3e%s\n", maxErr,
               maxErr < 1e-10 ? "  ✓" : "  ✗");
    }

    // ② 泊松残差：∇²ψ 应 ≈ −ρ（ρ 需与 SolvePoisson 一致，先减均值使 ∫ρ=0）
    {
        vector<vector<double>> rho(gridM, vector<double>(gridM, 0.0));
        double mean = 0;
        for (int i = 0; i < gridM; i++)
            for (int j = 0; j < gridM; j++)
            {
                rho[i][j] = nodeDensity[i][j] + fillerDensity[i][j] + terminalDensity[i][j];
                mean += rho[i][j];
            }
        mean /= (gridM * gridM);
        for (int i = 0; i < gridM; i++)
            for (int j = 0; j < gridM; j++)
                rho[i][j] -= mean;
        SolvePoisson();
        // 网格拉普拉斯（Neumann 边界用对称 ghost：ψ[-1]=ψ[1]）
        double maxRes = 0, sumRes = 0;
        int mi = 0, mj = 0;
        for (int i = 0; i < gridM; i++)
        {
            for (int j = 0; j < gridM; j++)
            {
                int im = max(i - 1, 0), ip = min(i + 1, gridM - 1);
                int jm = max(j - 1, 0), jp = min(j + 1, gridM - 1);
                // 中心差分拉普拉斯（边界处退化为对称单侧，对应 Neumann）
                double lxx = 0, lyy = 0;
                if (im == ip)      lxx = (potential[ip][j] - potential[i][j]) / (binW * binW);
                else if (i == 0)   lxx = (2 * potential[1][j] - 2 * potential[0][j]) / (binW * binW);
                else if (i == gridM - 1) lxx = (2 * potential[gridM - 2][j] - 2 * potential[gridM - 1][j]) / (binW * binW);
                else               lxx = (potential[i + 1][j] - 2 * potential[i][j] + potential[i - 1][j]) / (binW * binW);
                if (jm == jp)      lyy = (potential[i][jp] - potential[i][j]) / (binH * binH);
                else if (j == 0)   lyy = (2 * potential[i][1] - 2 * potential[i][0]) / (binH * binH);
                else if (j == gridM - 1) lyy = (2 * potential[i][gridM - 2] - 2 * potential[i][gridM - 1]) / (binH * binH);
                else               lyy = (potential[i][j + 1] - 2 * potential[i][j] + potential[i][j - 1]) / (binH * binH);

                double res = (lxx + lyy) - (-rho[i][j]); // 应 ≈ 0
                if (fabs(res) > maxRes) { maxRes = fabs(res); mi = i; mj = j; }
                sumRes += fabs(res);
            }
        }
        double avgRes = sumRes / (gridM * gridM);
        // 检查最大残差位置的密度是否"跳变"（单元边缘 → Gibbs 现象）
        double d0 = rho[mi][mj];
        double dNbr = (fabs(rho[mi][min(mj + 1, gridM - 1)]) + fabs(rho[mi][max(mj - 1, 0)]) +
                       fabs(rho[min(mi + 1, gridM - 1)][mj]) + fabs(rho[max(mi - 1, 0)][mj])) / 4.0;
        printf("② 泊松残差：平均=%.6e  最大=%.6e\n", avgRes, maxRes);
        printf("   最大残差在 bin(%d,%d) 密度=%.3f 相邻平均=%.3f %s\n",
               mi, mj, d0, dNbr,
               (fabs(d0 - dNbr) > 0.3) ? "← 密度突变处（Gibbs 现象，可接受）" : "← 需检查");
    }

    // ③ 单点电荷：在网格中心放一个高密度，验证电势形态与电场方向
    {
        vector<vector<double>> rho(gridM, vector<double>(gridM, 0.0));
        int c = gridM / 2;
        rho[c][c] = 10.0; // 单点高密度（忽略均值减除，直流无关形态）
        vector<vector<double>> coef;
        DCT2(rho, coef, gridM, true); // 正变换 → 系数
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < gridM; u++)
            for (int v = 0; v < gridM; v++)
            {
                if (u == 0 && v == 0) { coef[u][v] = 0; continue; }
                double su = sin(pi * u / (2.0 * gridM));
                double sv = sin(pi * v / (2.0 * gridM));
                coef[u][v] /= 4.0 * su * su / (binW * binW) + 4.0 * sv * sv / (binH * binH);
            }
        DCT2(coef, rho, gridM, false); // 逆变换 → 电势
        double psiCenter = rho[c][c];
        double psiEdge   = rho[0][0];
        printf("③ 单点电荷：中心电势=%.4e  角落电势=%.4e%s\n",
               psiCenter, psiEdge,
               (psiCenter > psiEdge && psiCenter > 0) ? "  ✓（中心高、向外衰减）" : "  ✗");
    }

    // ④ 真实密度场的电场与密度梯度统计
    {
        DistributeDensity();
        SolvePoisson();
        ComputeElectricField();
        vector<POS_2D> grad;
        double N = CalcDensityGradient(grad);

        // 电场幅值统计
        double maxE = 0, avgE = 0;
        for (int i = 0; i < gridM; i++)
            for (int j = 0; j < gridM; j++)
            {
                double e = sqrt(fieldX[i][j] * fieldX[i][j] + fieldY[i][j] * fieldY[i][j]);
                maxE = max(maxE, e);
                avgE += e;
            }
        avgE /= (gridM * gridM);
        // 密度梯度幅值统计
        double maxG = 0, avgG = 0;
        int Nn = (int)db->Nodes.size();
        for (int k = 0; k < Nn; k++)
        {
            double g = sqrt((double)grad[k].x * grad[k].x + (double)grad[k].y * grad[k].y);
            maxG = max(maxG, g);
            avgG += g;
        }
        avgG /= Nn;
        double overflow = CalcDensityOverflow();
        printf("④ 真实密度场：势能 N=%.6e  平均|E|=%.4e  最大|E|=%.4e\n", N, avgE, maxE);
        printf("   密度梯度   ：平均|∇N|=%.4e  最大|∇N|=%.4e  溢出率τ=%.3f%%\n",
               avgG, maxG, overflow * 100);
    }

    cout << "===== 任务5-C 自检结束 =====\n" << endl;
}

// ================================================================
// 任务5 全局布局 —— 步骤 D：Nesterov 加速梯度下降
// ================================================================

// ----------------------------------------------------------------
// 坐标序列 ↔ Module->center 双向同步
// ----------------------------------------------------------------
// 布局坐标 = 可移动单元的 center（x,y）+ filler 的 (x,y)
// Nesterov 的 u/v 是两套"坐标序列"，需要定期同步回 Module 供密度计算。
// 每轮迭代的流程：
//   ① 把 v 序列写到 Module → 用于本轮梯度计算（v 是"探头"）
//   ② 梯度算完后更新 u/v
//   ③ 把 u 序列写回 Module（u 是"主解"，代表本轮真正的布局）

// 把所有可移动单元和 filler 的坐标存进一个 vector
static vector<POS_2D> StoreCoords(PlaceData *db, const vector<GlobalPlacer::FillerCell> &fillers)
{
    vector<POS_2D> r(db->Nodes.size() + fillers.size(), POS_2D());
    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        r[i].x = db->Nodes[i]->center.x;
        r[i].y = db->Nodes[i]->center.y;
    }
    for (size_t i = 0; i < fillers.size(); i++)
    {
        r[db->Nodes.size() + i].x = (float)fillers[i].x;
        r[db->Nodes.size() + i].y = (float)fillers[i].y;
    }
    return r;
}

// 把坐标序列写回 Module->center（同时裁剪到核心区边界）
static void RestoreCoords(PlaceData *db, vector<GlobalPlacer::FillerCell> &fillers,
                          const vector<POS_2D> &coords, const POS_2D &ll, const POS_2D &ur)
{
    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        Module *m = db->Nodes[i];
        m->center.x = max((float)ll.x, min((float)ur.x, coords[i].x));
        m->center.y = max((float)ll.y, min((float)ur.y, coords[i].y));
        m->origin.x = m->center.x - m->width / 2;
        m->origin.y = m->center.y - m->height / 2;
    }
    for (size_t i = 0; i < fillers.size(); i++)
    {
        fillers[i].x = max((double)ll.x, min((double)ur.x, (double)coords[db->Nodes.size() + i].x));
        fillers[i].y = max((double)ll.y, min((double)ur.y, (double)coords[db->Nodes.size() + i].y));
    }
}

// ----------------------------------------------------------------
// λ 初始化（论文式35）：线长力与密度力的比值
// ----------------------------------------------------------------
void GlobalPlacer::InitLambda()
{
    vector<POS_2D> wGrad;
    CalcWirelengthGradient(wGrad);
    vector<POS_2D> dGrad;
    CalcDensityGradient(dGrad);
    double sumW = 0, sumD = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        sumW += fabs(wGrad[i].x) + fabs(wGrad[i].y);
        sumD += fabs(dGrad[i].x) + fabs(dGrad[i].y);
    }
    ns.lambda = (sumD > 1e-6) ? sumW / sumD : 1.0;
    printf("[参数] λ_0 = %.6e  (线长力/密度力 = %.3f/%.3f)\n", ns.lambda, sumW, sumD);
}

// ----------------------------------------------------------------
// γ 更新（论文式38）：γ(τ) = 8·wb·10^(20/9·τ − 11/9)
//   τ=1.0 时 γ=80wb（早迭代，全局平滑）
//   τ=0.1 时 γ=0.8wb（晚迭代，逼近 HPWL）
// ----------------------------------------------------------------
void GlobalPlacer::UpdateGamma()
{
    double wb = (binW + binH) / 2;
    double tau = CalcDensityOverflow();
    double k = 20.0 / 9, b = -11.0 / 9;
    double gamma = 8.0 * wb * pow(10.0, k * tau + b);
    ns.gamma = max(0.8 * wb, min(80.0 * wb, gamma));
}

// ----------------------------------------------------------------
// λ 迭代更新（论文式36）：ρ = 1.1 − ΔHPWL/HPWL_ref + 1.0
//   λ_{k+1} = λ_k × clamp(ρ, 0.75, 1.1)
// ----------------------------------------------------------------
void GlobalPlacer::UpdateLambda(double hpwlDelta)
{
    const double HPWL_REF = 3.5e5;
    double rho = 1.1 - hpwlDelta / HPWL_REF + 1.0;
    rho = max(0.75, min(1.1, rho));
    ns.lambda *= rho;
}

// ----------------------------------------------------------------
// 总梯度 ∇f = ∇W + λ·∇N（在 v_k 处计算）
// ----------------------------------------------------------------
double GlobalPlacer::ComputeGradient(NesterovState &ns)
{
    vector<POS_2D> wGrad, dGrad;
    CalcWirelengthGradient(wGrad);
    double N = CalcDensityGradient(dGrad);
    int Nn = (int)db->Nodes.size();
    ns.fGrad.assign(Nn + (int)fillers.size(), POS_2D());
    for (int i = 0; i < Nn; i++)
    {
        ns.fGrad[i].x = wGrad[i].x + (float)(ns.lambda * dGrad[i].x);
        ns.fGrad[i].y = wGrad[i].y + (float)(ns.lambda * dGrad[i].y);
    }
    for (size_t i = 0; i < fillers.size(); i++)
    {
        ns.fGrad[Nn + i].x = (float)(ns.lambda * fillerGrad[i].x);
        ns.fGrad[Nn + i].y = (float)(ns.lambda * fillerGrad[i].y);
    }
    return N;
}

// ----------------------------------------------------------------
// 位置更新：coords -= step × grad，同时裁剪边界
// ----------------------------------------------------------------
void GlobalPlacer::UpdatePositions(vector<POS_2D> &coords, const vector<POS_2D> &grad, double step)
{
    for (size_t i = 0; i < coords.size(); i++)
    {
        coords[i].x = (float)max((double)coreLL.x,
                       min((double)coreUR.x, coords[i].x - step * grad[i].x));
        coords[i].y = (float)max((double)coreLL.y,
                       min((double)coreUR.y, coords[i].y - step * grad[i].y));
    }
}

// ----------------------------------------------------------------
// 主循环：Nesterov 加速梯度下降
// ----------------------------------------------------------------
// 论文 Algorithm 2：u/v 双解序列
//   v 是"探头"（比 u 多走一步），用来计算梯度
//   u 是"主解"（当前最佳布局位置）
// 步长 α = 1/L，L 通过 Lipschitz 预测（式29）：L_k = ||∇f(v_k)−∇f(v_{k−1})|| / ||v_k−v_{k−1}||
// ----------------------------------------------------------------
void GlobalPlacer::RunGlobalPlacement(int maxIters_, double tauMin_)
{
    maxIters = maxIters_;
    tauMin = tauMin_;
    iterCount = 0;

    printf("\n========== Nesterov 全局布局（任务5-D） ==========\n");

    // ① 初始化网格 + filler + 初始密度
    //    （必须先 InitGrid 再打印，否则 gridM 尚未赋值，日志会输出垃圾值）
    InitGrid();
    printf("最大迭代=%d  停止条件 τ<%.3f  网格 m=%d\n", maxIters, tauMin, gridM);

    InsertFiller();
    DistributeDensity();
    SolvePoisson();
    ComputeElectricField();
    int totalSize = (int)db->Nodes.size() + (int)fillers.size();

    // ② 保存初始坐标（u₀ = v₀ = 初始布局）
    ns.u = StoreCoords(db, fillers);
    ns.v = ns.u;
    ns.a = 1.0;
    ns.alpha = 0.044 * (binW + binH) / 2; // 初始步长兜底值（论文默认）
    ns.prevV = ns.u;
    ns.prevGrad.assign(totalSize, POS_2D());
    smoothGamma = 200.0f; // 初始 γ（后续会自动调节）
    ns.fGrad.assign(totalSize, POS_2D());

    // ③ 初始化 λ（需要 fieldX/fieldY 已就绪）
    InitLambda();

    // ④ 计算初始 HPWL（用于 λ 更新的 ΔHPWL）
    float prevHPWL = CalcHPWL();

    printf("\n  iter   HPWL       λ          γ       τ(%%)     N(势能)     α        Δt(s)\n");
    printf("  ---- -------- ---------- --------- ------ ------------ -------- -------\n");

    // 记录历史最佳（防止迭代发散时丢失最优解）
    float bestHPWL = prevHPWL;
    vector<POS_2D> bestU = ns.u;
    int staleCount = 0; // 连续未下降轮数

    // ==================== 主循环 ====================
    for (int k = 0; k < maxIters; k++)
    {
        iterCount = k + 1;
        clock_t t0 = clock();

        // 2-1: 把 v_k 写到 Module，用 v 处的坐标计算本轮梯度（论文 Algorithm 2 line 1）
        RestoreCoords(db, fillers, ns.v, coreLL, coreUR);

        // 2-2: 更新密度/电场/梯度（步骤 B/C）
        DistributeDensity();
        SolvePoisson();
        ComputeElectricField();
        double N = ComputeGradient(ns);

        // 2-3: Lipschitz 预测步长 α = 1/L（论文式29）
        //   L_k = ||∇f(v_k) − ∇f(v_{k−1})|| / ||v_k − v_{k−1}||
        //   α = 1/L
        // 第一轮用兜底值，之后用真实 Lipschitz 预测
        double alpha;
        if (k == 0)
        {
            alpha = ns.alpha;
        }
        else
        {
            double distV = 0, distG = 0;
            for (int i = 0; i < totalSize; i++)
            {
                double dvx = ns.v[i].x - ns.prevV[i].x;
                double dvy = ns.v[i].y - ns.prevV[i].y;
                double dgx = ns.fGrad[i].x - ns.prevGrad[i].x;
                double dgy = ns.fGrad[i].y - ns.prevGrad[i].y;
                distV += dvx * dvx + dvy * dvy;
                distG += dgx * dgx + dgy * dgy;
            }
            if (distV > 1e-12)
            {
                double L = sqrt(distG / distV);
                alpha = (L > 1e-6) ? 1.0 / L : ns.alpha;
            }
            else
            {
                alpha = ns.alpha;
            }
        }
        // 2-4: u_{k+1} = v_k − α·∇f(v_k)（论文 Algorithm 2 line 3）
        // 步长上限：不超过核心区尺寸的 1/10（防止跨区跳跃导致震荡）
        double maxStep = max(coreUR.x - coreLL.x, coreUR.y - coreLL.y) / 10.0;
        alpha = min(alpha, maxStep);

        // 【性能优化】改用成员缓冲直接写入，配合末尾的指针轮转，
        // 把原来每轮 6 次 45 万元素级的 vector 深拷贝（约 20 MB 内存流量，
        // 128 轮累计 2.7 GB）降为接近零。计算式的求值顺序与原来完全一致：
        // 仍然从 v_k、∇f(v_k)、u_k 出发。
        if ((int)bufU.size() != totalSize)
        {
            bufU.resize(totalSize);
            bufV.resize(totalSize);
        }

        for (int i = 0; i < totalSize; i++)
        {
            bufU[i].x = (float)max((double)coreLL.x,
                         min((double)coreUR.x, ns.v[i].x - alpha * ns.fGrad[i].x));
            bufU[i].y = (float)max((double)coreLL.y,
                         min((double)coreUR.y, ns.v[i].y - alpha * ns.fGrad[i].y));
        }

        // 2-5: Nesterov 参数更新（论文 Algorithm 2 line 4-5）
        double aOld = ns.a;
        ns.a = (1.0 + sqrt(4.0 * aOld * aOld + 1.0)) / 2.0;
        double momentum = (aOld - 1.0) / ns.a;
        for (int i = 0; i < totalSize; i++)
        {
            bufV[i].x = (float)max((double)coreLL.x,
                         min((double)coreUR.x, bufU[i].x + momentum * (bufU[i].x - ns.u[i].x)));
            bufV[i].y = (float)max((double)coreLL.y,
                         min((double)coreUR.y, bufU[i].y + momentum * (bufU[i].y - ns.u[i].y)));
        }

        // 缓冲轮转（全部是 O(1) 的指针交换）：
        //   prevV ← v_k, u ← u_{k+1}, v ← v_{k+1}, prevGrad ← ∇f(v_k)
        // bufU / bufV 接住被换下的旧 u / 旧 v，供下一轮复用。
        ns.prevV.swap(ns.v);
        ns.u.swap(bufU);
        ns.v.swap(bufV);
        ns.prevGrad.swap(ns.fGrad);

        // 2-6: 更新步长记录
        ns.alpha = alpha;

        // 2-7: 从主解 u 写回 Module（这才是本轮真正的布局结果）
        RestoreCoords(db, fillers, ns.u, coreLL, coreUR);

        // 2-8: 更新 γ、λ、溢出率
        UpdateGamma();
        float hpwl = CalcHPWL();
        double hpwlDelta = (double)hpwl - prevHPWL;
        UpdateLambda(hpwlDelta);
        double tau = CalcDensityOverflow();
        smoothGamma = (float)ns.gamma;

        clock_t t1 = clock();
        double dt = (double)(t1 - t0) / CLOCKS_PER_SEC;

        // 每 20 轮或关键轮打印一次
        if ((k % 20 == 0) || tau < tauMin || k == maxIters - 1)
        {
            printf(" %4d %9.0f %10.3e %8.2f %5.2f%% %12.4e %8.4f %6.2f\n",
                   k + 1, hpwl, ns.lambda, ns.gamma, tau * 100, N, alpha, dt);
        }

        // 早停：记录历史最佳，连续 20 轮不下降则停止
        if (hpwl < bestHPWL)
        {
            bestHPWL = hpwl;
            bestU = ns.u;
            staleCount = 0;
        }
        else
        {
            staleCount++;
        }
        if (staleCount >= 20 && k >= 30)
        {
            printf("\n  ✓ 早停：连续 20 轮 HPWL 未下降（最佳=%g，第 %d 轮）\n",
                   bestHPWL, k + 1);
            break;
        }

        prevHPWL = hpwl;

        // 收敛判据：至少跑 30 轮，之后检查密度溢出率
        if (k >= 30 && tau < tauMin)
        {
            printf("\n  ✓ 收敛：τ=%.3f%% < %.3f%%（第 %d 轮）\n", tau * 100, tauMin * 100, k + 1);
            break;
        }
    }

    // 恢复历史最佳布局
    RestoreCoords(db, fillers, bestU, coreLL, coreUR);

    if (CalcDensityOverflow() >= tauMin && iterCount == maxIters)
        printf("\n  ✗ 未收敛（达到最大迭代 %d 轮），τ=%.3f%%\n", maxIters, CalcDensityOverflow() * 100);

    float finalHPWL = CalcHPWL();
    printf("\n===== Nesterov 全局布局结束 =====\n");
    printf("  迭代 %d 轮   最终 HPWL = %.0f   最佳 HPWL = %.0f   最终 τ = %.3f%%\n",
           iterCount, finalHPWL, bestHPWL, CalcDensityOverflow() * 100);
    printf("===================================\n\n");
}
