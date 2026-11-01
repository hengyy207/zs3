#include "myplace.h"
#include <sstream>
#include <cstdio>

// 辅助函数：去掉行尾的 \r（防止数据文件用 Windows 换行导致字段尾巴带 \r）
static void TrimCR(string &s)
{
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

// ----------------------------------------------------------------
// ① 读 .aux 清单文件
//    文件内容就一行：RowBasedPlacement :  adaptec1.nodes  adaptec1.nets ...
//    冒号后依次是 nodes / nets / wts / pl / scl 五个文件名
// ----------------------------------------------------------------
void MyPlacer::ReadAux(const string &auxFile)
{
    ifstream fin(auxFile.c_str());
    if (!fin.is_open())
    {
        cerr << "[错误] 无法打开 aux 文件：" << auxFile << endl;
        exit(1);
    }

    string line;
    getline(fin, line);
    TrimCR(line);

    // 去掉 "RowBasedPlacement :" 前缀，剩下 5 个文件名
    stringstream ss(line);
    string token, colon;
    ss >> token; // RowBasedPlacement
    ss >> colon; // :

    vector<string> names;
    while (ss >> token) names.push_back(token);
    if (names.size() >= 5)
    {
        nodesFile = names[0]; // .nodes
        netsFile  = names[1]; // .nets
        wtsFile   = names[2]; // .wts
        plFile    = names[3]; // .pl
        sclFile   = names[4]; // .scl
    }
    else
    {
        cerr << "[错误] aux 文件格式不正确：" << auxFile << endl;
        exit(1);
    }
    fin.close();

    cout << "[1/6] 读取 aux：" << nodesFile << " | " << netsFile << " | "
         << wtsFile << " | " << plFile << " | " << sclFile << endl;
}

// ----------------------------------------------------------------
// ② 读 .scl 场地规划文件
//    NumRows : 890          ← 一共 890 行
//    CoreRow Horizontal     ← 行块开始
//      Coordinate    : 459  ← 行底 y 坐标
//      Height        : 12   ← 行高
//      Sitewidth     : 1    ← 站点宽度（= 步长）
//      SubrowOrigin : 459  NumSites : 10692   ← 起点 x 和站点数
//    End                    ← 行块结束
// ----------------------------------------------------------------
void MyPlacer::ReadScl(const string &basePath)
{
    ifstream fin((basePath + sclFile).c_str());
    if (!fin.is_open())
    {
        cerr << "[错误] 无法打开 scl 文件：" << basePath + sclFile << endl;
        exit(1);
    }

    string line;
    getline(fin, line); // 第一行：UCLA scl 1.0

    while (getline(fin, line))
    {
        TrimCR(line);
        if (line.empty() || line[0] == '#') continue;

        // 找到一个行块起点 "CoreRow Horizontal"
        if (line.find("CoreRow") != string::npos)
        {
            SiteRow row;
            while (getline(fin, line))
            {
                TrimCR(line);
                if (line.find("End") != string::npos) break; // 行块结束

                stringstream ss(line);
                string key, colon;
                ss >> key;
                if (key == "Coordinate")
                {
                    ss >> colon >> row.bottom; // 行底 y
                }
                else if (key == "Height")
                {
                    ss >> colon >> row.height; // 行高
                }
                else if (key == "Sitewidth" || key == "Sitespacing")
                {
                    ss >> colon >> row.step;   // 站点步长
                }
                else if (key == "SubrowOrigin")
                {
                    // SubrowOrigin : 459   NumSites : 10692
                    double numSites;
                    ss >> colon >> row.start.x; // 起点 x
                    ss >> colon;                // NumSites
                    ss >> colon >> numSites;    // : 10692
                    row.start.y = row.bottom;
                    row.end.x   = row.start.x + numSites; // 右端 x
                    row.end.y   = row.bottom;
                }
            }
            db->SiteRows.push_back(row);
        }
    }
    fin.close();

    cout << "[2/6] 读取 scl：共 " << db->SiteRows.size() << " 行" << endl;
}

// ----------------------------------------------------------------
// ③ 读 .nodes 单元清单文件
//    NumNodes : 211447
//    NumTerminals : 543
//    数据行：名字  宽  高   [terminal]
//      普通单元行尾没有关键字；终端行尾多一个 "terminal" 字段
//    注意：不要用缩进（空格/Tab）判断，因为不同数据文件缩进不统一
// ----------------------------------------------------------------
void MyPlacer::ReadNode(const string &basePath)
{
    ifstream fin((basePath + nodesFile).c_str());
    if (!fin.is_open())
    {
        cerr << "[错误] 无法打开 nodes 文件：" << basePath + nodesFile << endl;
        exit(1);
    }

    string line;
    getline(fin, line); // UCLA nodes 1.0

    while (getline(fin, line))
    {
        TrimCR(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.find("NumNodes") != string::npos) continue;
        if (line.find("NumTerminals") != string::npos) continue;

        // 解析：名字 宽 高 [terminal]
        stringstream ss(line);
        string name, extra;
        float w, h;
        ss >> name >> w >> h;
        ss >> extra; // 若有第 4 个字段，就是 "terminal"

        Module *m = new Module();
        m->name    = name;
        m->width   = w;
        m->height  = h;
        m->area    = w * h;
        m->isFixed = (extra == "terminal");
        m->idx     = (int)(db->Nodes.size() + db->Terminals.size());

        // 普通单元进 Nodes，固定终端进 Terminals
        if (m->isFixed) db->Terminals.push_back(m);
        else            db->Nodes.push_back(m);

        db->moduleMap[name] = m; // 名字 → 指针，供 pl/nets 快速查找
    }
    fin.close();

    db->moduleCount = (int)(db->Nodes.size() + db->Terminals.size());
    cout << "[3/6] 读取 nodes：可移动单元 " << db->Nodes.size()
         << " 个，终端 " << db->Terminals.size() << " 个" << endl;
}

// ----------------------------------------------------------------
// ④ 读 .pl 坐标文件
//    数据行：名字  x  y  :  N
//    给每个单元设置左下角坐标 origin，并算中心坐标 center
//    （布局算法通常用中心坐标计算）
// ----------------------------------------------------------------
void MyPlacer::ReadPl(const string &basePath)
{
    ifstream fin((basePath + plFile).c_str());
    if (!fin.is_open())
    {
        cerr << "[错误] 无法打开 pl 文件：" << basePath + plFile << endl;
        exit(1);
    }

    string line;
    getline(fin, line); // UCLA pl 1.0

    int cnt = 0;
    while (getline(fin, line))
    {
        TrimCR(line);
        if (line.empty() || line[0] == '#') continue;

        stringstream ss(line);
        string name, colon, orient;
        float x, y;
        ss >> name >> x >> y >> colon >> orient;

        map<string, Module *>::iterator it = db->moduleMap.find(name);
        if (it == db->moduleMap.end()) continue; // 找不到则跳过（防御）

        Module *m = it->second;
        m->origin.x = x;
        m->origin.y = y;
        // 中心坐标 = 左下角 + 半宽半高
        m->center.x = x + m->width / 2.0;
        m->center.y = y + m->height / 2.0;
        m->orientation = (orient == "S") ? 1 : 0; // 只区分 N / S
        cnt++;
    }
    fin.close();

    cout << "[4/6] 读取 pl：设置坐标的单元 " << cnt << " 个" << endl;
}

// ----------------------------------------------------------------
// ⑤ 读 .nets 网络文件（最关键的一步）
//    网络头行：NetDegree : 4   n0          ← 网络 n0 连了 4 个引脚
//    引脚行：  o197239  I : -0.500000  -6.000000
//             ↑单元名  ↑方向    ↑相对中心偏移x  ↑偏移y
//    要做的事：建立 网络→引脚→单元 三方关联，超图模型就此成型
// ----------------------------------------------------------------
void MyPlacer::ReadNet(const string &basePath)
{
    ifstream fin((basePath + netsFile).c_str());
    if (!fin.is_open())
    {
        cerr << "[错误] 无法打开 nets 文件：" << basePath + netsFile << endl;
        exit(1);
    }

    string line;
    getline(fin, line); // UCLA nets 1.0

    while (getline(fin, line))
    {
        TrimCR(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.find("NumNets") != string::npos) continue;
        if (line.find("NumPins") != string::npos) continue;

        if (line.find("NetDegree") != string::npos)
        {
            stringstream ss(line);
            string key, colon, netName;
            int degree;
            ss >> key >> colon >> degree >> netName; // NetDegree : 4 n0

            Net *net = new Net();
            net->idx  = (int)db->Nets.size();
            net->name = netName;
            db->netMap[netName] = net;

            // 读该网络下的 degree 个引脚
            for (int i = 0; i < degree; i++)
            {
                if (!getline(fin, line)) break;
                TrimCR(line);

                stringstream ps(line);
                string pname, tok;
                ps >> pname; // 引脚所属单元名
                ps >> tok;   // 方向（I / O）或 ":"

                Pin *pin = new Pin();
                pin->idx = (int)db->Pins.size();
                pin->net = net;

                if (tok == "I")      pin->direction = PIN_INPUT;
                else if (tok == "O") pin->direction = PIN_OUTPUT;
                else                 pin->direction = PIN_UNDEF;

                // 读取偏移：" : -0.5  -6.0" 或 " -0.5  -6.0"
                ps >> tok;
                if (tok == ":") ps >> pin->offset.x;
                else            pin->offset.x = (float)atof(tok.c_str());
                ps >> pin->offset.y;

                // 把引脚挂到所属单元上
                map<string, Module *>::iterator it = db->moduleMap.find(pname);
                if (it != db->moduleMap.end())
                {
                    pin->module = it->second;
                    it->second->modulePins.push_back(pin);
                    it->second->nets.push_back(net);
                }

                // 把引脚挂到网络上
                net->netPins.push_back(pin);
                db->Pins.push_back(pin);
            }

            db->Nets.push_back(net);
        }
    }
    fin.close();

    db->netCount = (int)db->Nets.size();
    db->pinCount = (int)db->Pins.size();
    cout << "[5/6] 读取 nets：网络 " << db->Nets.size()
         << " 个，引脚 " << db->Pins.size() << " 个" << endl;
}

// ----------------------------------------------------------------
// ⑥ 读 .wts 权重文件（可选）
//    adaptec1 的 wts 文件是空的（只有版本头和注释），
//    所以这里做兜底：没有数据行则所有网络权重保持默认 1
// ----------------------------------------------------------------
void MyPlacer::ReadWts(const string &basePath)
{
    ifstream fin((basePath + wtsFile).c_str());
    if (!fin.is_open())
    {
        cout << "[6/6] 无 wts 文件，全部网络权重默认为 1" << endl;
        return;
    }

    string line;
    getline(fin, line); // UCLA wts 1.0

    int cnt = 0;
    while (getline(fin, line))
    {
        TrimCR(line);
        if (line.empty() || line[0] == '#') continue;

        stringstream ss(line);
        string netName;
        float w;
        ss >> netName >> w;

        map<string, Net *>::iterator it = db->netMap.find(netName);
        if (it != db->netMap.end())
        {
            it->second->weight = w;
            cnt++;
        }
    }
    fin.close();
    cout << "[6/6] 读取 wts：设置权重 " << cnt << " 个网络" << endl;
}

// ----------------------------------------------------------------
// 总入口：按顺序解析 6 个文件
//    basePath 是 aux 所在的目录，其余 5 个文件都在同一目录
// ----------------------------------------------------------------
void MyPlacer::Run(const string &auxFile)
{
    size_t pos = auxFile.find_last_of('/');
    string basePath = (pos == string::npos) ? "" : auxFile.substr(0, pos + 1);

    ReadAux(auxFile);
    ReadScl(basePath);
    ReadNode(basePath);
    ReadPl(basePath);
    ReadNet(basePath);
    ReadWts(basePath);
}

// ----------------------------------------------------------------
// 统计信息输出：与任务描述 PDF 中 adaptec1 的参考数据逐项对照
// ----------------------------------------------------------------
void MyPlacer::PrintOverview()
{
    cout << "\n========== 布局数据总览 Overview ==========" << endl;

    // ① 核心区域：所有行的外接矩形
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (size_t i = 0; i < db->SiteRows.size(); i++)
    {
        SiteRow &r = db->SiteRows[i];
        minX = min(minX, (double)r.start.x);   // 注意 start.x/end.x 是 float，需转 double
        minY = min(minY, (double)r.start.y);
        maxX = max(maxX, (double)r.end.x);
        maxY = max(maxY, r.bottom + r.height);
    }
    double coreW = maxX - minX;
    double coreH = maxY - minY;
    double coreArea = coreW * coreH;

    // ② 面积统计
    double movableArea = 0, fixedArea = 0, fixedAreaInCore = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++)
        movableArea += db->Nodes[i]->area;
    for (size_t i = 0; i < db->Terminals.size(); i++)
    {
        Module *t = db->Terminals[i];
        fixedArea += t->area;
        // 只有"中心落在核心区内"的终端面积才算 Fixed Area in Core
        if (t->center.x >= minX && t->center.x <= maxX &&
            t->center.y >= minY && t->center.y <= maxY)
            fixedAreaInCore += t->area;
    }

    // ③ 网络统计：最大网络度（一条网络最多连几个引脚）
    int maxDegree = 0;
    for (size_t i = 0; i < db->Nets.size(); i++)
    {
        int d = (int)db->Nets[i]->netPins.size();
        if (d > maxDegree) maxDegree = d;
    }

    // ④ 宏单元数量（本数据为 0）
    int macroCount = 0;
    for (size_t i = 0; i < db->Nodes.size(); i++)
        if (db->Nodes[i]->isMacro) macroCount++;

    // ⑤ 逐项输出
    printf("Core region   : lower left (%.0f, %.0f)  to  upper right (%.0f, %.0f)\n",
           minX, minY, maxX, maxY);
    printf("Row Height/Num: %.0f / %d  (site step %.6f)\n",
           db->SiteRows[0].height, (int)db->SiteRows.size(), db->SiteRows[0].step);
    printf("Core Area     : %.0f\n", coreArea);
    printf("Cell Area     : %.0f (%.2f%%)\n", movableArea, movableArea / coreArea * 100);
    printf("Movable Area  : %.0f (%.2f%%)\n", movableArea, movableArea / coreArea * 100);
    printf("Fixed Area    : %.0f (%.2f%%)\n", fixedArea, fixedArea / coreArea * 100);
    printf("Fixed Area in Core: %.0f (%.2f%%)\n",
           fixedAreaInCore, fixedAreaInCore / coreArea * 100);
    printf("Placement Util.: %.2f%% (=movable/(core-fixedInCore))\n",
           movableArea / (coreArea - fixedAreaInCore) * 100);
    printf("Core Density  : %.2f%% (=usedArea/core)\n",
           (movableArea + fixedAreaInCore) / coreArea * 100);
    printf("Cell #        : %d  (=%dk)\n",
           (int)db->Nodes.size(), (int)(db->Nodes.size() / 1000));
    printf("Object #      : %d  (fixed: %d) (macro: %d)\n",
           db->moduleCount, (int)db->Terminals.size(), macroCount);
    printf("Net #         : %d  (=%dk)\n",
           (int)db->Nets.size(), (int)(db->Nets.size() / 1000));
    printf("Max net degree: %d\n", maxDegree);
    printf("Pin #         : %d\n", (int)db->Pins.size());

    // ⑥ 网络度分布统计（对照参考输出的 "There are ... nets (has ... pins)" 五行）
    //    按每条网络连接的引脚数（即网络度）分档：1 / 2 / 3-10 / 11-100 / >100
    //    自检：五档数量之和应等于网络总数（adaptec1: 1348+117104+87085+15603+2 =   221142）
    {
        int cnt1 = 0, cnt2 = 0, cnt3to10 = 0, cnt11to100 = 0, cntOver100 = 0;
        for (size_t i = 0; i < db->Nets.size(); i++)
        {
            int d = (int)db->Nets[i]->netPins.size(); // 该网络实际连接的引脚数
            if (d == 1)                cnt1++;        // 单引脚网络（悬空/占位）
            else if (d == 2)           cnt2++;        // 两引脚网络（点对点连线）
            else if (d >= 3 && d <= 10)  cnt3to10++;  // 小型网络
            else if (d >= 11 && d <= 100) cnt11to100++; // 中型网络
            else if (d > 100)          cntOver100++;  // 大型网络（时钟/复位等高扇出）
        }
        printf("There are %d nets (has 1 pins)\n", cnt1);
        printf("There are %d nets (has 2 pins)\n", cnt2);
        printf("There are %d nets (has 3-10 pins)\n", cnt3to10);
        printf("There are %d nets (has 11-100 pins)\n", cnt11to100);
        printf("There are %d nets (has >100 pins)\n", cntOver100);
    }

    cout << "==================================================" << endl;
}
