#include "legalization.h"
#include <algorithm>
#include <cmath>
#include <cfloat>

// ================================================================
// 任务7 布局合法化（Abacus 算法）实现
// ================================================================

// ----------------------------------------------------------------
// 建立行结构：从 .scl 解析出的 SiteRow 转换成算法内部的行数据
// ----------------------------------------------------------------
void Legalizer::SetupRows()
{
    rows.clear();
    for (size_t i = 0; i < db->SiteRows.size(); i++)
    {
        SiteRow &sr = db->SiteRows[i];
        LegRow r;
        r.left      = sr.start.x;
        r.right     = sr.end.x;
        r.bottom    = sr.bottom;
        r.height    = sr.height;
        rows.push_back(r);
    }
    // 按 bottom 排序（解析顺序通常已有序，保险起见再排一次）
    sort(rows.begin(), rows.end(), [](const LegRow &a, const LegRow &b) {
        return a.bottom < b.bottom;
    });
    rowCnt = (int)rows.size();
}

// ----------------------------------------------------------------
// 建立障碍：把固定终端（大块 I/O、电源等）在每行内的占用区间标记出来
// 标准单元合法化时不能放进这些区间，否则会与固定终端重叠
// ----------------------------------------------------------------
void Legalizer::BuildObstacles()
{
    for (int i = 0; i < rowCnt; i++)
        rows[i].obstacles.clear();

    for (size_t k = 0; k < db->Terminals.size(); k++)
    {
        Module *t = db->Terminals[k];
        double ty0 = t->center.y - t->height / 2;
        double ty1 = t->center.y + t->height / 2;
        double tx0 = t->center.x - t->width / 2;
        double tx1 = t->center.x + t->width / 2;

        // 终端可能跨多行：遍历所有行找 y 重叠的行
        for (int i = 0; i < rowCnt; i++)
        {
            double ry0 = rows[i].bottom, ry1 = rows[i].bottom + rows[i].height;
            if (ty1 > ry0 + 1e-9 && ty0 < ry1 - 1e-9) // 行的 y 与终端 y 有重叠
            {
                // 把终端的 x 区间裁剪到行内
                double ox0 = max(tx0, rows[i].left);
                double ox1 = min(tx1, rows[i].right);
                if (ox1 > ox0 + 1e-9)
                    rows[i].obstacles.push_back(make_pair(ox0, ox1));
            }
        }
    }

    // 每行障碍排序合并（相邻区间合并成一段）
    int obsCnt = 0;
    for (int i = 0; i < rowCnt; i++)
    {
        sort(rows[i].obstacles.begin(), rows[i].obstacles.end());
        vector<pair<double, double>> merged;
        for (size_t k = 0; k < rows[i].obstacles.size(); k++)
        {
            if (merged.empty() || rows[i].obstacles[k].first > merged.back().second + 1e-9)
                merged.push_back(rows[i].obstacles[k]);
            else
                merged.back().second = max(merged.back().second, rows[i].obstacles[k].second);
        }
        rows[i].obstacles = merged;
        obsCnt += (int)merged.size();
    }
    printf("[合法化] 固定终端障碍（行内被占用的行段）总数 = %d\n", obsCnt);
}

// ----------------------------------------------------------------
// 找离 cy 最近的 CAND_ROW 个候选行（二分定位 + 就近排序）
// ----------------------------------------------------------------
void Legalizer::FindCandidateRows(double cy, vector<int> &cands)
{
    cands.clear();
    if (rowCnt == 0) return;

    // 二分：找第一个"行中心 >= cy"的行
    int lo = 0, hi = rowCnt;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (rows[mid].bottom + rows[mid].height / 2 < cy) lo = mid + 1;
        else hi = mid;
    }

    // 取 lo 附近 ±2 行作为候选池，再按距离排序取最近 CAND_ROW 个
    vector<int> idx;
    for (int d = -2; d <= 2; d++)
    {
        int i = lo + d;
        if (i >= 0 && i < rowCnt) idx.push_back(i);
    }
    sort(idx.begin(), idx.end(), [&](int a, int b) {
        double da = fabs(rows[a].bottom + rows[a].height / 2 - cy);
        double dbv = fabs(rows[b].bottom + rows[b].height / 2 - cy);
        return da < dbv;
    });
    for (int i = 0; i < (int)idx.size() && i < CAND_ROW; i++)
        cands.push_back(idx[i]);
}

