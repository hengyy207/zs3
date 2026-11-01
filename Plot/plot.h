#ifndef PLOT_H
#define PLOT_H

#include "common.h"
#include "placedata.h"

// CImg 图像库（仅在 Linux/WSL 下使用，Windows 下有兼容性问题）
#ifndef _WIN32
    #define USE_CIMG 1
    #include "CImg.h"
    using namespace cimg_library;
#else
    #define USE_CIMG 0
#endif

namespace PLOTTING
{
    // 输出目录（结尾带 "/"），默认 "./output/"。
    // 主程序按 --out 参数覆盖，供 Web 端为每个请求分配独立目录、避免并发覆盖。
    extern string outputDir;

    // 颜色定义（RGB）
    const unsigned char Black[] = {0, 0, 0};
    const unsigned char Green[] = {0, 150, 0};      // 宏单元
    const unsigned char Red[]   = {255, 0, 0};       // 标准单元
    const unsigned char Blue[]  = {0, 0, 255};       // 边框
    const unsigned char White[] = {255, 255, 255};    // 背景

    /**
     * 用 CImg 画布局图，保存为 BMP 文件
     * @param imageName 输出文件名（不含扩展名）
     * @param db        布局数据
     */
    void plotPlacement(const string& imageName, PlaceData* db);

    /**
     * 输出 GDSII 格式文件
     * @param filename 输出文件名（含 .gds 扩展名）
     * @param db       布局数据
     */
    void writeGDSII(const string& filename, PlaceData* db);
}

#endif // PLOT_H
