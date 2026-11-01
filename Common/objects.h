#ifndef OBJECTS_H
#define OBJECTS_H
#include "common.h"

// 前向声明
class Module;
class SiteRow;
class Pin;
class Net;
class PlaceData;

// 引脚方向常量：0=输出 1=输入 -1=未定义
#define PIN_OUTPUT 0
#define PIN_INPUT  1
#define PIN_UNDEF  -1

// ========== 布局单元（对应 .nodes 文件） ==========
// 分成两类：
//   Node     ：可以移动的单元（标准单元 Standard Cell 或宏单元 Macro Cell）
//   Terminal ：固定不动的终端（IO 引脚、电源等，也叫 Fixed）
class Module
{
public:
    Module() { Init(); }

    int idx;              // 单元全局编号
    string name;          // 单元名字
    float width;          // 宽
    float height;         // 高
    float area;           // 面积 = 宽 × 高
    float orientation;    // 朝向（0 = N 朝上，1 = S 翻转）
    bool isMacro;         // 是否为宏单元（大单元）
    bool isFixed;         // 是否固定（终端 / IO 引脚）
    bool isFiller;        // 是否为填充单元（任务 5 密度计算才用到）
    vector<Pin *> modulePins; // 该单元拥有的引脚列表
    vector<Net *> nets;       // 该单元参与的网络列表

    POS_2D origin;        // 单元左下角坐标（来自 .pl 文件）
    POS_2D center;        // 单元中心坐标 = origin + (width/2, height/2)

    void Init()
    {
        idx = -1;
        origin.SetZero();
        center.SetZero();
        width = 0;
        height = 0;
        area = 0;
        orientation = 0;
        isMacro = false;
        isFiller = false;
        isFixed = false;
    }
};

// ========== 布局行（对应 .scl 文件中的 CoreRow） ==========
// 芯片核心区被划分成若干行，标准单元最终要放进这些行里
class SiteRow
{
public:
    SiteRow()
    {
        bottom = 0;
        height = 0;
        step = 0;
        start.SetZero();
        end.SetZero();
    }

    double bottom;   // 行的底边 y 坐标
    double height;   // 行高
    double step;     // 站点步长（= 站点宽度）
    POS_2D start;    // 行的左下角坐标 (x = 起点, y = bottom)
    POS_2D end;      // 行的右下角坐标 (x = 起点 + 站点数, y = bottom)
};

// ========== 引脚（对应 .nets 文件中的每个引脚行） ==========
// 引脚属于某个单元，也属于某个网络
class Pin
{
public:
    Pin() { init(); }

    void init()
    {
        idx = -1;
        module = NULL;
        net = NULL;
        offset.SetZero();
        direction = PIN_UNDEF;
    }
    int idx;          // 引脚全局编号
    Module *module;   // 引脚所属的单元
    Net *net;         // 引脚所在的网络
    POS_2D offset;    // 引脚相对于单元中心的偏移
    int direction;    // 方向：0 输出 / 1 输入 / -1 未定义
};

// ========== 网络（对应 .nets 文件中的每个 NetDegree 块） ==========
// 超图模型中的"超边"：一条网络可以连接任意数量的引脚（不止两个）
class Net
{
public:
    Net() { init(); }
    int idx;               // 网络全局编号
    string name;           // 网络名字
    float weight;          // 网络权重（来自 .wts，默认 1）
    vector<Pin *> netPins; // 该网络包含的所有引脚

    void init()
    {
        idx = 0;
        name = "";
        weight = 1.0;
        netPins.clear();
    }
};

#endif