// ----------------------------------------------------------------
// 试放：把单元 c 放进 row，返回方案总位移；放不下返回 -1
//   bestX 输出建议的放置位置（左下角 x），供 CommitPlace 使用。
//   方法：在行内找"自由段"（减去固定终端障碍、减去已放单元后的空隙），
//   选一个能放下该单元、且离单元原位最近的自由位置。
//   这样天然保证：不与终端重叠、不与已放单元重叠、尽量少动。
//   （也解决了单元贴行右边界导致原位放不下的问题）
// ----------------------------------------------------------------
double Legalizer::TryPlace(LegRow &row, Module *c, double &bestX)
{
    double leftX = c->center.x - c->width / 2; // 单元想放的左下角 x（尽量少动）
    double yDisp = fabs(row.bottom + row.height / 2 - c->center.y); // y 对齐位移
    int n = (int)row.cells.size();
    int no = (int)row.obstacles.size();
    double best = DBL_MAX;
    bestX = -1;

    // 障碍（已排序合并）和已放单元（按 x 有序）都非降序，双指针归并扫描。
    // cur = 当前自由段左端，逐个取占用区间，自由段 = [cur, 占用起点]。
    double cur = row.left;
    int oi = 0, ci = 0;
    while (oi < no || ci < n)
    {
        // 取下一个占用区间：起点更靠左的优先（相同起点先障碍，无影响）
        double occStart, occEnd;
        if (ci >= n || (oi < no && row.obstacles[oi].first <= row.cellX[ci]))
        {
            occStart = row.obstacles[oi].first;
            occEnd   = row.obstacles[oi].second;
            oi++;
        }
        else
        {
            occStart = row.cellX[ci];
            occEnd   = row.cellX[ci] + row.cells[ci]->width;
            ci++;
        }
        if (occStart >= row.right) break; // 之后的占用都在行外了

        // 自由段 [cur, occStart]
        double segRight = min(occStart, row.right);
        if (segRight - cur >= c->width - 1e-9) // 自由段足够宽
        {
            double x = max(cur, min(leftX, segRight - c->width));
            double disp = fabs(x - leftX) + yDisp;
            if (disp < best) { best = disp; bestX = x; }
        }
        cur = max(cur, occEnd); // 占用区间可能相互重叠，cur 取最大右端
    }
    // 最后一个自由段（到行右端）
    if (row.right - cur >= c->width - 1e-9)
    {
        double x = max(cur, min(leftX, row.right - c->width));
        double disp = fabs(x - leftX) + yDisp;
        if (disp < best) { best = disp; bestX = x; }
    }

    return (best < DBL_MAX) ? best : -1.0;
}

// ----------------------------------------------------------------
// 正式放置：把单元 c 放到行内 x 位置，同步更新行内列表和单元坐标
//   若 x 落在空隙（方案B），右侧单元无需移动；若 x 与右侧重叠（方案A），
//   右侧整体右推 —— 两种情况这里统一处理
// ----------------------------------------------------------------
void Legalizer::CommitPlace(LegRow &row, Module *c, double x)
{
    double centerY = row.bottom + row.height / 2;
    PlaceCell(c, x, centerY);

    int insert = (int)(lower_bound(row.cellX.begin(), row.cellX.end(), x) - row.cellX.begin());
    row.cells.insert(row.cells.begin() + insert, c);
    row.cellX.insert(row.cellX.begin() + insert, x);

    // 右侧整体右推（自由段方案下右侧本来不重叠，推挤是空操作）
    double cursor = x + c->width;
    for (int i = insert + 1; i < (int)row.cells.size(); i++)
    {
        double nx = max(row.cellX[i], cursor);
        row.cellX[i] = nx;
        PlaceCell(row.cells[i], nx, centerY);
        cursor = nx + row.cells[i]->width;
    }
}

// ----------------------------------------------------------------
// 设置单元新坐标：左下角 x = x，中心 y = centerY
// ----------------------------------------------------------------
void Legalizer::PlaceCell(Module *c, double x, double centerY)
{
    c->center.x = (float)(x + c->width / 2);
    c->center.y = (float)centerY;
    c->origin.x = (float)x;
    c->origin.y = (float)(centerY - c->height / 2);
}

