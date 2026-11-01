#include "initial_placement.h"
#include <random>
#include <unordered_map>
#include <queue>
#include <functional>
#include <utility>
// Eigen：稀疏矩阵 + BiCGSTAB 求解器（任务4 方法③，任务5 也要用）
#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>

// ----------------------------------------------------------------
// 核心区域：所有行（SiteRow）的外接矩形
// ----------------------------------------------------------------
void InitialPlacer::SetupCoreRegion()
{
    float minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
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
// HPWL（半周长线长）：布局好坏的核心评价指标
// 对每个网络 Net：
//   遍历它所有引脚的绝对位置 = 单元中心 + 引脚相对中心的偏移
//   该网络的线长 = (x最大值 - x最小值) + (y最大值 - y最小值)
// 所有网络累加，就是整个布局的总线长。越小越好。
// ----------------------------------------------------------------
float InitialPlacer::CalcHPWL()
{
    float total = 0;
    for (size_t i = 0; i < db->Nets.size(); i++)
    {
        Net *net = db->Nets[i];
        float minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;

        for (size_t j = 0; j < net->netPins.size(); j++)
        {
            Pin *pin = net->netPins[j];
            if (pin->module == NULL) continue; // 防御：找不到单元的引脚跳过

            // 引脚的绝对位置 = 所属单元中心 + 引脚相对中心的偏移
            float x = pin->module->center.x + pin->offset.x;
            float y = pin->module->center.y + pin->offset.y;
            minX = min(minX, x);
            maxX = max(maxX, x);
            minY = min(minY, y);
            maxY = max(maxY, y);
        }
        // 累加该网络的 x 跨度 + y 跨度
        total += (maxX - minX) + (maxY - minY);
    }
    return total;
}

// ----------------------------------------------------------------
// 方法①：随机布局（最简单，纯"扔骰子"）
// 给每个可移动单元，在核心区内随机分配一个左下角坐标，
// 完全不考虑单元之间会不会重叠，也不看任何连接关系。
// 作用：作为"对照组"，用来衬托其他方法有多好。
// ----------------------------------------------------------------
void InitialPlacer::RandomPlacement()
{
    // 固定随机种子，保证每次运行结果相同（方便复现和对比）
    mt19937 rng(42);
    // 均匀分布在核心区范围内取随机数
    uniform_real_distribution<float> distX(coreLL.x, coreUR.x);
    uniform_real_distribution<float> distY(coreLL.y, coreUR.y);

    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        Module *m = db->Nodes[i];
        // 随机生成左下角坐标
        float x = distX(rng);
        float y = distY(rng);
        // 裁剪：保证整个单元不超出核心区右/上边界
        x = min(x, coreUR.x - m->width);
        y = min(y, coreUR.y - m->height);
        // 更新左下角坐标和中心坐标
        m->origin.x = x;
        m->origin.y = y;
        m->center.x = x + m->width / 2.0;
        m->center.y = y + m->height / 2.0;
    }
}

// ----------------------------------------------------------------
// 方法②：聚类驱动布局（Clustering-Driven Placement）
// 核心思想："分而治之" —— 不直接摆 21 万单元，而是：
//   第1步 聚类：把"连接紧密"的单元先归成一堆一堆的"簇"
//   第2步 簇内布局：每个簇占一块矩形区域，簇内单元排进小格子
// 好处：关系好的单元放进同一个簇，簇整体放一起，线自然短。
// ----------------------------------------------------------------
namespace
{
// 贪心合并用的优先级队列元素：连接强度 w，把 v 合并进 u
struct MergeItem
{
    int w, u, v;
    bool operator<(const MergeItem &o) const { return w < o.w; } // 最大堆
};
}

