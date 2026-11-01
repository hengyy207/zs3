#ifndef PLACEDATA_H
#define PLACEDATA_H
#include "objects.h"

// ========== 布局数据仓库：解析完的所有数据的"总管家" ==========
// 后面任务 4/5/6 的算法都直接从这个类里取数据
class PlaceData
{
public:
    int moduleCount; // 单元总数（Node + Terminal）
    int MacroCount;  // 宏单元数量
    int netCount;    // 网络总数
    int pinCount;    // 引脚总数

    vector<Module *> Nodes;      // 可移动单元列表（Node）
    vector<Module *> Terminals;  // 固定终端列表（Terminal）
    vector<Pin *> Pins;          // 所有引脚
    vector<Net *> Nets;          // 所有网络
    vector<SiteRow> SiteRows;    // 所有行

    map<string, Module *> moduleMap; // 名字 → 单元指针（解析时快速查找）
    map<string, Net *>    netMap;    // 名字 → 网络指针
};
#endif