// ----------------------------------------------------------------
// 结束时统一计算净位移（最终位置 - 原始位置），更符合作业口径
// ----------------------------------------------------------------
void Legalizer::CalcDisplacement()
{
    totalDisp = maxDisp = 0;
    int n = (int)db->Nodes.size();
    // 位移分布诊断
    int h[5] = {0,0,0,0,0}; // <10, <100, <500, <2000, >=2000
    Module *worst = NULL;
    double worstD = 0, wox=0, woy=0, wnx=0, wny=0;
    for (int k = 0; k < n; k++)
    {
        Module *m = db->Nodes[k];
        double dx = fabs(m->center.x - origCenter[k].x);
        double dy = fabs(m->center.y - origCenter[k].y);
        double d = dx + dy;
        totalDisp += d;
        if (d > maxDisp) maxDisp = d;
        if (d < 10) h[0]++;
        else if (d < 100) h[1]++;
        else if (d < 500) h[2]++;
        else if (d < 2000) h[3]++;
        else h[4]++;
        if (d > worstD) { worstD = d; worst = m; wox=origCenter[k].x; woy=origCenter[k].y; wnx=m->center.x; wny=m->center.y; }
    }
    avgDisp = (n > 0 ? totalDisp / n : 0);
    printf("[诊断] 位移分布: <10:%d  <100:%d  <500:%d  <2000:%d  >=2000:%d\n",
           h[0], h[1], h[2], h[3], h[4]);
    if (worst)
        printf("[诊断] 最大位移单元 %s: 原(%.0f,%.0f) -> 新(%.0f,%.0f) 位移=%.0f\n",
               worst->name.c_str(), wox, woy, wnx, wny, worstD);
}

// ----------------------------------------------------------------
// 网格法统计重叠单元对数（只检查同格 + 相邻格，避免 O(n²)）
// ----------------------------------------------------------------
long long Legalizer::CountOverlaps()
{
    if (rows.empty()) return 0;
    double minX = rows[0].left, maxX = rows[0].right;
    double minY = rows[0].bottom, maxY = rows.back().bottom + rows.back().height;
    for (int i = 0; i < rowCnt; i++)
    {
        minX = min(minX, rows[i].left);   maxX = max(maxX, rows[i].right);
        minY = min(minY, rows[i].bottom); maxY = max(maxY, rows[i].bottom + rows[i].height);
    }

    const int B = 256; // 网格维度
    double gw = (maxX - minX) / B, gh = (maxY - minY) / B;
    if (gw <= 0 || gh <= 0) return 0;

    vector<vector<int>> grid((size_t)B * B);
    const int N = (int)db->Nodes.size();
    for (int k = 0; k < N; k++)
    {
        Module *m = db->Nodes[k];
        int ix = (int)((m->center.x - minX) / gw);
        int iy = (int)((m->center.y - minY) / gh);
        if (ix < 0) ix = 0; if (ix >= B) ix = B - 1;
        if (iy < 0) iy = 0; if (iy >= B) iy = B - 1;
        grid[(size_t)iy * B + ix].push_back(k);
    }

    // 矩形重叠判断（含边界容差）
    auto overlap = [](Module *a, Module *b) {
        double ax0 = a->center.x - a->width / 2, ax1 = a->center.x + a->width / 2;
        double ay0 = a->center.y - a->height / 2, ay1 = a->center.y + a->height / 2;
        double bx0 = b->center.x - b->width / 2, bx1 = b->center.x + b->width / 2;
        double by0 = b->center.y - b->height / 2, by1 = b->center.y + b->height / 2;
        return (ax1 > bx0 + 1e-9 && bx1 > ax0 + 1e-9 && ay1 > by0 + 1e-9 && by1 > ay0 + 1e-9);
    };

    long long cnt = 0;
    // 同格内两两 + 右/下/右下/左下四个相邻格（保证每对只统计一次）
    int dxx[4] = {1, 0, 1, -1};
    int dyy[4] = {0, 1, 1, 1};
    for (int iy = 0; iy < B; iy++)
    {
        for (int ix = 0; ix < B; ix++)
        {
            vector<int> &g = grid[(size_t)iy * B + ix];
            for (size_t a = 0; a < g.size(); a++)
                for (size_t b = a + 1; b < g.size(); b++)
                    if (overlap(db->Nodes[g[a]], db->Nodes[g[b]])) cnt++;
            for (int d = 0; d < 4; d++)
            {
                int nx = ix + dxx[d], ny = iy + dyy[d];
                if (nx < 0 || nx >= B || ny < 0 || ny >= B) continue;
                vector<int> &h = grid[(size_t)ny * B + nx];
                for (size_t a = 0; a < g.size(); a++)
                    for (size_t b = 0; b < h.size(); b++)
                        if (overlap(db->Nodes[g[a]], db->Nodes[h[b]])) cnt++;
            }
        }
    }
    return cnt;
}

// ----------------------------------------------------------------
// 统计中心 y 不在任何行内的单元数（用二分，避免 O(n×行数)）
// ----------------------------------------------------------------
int Legalizer::CountMisaligned()
{
    int cnt = 0;
    for (size_t k = 0; k < db->Nodes.size(); k++)
    {
        Module *m = db->Nodes[k];
        double cy = m->center.y;
        // 二分找第一个 bottom+height >= cy 的行
        int lo = 0, hi = rowCnt;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (rows[mid].bottom + rows[mid].height < cy) lo = mid + 1;
            else hi = mid;
        }
        bool aligned = false;
        for (int i = max(0, lo - 1); i <= min(rowCnt - 1, lo); i++)
        {
            if (cy >= rows[i].bottom - 1e-6 && cy <= rows[i].bottom + rows[i].height + 1e-6)
            {
                aligned = true;
                break;
            }
        }
        if (!aligned) cnt++;
    }
    return cnt;
}