void InitialPlacer::ClusteringPlacement()
{
    const int N = (int)db->Nodes.size();
    if (N == 0) return;

    // ========== 第1步：构建连接强度图 ==========
    // 连接强度 = 两个单元"共享多少个网络"（连得越多越紧密）
    vector<unordered_map<int, int>> wgt(N); // wgt[u][v] = u 和 v 的连接强度
    // 单元 → Nodes 下标：按 Module::idx 直接数组寻址（同 QuadraticPlacement 的做法，
    // 避免在对全部网络引脚遍历时反复做哈希查找）
    vector<int> nodeIndex(db->moduleCount > 0 ? db->moduleCount : N, -1);
    for (int i = 0; i < N; i++) nodeIndex[db->Nodes[i]->idx] = i;

    for (size_t k = 0; k < db->Nets.size(); k++)
    {
        Net *net = db->Nets[k];
        // 收集本网络内可移动单元的 Nodes 下标
        vector<int> mids;
        for (size_t j = 0; j < net->netPins.size(); j++)
        {
            Module *m = net->netPins[j]->module;
            if (m && m->idx >= 0 && m->idx < (int)nodeIndex.size() && nodeIndex[m->idx] >= 0)
                mids.push_back(nodeIndex[m->idx]);
        }
        // 网络内单元两两之间，连接强度 +1
        // 超大网络（如时钟）几乎连所有单元，没有聚类区分度，只取前 40 个控制开销
        const int MAX_PAIR = 40;
        if ((int)mids.size() > MAX_PAIR) mids.resize(MAX_PAIR);
        for (size_t a = 0; a < mids.size(); a++)
            for (size_t b = a + 1; b < mids.size(); b++)
            {
                int u = mids[a], v = mids[b];
                if (u == v) continue; // 同一模块可能有多个引脚连同一网络 → 跳过自环
                wgt[u][v]++;
                wgt[v][u]++; // 存双向，合并时方便查邻居
            }
    }

    // ========== 第2步：贪心合并成簇（直到达到目标簇数） ==========
    vector<int> parent(N);
    for (int i = 0; i < N; i++) parent[i] = i;
    // 并查集找根（路径压缩）
    function<int(int)> find = [&](int x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    // 把所有"有连接"的簇对放进最大堆，每次取连接最强的一对合并
    priority_queue<MergeItem> pq;
    for (int u = 0; u < N; u++)
        for (auto &kv : wgt[u])
            if (u < kv.first) // 每对只 push 一次（避免重复）
                pq.push({kv.second, u, kv.first});

    const int TARGET = 500; // 目标簇数（调小 → 簇更大更粗，调大 → 簇更小更细）
    int active = N;
    while (active > TARGET && !pq.empty())
    {
        MergeItem top = pq.top();
        pq.pop();
        int ru = find(top.u), rv = find(top.v);
        if (ru == rv) continue; // 已经同簇，忽略
        // 惰性删除：堆里的旧记录若权重已不是当前值，说明已过期，忽略
        auto it = wgt[ru].find(rv);
        if (it == wgt[ru].end() || it->second != top.w) continue;

        // 合并：把 rv 并入 ru
        parent[rv] = ru;
        // 先拷贝 rv 的邻居列表：合并过程会修改其他 map，
        // 若边遍历 wgt[rv] 边改，迭代器会失效导致崩溃（已踩坑）
        vector<pair<int, int>> nbrList(wgt[rv].begin(), wgt[rv].end());
        for (size_t ni = 0; ni < nbrList.size(); ni++)
        {
            int nbr = nbrList[ni].first, w = nbrList[ni].second;
            if (nbr == ru) continue;
            wgt[ru][nbr] += w;    // ru 的强度累加上 rv 的
            wgt[nbr][ru] += w;    // 对称更新
            wgt[nbr].erase(rv);   // 邻居忘掉 rv
            pq.push({wgt[ru][nbr], ru, nbr}); // 新强度入堆，供后续合并
        }
        wgt[rv].clear();
        wgt[ru].erase(rv);
        active--;
    }

    // ========== 第3步：簇内布局 ==========
    // 把单元按所属簇分组，方便逐个簇处理
    vector<vector<int>> members(N);
    for (int i = 0; i < N; i++) members[find(i)].push_back(i);
    vector<int> roots;
    for (int i = 0; i < N; i++) if (find(i) == i) roots.push_back(i);

    // 瀑布式填充：从左到右放簇块，一行放满就换行
    const float UTIL = 0.8f; // 簇内利用率（留 20% 空隙给布线）
    const float GAP = 4.0f;  // 簇块之间的间隔
    float curX = coreLL.x, curY = coreLL.y, rowH = 0;

    for (size_t rIdx = 0; rIdx < roots.size(); rIdx++)
    {
        int r = roots[rIdx];
        // 簇的总面积
        float cArea = 0;
        for (size_t i = 0; i < members[r].size(); i++)
            cArea += db->Nodes[members[r][i]]->area;
        // 簇块按面积折算成正方形边长
        float side = sqrt(cArea / UTIL);
        // 放不下当前行则换行
        if (curX + side > coreUR.x)
        {
            curX = coreLL.x;
            curY += rowH + GAP;
            rowH = 0;
        }

        // 簇内切成 cols×rows 个小格子
        int nCell = (int)members[r].size();
        int cols = max(1, (int)ceil(sqrt((double)nCell)));
        int rows = (nCell + cols - 1) / cols;
        float cellW = side / cols;
        float cellH = side / rows;

        // 把簇内单元依次放进格子（每个格子放一个单元的中心）
        for (size_t k = 0; k < members[r].size(); k++)
        {
            Module *m = db->Nodes[members[r][k]];
            int cc = (int)k % cols;
            int rr = (int)k / cols;
            m->center.x = curX + cellW * (cc + 0.5f);
            m->center.y = curY + cellH * (rr + 0.5f);
            m->origin.x = m->center.x - m->width / 2;
            m->origin.y = m->center.y - m->height / 2;
        }
        rowH = max(rowH, side);
        curX += side + GAP;
    }
}

// ----------------------------------------------------------------
// 方法③：二次方程解析布局（Quadratic Placement）—— 任务4 的"重头戏"
// 核心思想：把"让连线变短"翻译成一个二次函数，求它的最小值。
//   二次函数最小化 → 解大型稀疏线性方程组  AX = b
//     A：拉普拉斯矩阵（稀疏，Eigen SparseMatrix + CSR 格式）
//     b：由引脚偏移和固定终端构成的常数项
//   求解器：Eigen 的 BiCGSTAB（适合大规模稀疏方程）
// 因为权重 w 依赖当前单元距离，所以要反复"算权重 → 求解"，
// 每轮用上一轮的位置作初值（solveWithGuess），收敛很快。
// ----------------------------------------------------------------
void InitialPlacer::QuadraticPlacement()
{
    const int N = (int)db->Nodes.size();
    if (N == 0) return;

    // 单元 → Nodes 内下标：按 Module::idx 直接数组寻址（替代 unordered_map）。
    // 内层“引脚两两配对”循环每轮要查上千万次，哈希查找是二次方程布局的
    // 主要开销之一；数组寻址是 O(1)，且对缓存友好。
    vector<int> nodeIndex(db->moduleCount > 0 ? db->moduleCount : N, -1);
    for (int i = 0; i < N; i++) nodeIndex[db->Nodes[i]->idx] = i;

    // 三元组数量预估（网络拓扑各轮不变，只算一次）：
    // 原实现固定 reserve(8000000)，实际规模不足时白占内存，超出时又要多轮
    // 翻倍扩容并整体拷贝，adaptec1 上内存峰值可达 700 MB 以上。这里先按
    // 实际拓扑算出精确上界，每轮只分配一次、不再扩容。
    size_t tripEst = 0;
    for (size_t k = 0; k < db->Nets.size(); k++)
    {
        const vector<Pin *> &ps = db->Nets[k]->netPins;
        for (size_t a = 0; a < ps.size(); a++)
        {
            for (size_t b = a + 1; b < ps.size(); b++)
            {
                if (ps[a]->module == NULL || ps[b]->module == NULL) continue;
                if (ps[a]->module == ps[b]->module) continue;
                bool ma = !ps[a]->module->isFixed;
                bool mb = !ps[b]->module->isFixed;
                if (!ma && !mb) continue;
                tripEst += (ma && mb) ? 4 : 1; // 两端可移动 → 4 条，含固定端 → 1 条
            }
        }
    }

    // 第一步：把所有单元的中心先放到核心区中心（PDF 第二步）
    float cx = (coreLL.x + coreUR.x) / 2.0f;
    float cy = (coreLL.y + coreUR.y) / 2.0f;
    Eigen::VectorXf posX(N), posY(N);
    for (int i = 0; i < N; i++)
    {
        Module *m = db->Nodes[i];
        m->center.x = cx;
        m->center.y = cy;
        posX[i] = cx;
        posY[i] = cy;
    }

    const float CONSTANT   = 2.0f;   // 权重常数（可调，影响收敛速度）
    const float MIN_DIST   = 1e-3f;  // 最小距离，防止距离为 0 时权重爆炸
    const float TARGET_ERR = 5e-4f;  // 收敛目标残差（初始布局不需收敛到极致）
    const int   MAX_ITER   = 10;     // 最大迭代轮数

    float lastHPWL = 0;
    for (int iter = 0; iter < MAX_ITER; iter++)
    {
        // ---------- 构建 A 和 b（x、y 两个方向分开，结构相同） ----------
        Eigen::SparseMatrix<float, Eigen::RowMajor> X_A(N, N), Y_A(N, N);
        Eigen::VectorXf X_b = Eigen::VectorXf::Zero(N);
        Eigen::VectorXf Y_b = Eigen::VectorXf::Zero(N);
        vector<Eigen::Triplet<float>> tripX, tripY;
        tripX.reserve(tripEst);
        tripY.reserve(tripEst);

        // 遍历每条网络里的每一对引脚
        for (size_t k = 0; k < db->Nets.size(); k++)
        {
            Net *net = db->Nets[k];
            const vector<Pin *> &pins = net->netPins;
            for (size_t a = 0; a < pins.size(); a++)
            {
                for (size_t b = a + 1; b < pins.size(); b++)
                {
                    Pin *pa = pins[a], *pb = pins[b];
                    if (pa->module == NULL || pb->module == NULL) continue;
                    // 同一单元的多个引脚之间没有独立自由度，跳过
                    if (pa->module == pb->module) continue;

                    bool ma = !pa->module->isFixed; // a 可移动？
                    bool mb = !pb->module->isFixed; // b 可移动？
                    if (!ma && !mb) continue;       // 两端都是固定终端 → 无未知数

                    // 引脚绝对坐标（用于算权重）
                    float xa = pa->module->center.x + pa->offset.x;
                    float xb = pb->module->center.x + pb->offset.x;
                    float ya = pa->module->center.y + pa->offset.y;
                    float yb = pb->module->center.y + pb->offset.y;

                    // 权重 = 常数 / 距离（短连线拉得紧、权重高）
                    float dx = max(fabsf(xa - xb), MIN_DIST);
                    float dy = max(fabsf(ya - yb), MIN_DIST);
                    float wx = CONSTANT / dx;
                    float wy = CONSTANT / dy;

                    if (ma && mb)
                    {
                        // 情况(i)：两端都可移动 → 标准拉普拉斯块
                        int ia = nodeIndex[pa->module->idx];
                        int ib = nodeIndex[pb->module->idx];
                        tripX.push_back(Eigen::Triplet<float>(ia, ia,  wx));
                        tripX.push_back(Eigen::Triplet<float>(ia, ib, -wx));
                        tripX.push_back(Eigen::Triplet<float>(ib, ia, -wx));
                        tripX.push_back(Eigen::Triplet<float>(ib, ib,  wx));
                        tripY.push_back(Eigen::Triplet<float>(ia, ia,  wy));
                        tripY.push_back(Eigen::Triplet<float>(ia, ib, -wy));
                        tripY.push_back(Eigen::Triplet<float>(ib, ia, -wy));
                        tripY.push_back(Eigen::Triplet<float>(ib, ib,  wy));
                        // 常数项来自 offset 差
                        X_b[ia] += -wx * (pa->offset.x - pb->offset.x);
                        X_b[ib] +=  wx * (pa->offset.x - pb->offset.x);
                        Y_b[ia] += -wy * (pa->offset.y - pb->offset.y);
                        Y_b[ib] +=  wy * (pa->offset.y - pb->offset.y);
                    }
                    else if (ma)
                    {
                        // 情况(ii)：a 可移动，b 是固定终端 → 终端位置并入常数项
                        int ia = nodeIndex[pa->module->idx];
                        tripX.push_back(Eigen::Triplet<float>(ia, ia, wx));
                        tripY.push_back(Eigen::Triplet<float>(ia, ia, wy));
                        X_b[ia] += -wx * (pa->offset.x - pb->offset.x - pb->module->center.x);
                        Y_b[ia] += -wy * (pa->offset.y - pb->offset.y - pb->module->center.y);
                    }
                    else
                    {
                        // 情况(iii)：a 是固定终端，b 可移动
                        int ib = nodeIndex[pb->module->idx];
                        tripX.push_back(Eigen::Triplet<float>(ib, ib, wx));
                        tripY.push_back(Eigen::Triplet<float>(ib, ib, wy));
                        X_b[ib] += -wx * (pb->offset.x - pa->offset.x - pa->module->center.x);
                        Y_b[ib] += -wy * (pb->offset.y - pa->offset.y - pa->module->center.y);
                    }
                }
            }
        }

        // 一次性把 Triplet 压缩成稀疏矩阵（先累加信息，再统一建矩阵，比逐个 insert 快得多）
        X_A.setFromTriplets(tripX.begin(), tripX.end());
        Y_A.setFromTriplets(tripY.begin(), tripY.end());

        // ---------- 用 BiCGSTAB 求解 AX = b ----------
        // 以上一轮位置作初值：解具有连续性，初值越准收敛越快
        Eigen::BiCGSTAB<Eigen::SparseMatrix<float, Eigen::RowMajor>,
                        Eigen::IdentityPreconditioner> solverX, solverY;
        solverX.setMaxIterations(100);
        solverX.compute(X_A);
        Eigen::VectorXf newX = solverX.solveWithGuess(X_b, posX);
        solverY.setMaxIterations(100);
        solverY.compute(Y_A);
        Eigen::VectorXf newY = solverY.solveWithGuess(Y_b, posY);

        bool convX = (solverX.info() == Eigen::Success);
        bool convY = (solverY.info() == Eigen::Success);

        // ---------- 更新位置 + 边界裁剪 ----------
        for (int i = 0; i < N; i++)
        {
            Module *m = db->Nodes[i];
            float x = convX ? newX[i] : posX[i]; // 求解失败则保留原位置
            float y = convY ? newY[i] : posY[i];
            // 中心不超出核心区（PDF 参考代码）
            const float EPS = 1e-3f;
            if (x - 0.5f * m->width < coreLL.x)  x = coreLL.x + 0.5f * m->width + EPS;
            if (x + 0.5f * m->width > coreUR.x)  x = coreUR.x - 0.5f * m->width - EPS;
            if (y - 0.5f * m->height < coreLL.y) y = coreLL.y + 0.5f * m->height + EPS;
            if (y + 0.5f * m->height > coreUR.y) y = coreUR.y - 0.5f * m->height - EPS;
            m->center.x = x;
            m->center.y = y;
            m->origin.x = x - 0.5f * m->width;
            m->origin.y = y - 0.5f * m->height;
            posX[i] = x;
            posY[i] = y;
        }

        // ---------- 收敛判断：残差足够小，或 HPWL 几乎不再变化 ----------
        float err = max(solverX.error(), solverY.error());
        float hpwl = CalcHPWL();
        float relChange = fabsf(hpwl - lastHPWL) / max(1.0f, hpwl);
        if (iter > 4 && (err < TARGET_ERR || relChange < 1e-4f)) break;
        lastHPWL = hpwl;
    }
}

// ----------------------------------------------------------------
// 密度统计：衡量布局的"面积控制"能力
// 把核心区切成 64×64 网格，统计每个网格里单元面积占网格面积的比例。
//   峰值密度 > 100% → 该网格塞的单元面积超过格子本身 → 拥挤/重叠
// 三种方法对比：
//   随机布局  → 有的格子空、有的格子爆满（峰值密度极高）
//   聚类布局  → 瀑布填充，比较规整（峰值接近利用率）
//   二次方程  → 只求线长最优、不管密度，单元可能挤成一团（峰值极高）
// ----------------------------------------------------------------
void InitialPlacer::ReportDensity()
{
    const int binsX = 64, binsY = 64;
    float gw = (coreUR.x - coreLL.x) / binsX; // 每个格子的宽高
    float gh = (coreUR.y - coreLL.y) / binsY;
    vector<float> gridArea(binsX * binsY, 0);

    // 每个可移动单元，按其中心所在的格子累加面积
    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        Module *m = db->Nodes[i];
        int ix = (int)((m->center.x - coreLL.x) / gw);
        int iy = (int)((m->center.y - coreLL.y) / gh);
        if (ix >= 0 && ix < binsX && iy >= 0 && iy < binsY)
            gridArea[iy * binsX + ix] += m->area;
    }

    // 统计过满格子数和峰值密度
    float cellArea = gw * gh;
    int overflow = 0;
    float maxDensity = 0;
    for (size_t i = 0; i < gridArea.size(); i++)
    {
        float d = gridArea[i] / cellArea;
        if (d > maxDensity) maxDensity = d;
        if (d > 1.0f) overflow++; // 超过 100% = 拥挤
    }
    printf("        面积控制: 过满网格 %d/%d, 峰值密度 %.1f%%\n",
           overflow, binsX * binsY, maxDensity * 100);
}

// ----------------------------------------------------------------
// 获取峰值密度（供主程序动态对比表使用）
// ----------------------------------------------------------------
float InitialPlacer::GetPeakDensity()
{
    const int binsX = 64, binsY = 64;
    float gw = (coreUR.x - coreLL.x) / binsX;
    float gh = (coreUR.y - coreLL.y) / binsY;
    vector<float> gridArea(binsX * binsY, 0);

    for (size_t i = 0; i < db->Nodes.size(); i++)
    {
        Module *m = db->Nodes[i];
        int ix = (int)((m->center.x - coreLL.x) / gw);
        int iy = (int)((m->center.y - coreLL.y) / gh);
        if (ix >= 0 && ix < binsX && iy >= 0 && iy < binsY)
            gridArea[iy * binsX + ix] += m->area;
    }

    float cellArea = gw * gh;
    float maxDensity = 0;
    for (size_t i = 0; i < gridArea.size(); i++)
    {
        float d = gridArea[i] / cellArea;
        if (d > maxDensity) maxDensity = d;
    }
    return maxDensity;
}
