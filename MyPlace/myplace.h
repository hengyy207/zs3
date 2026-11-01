#ifndef MYPLACE_H
#define MYPLACE_H

#include "placedata.h"
#include "common.h"

// ========== 布局主类：负责"读文件 + 组织数据 + 统计输出" ==========
// 任务 3 先只做解析；任务 4/5 的布局算法也都会挂在这个类上
class MyPlacer
{
public:
    MyPlacer(PlaceData *_db) { db = _db; }
    PlaceData *db;

    // 5 个数据文件名（从 .aux 文件解析得到）
    string nodesFile, netsFile, wtsFile, plFile, sclFile;

    // ---------- 解析函数（实现见 myplace.cpp） ----------
    void ReadAux(const string &auxFile);   // ① 读清单，得到其余 5 个文件名
    void ReadScl(const string &basePath);  // ② 读场地规划 → SiteRows
    void ReadNode(const string &basePath); // ③ 读单元清单 → Nodes / Terminals
    void ReadPl(const string &basePath);   // ④ 读坐标 → origin / center
    void ReadNet(const string &basePath);  // ⑤ 读网络 → Nets / Pins / 关联
    void ReadWts(const string &basePath);  // ⑥ 读权重（可选，默认 1）

    // ---------- 总入口 ----------
    void Run(const string &auxFile);

    // ---------- 统计输出（对照任务描述 PDF 的参考数据） ----------
    void PrintOverview();
};
#endif