// ----------------------------------------------------------------
// 打印行统计信息
// ----------------------------------------------------------------
void Legalizer::PrintRowInfo()
{
    double totalCap = 0, totalUsed = 0;
    for (int i = 0; i < rowCnt; i++)
    {
        totalCap += (rows[i].right - rows[i].left);
        // 实际占用 = 行内所有单元宽度之和（不是最右端位置）
        for (size_t k = 0; k < rows[i].cells.size(); k++)
            totalUsed += rows[i].cells[k]->width;
    }
    printf("[合法化] 行数=%d  行总容量=%.0f (站点)  实际占用=%.0f  利用率=%.1f%%\n",
           rowCnt, totalCap, totalUsed,
           (totalCap > 0 ? totalUsed / totalCap * 100 : 0));
}

// ================================================================
// 主入口
// ================================================================
void Legalizer::Run()
{
    printf("\n========== 任务7：标准单元合法化（Abacus 算法） ==========\n");

    // ① 备份原始坐标（用于净位移统计），建立行结构
    origCenter.clear();
    for (size_t k = 0; k < db->Nodes.size(); k++)
        origCenter.push_back(db->Nodes[k]->center);
    SetupRows();
    BuildObstacles();   // 标记固定终端占用的行段为障碍
    PrintRowInfo();

    // ② 合法化前统计
    overlapBefore = CountOverlaps();
    misplacedBefore = CountMisaligned();
    printf("[合法化前] 重叠单元对=%lld   未对齐行单元数=%d (共 %d)\n",
           overlapBefore, misplacedBefore, (int)db->Nodes.size());

    // ③ 单元按 y 降序排序（先放顶部，后面的向下找行）
    vector<Module *> cells = db->Nodes;
    sort(cells.begin(), cells.end(), [](Module *a, Module *b) {
        return a->center.y > b->center.y;
    });

    // ④ 逐个合法化：候选行试放 → 取位移最小的行 → 正式落位
    int placed = 0;
    int notPlacedCnt = 0;
    double rowH = (rowCnt > 0 ? rows[0].height : 0);
    for (size_t k = 0; k < cells.size(); k++)
    {
        Module *c = cells[k];
        // 防御：高于行高的单元（宏单元）不在标准单元合法化范围，跳过
        if (c->height > rowH + 1e-6) continue;

        vector<int> cands;
        FindCandidateRows(c->center.y, cands);

        double best = DBL_MAX;
        int bestRow = -1;
        double bestX = -1;
        for (size_t i = 0; i < cands.size(); i++)
        {
            double px;
            double cost = TryPlace(rows[cands[i]], c, px);
            if (cost >= 0 && cost < best)
            {
                best = cost;
                bestRow = cands[i];
                bestX = px;
            }
        }

        // 兜底：候选行都放不下时，扫所有行（容量充足时几乎不会发生）
        if (bestRow < 0)
        {
            for (int i = 0; i < rowCnt; i++)
            {
                double px;
                double cost = TryPlace(rows[i], c, px);
                if (cost >= 0 && cost < best)
                {
                    best = cost;
                    bestRow = i;
                    bestX = px;
                }
            }
        }

        if (bestRow >= 0)
        {
            CommitPlace(rows[bestRow], c, bestX);
            placed++;
        }
        else
        {
            notPlacedCnt++;
            if (notPlacedCnt <= 5)
                printf("[诊断] 未放置: %s center=(%.0f, %.0f) size=(%.0f x %.0f)\n",
                       c->name.c_str(), c->center.x, c->center.y, c->width, c->height);
        }
    }
    printf("[统计]     未放置单元数=%d\n", notPlacedCnt);

    // ⑤ 合法化后统计
    overlapAfter = CountOverlaps();
    misplacedAfter = CountMisaligned();
    CalcDisplacement();

    printf("[合法化后] 重叠单元对=%lld   未对齐行单元数=%d (共 %d)\n",
           overlapAfter, misplacedAfter, (int)db->Nodes.size());
    printf("[位移]     总位移=%.0f   平均位移=%.2f   最大位移=%.0f   (已放置 %d 个单元)\n",
           totalDisp, avgDisp, maxDisp, placed);
    PrintRowInfo(); // 合法化后再打印，显示真实的行利用率
    printf("===================================\n\n");
}
