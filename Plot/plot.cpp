#include "plot.h"

using namespace PLOTTING;

// 输出目录定义：主程序可通过 --out 覆盖（结束符统一带 "/"）
string PLOTTING::outputDir = "./output/";

/**
 * 用 CImg 画布局图，保存为 BMP 文件
 * 坐标系：数学坐标 y↑，屏幕坐标 y↓，画图时需要翻转 Y 轴
 * 注意：仅在 Linux/WSL 下可用（需要 X11）
 */
void PLOTTING::plotPlacement(const string& imageName, PlaceData* db)
{
#if USE_CIMG
    string plotPath = PLOTTING::outputDir;

    // ========== 1. 计算芯片区域边界 ==========
    float minX = 1e20, minY = 1e20;
    float maxX = -1e20, maxY = -1e20;

    // 遍历所有可移动单元
    for (Module* node : db->Nodes)
    {
        float llx = node->origin.x;
        float lly = node->origin.y;
        float urx = llx + node->width;
        float ury = lly + node->height;

        if (llx < minX) minX = llx;
        if (lly < minY) minY = lly;
        if (urx > maxX) maxX = urx;
        if (ury > maxY) maxY = ury;
    }

    // 遍历所有固定终端
    for (Module* term : db->Terminals)
    {
        float llx = term->origin.x;
        float lly = term->origin.y;
        float urx = llx + term->width;
        float ury = lly + term->height;

        if (llx < minX) minX = llx;
        if (lly < minY) minY = lly;
        if (urx > maxX) maxX = urx;
        if (ury > maxY) maxY = ury;
    }

    float chipWidth  = maxX - minX;
    float chipHeight = maxY - minY;

    printf("[绘图] 芯片区域: (%.0f, %.0f) ~ (%.0f, %.0f)\n", minX, minY, maxX, maxY);

    // ========== 2. 创建画布 ==========
    int imageWidth  = 800;  // 画布宽度（像素）
    int imageHeight = (int)(imageWidth * chipHeight / chipWidth);  // 等比例缩放

    // 限制最大高度
    if (imageHeight > 1200)
    {
        imageHeight = 1200;
        imageWidth  = (int)(imageHeight * chipWidth / chipHeight);
    }

    int xMargin = 20, yMargin = 20;
    CImg<unsigned char> img(imageWidth + 2 * xMargin, imageHeight + 2 * yMargin, 1, 3, 255);

    // 缩放因子：物理坐标 → 像素坐标
    float unitX = imageWidth / chipWidth;
    float unitY = imageHeight / chipHeight;

    // ========== 3. 画所有可移动单元 ==========
    int macroCount = 0, stdCellCount = 0;

    for (Module* node : db->Nodes)
    {
        // 物理坐标（左下角 + 右上角）
        float llx = node->origin.x;
        float lly = node->origin.y;
        float urx = llx + node->width;
        float ury = lly + node->height;

        // 转换为像素坐标（Y 轴翻转）
        int x1 = (int)((llx - minX) * unitX) + xMargin;
        int x2 = (int)((urx - minX) * unitX) + xMargin;
        int y1 = (int)((chipHeight - (ury - minY)) * unitY) + yMargin;  // 翻转：ury → y1
        int y2 = (int)((chipHeight - (lly - minY)) * unitY) + yMargin;  // 翻转：lly → y2

        // 画矩形（填充 + 边框）
        if (node->isMacro)
        {
            img.draw_rectangle(x1, y1, x2, y2, Green, 0.7f);
            img.draw_rectangle(x1, y1, x2, y2, Blue, 1.0f, ~0U);  // 边框
            macroCount++;
        }
        else
        {
            img.draw_rectangle(x1, y1, x2, y2, Red, 0.7f);
            stdCellCount++;
        }
    }

    // ========== 4. 画固定终端（灰色边框） ==========
    for (Module* term : db->Terminals)
    {
        float llx = term->origin.x;
        float lly = term->origin.y;
        float urx = llx + term->width;
        float ury = lly + term->height;

        int x1 = (int)((llx - minX) * unitX) + xMargin;
        int x2 = (int)((urx - minX) * unitX) + xMargin;
        int y1 = (int)((chipHeight - (ury - minY)) * unitY) + yMargin;
        int y2 = (int)((chipHeight - (lly - minY)) * unitY) + yMargin;

        // 固定终端用灰色边框，不填充
        unsigned char Gray[] = {128, 128, 128};
        img.draw_rectangle(x1, y1, x2, y2, Gray, 1.0f, ~0U);
    }

    // ========== 5. 添加文字标注 ==========
    char info[256];
    sprintf(info, "HPWL: Global Placement");
    img.draw_text(xMargin + 10, yMargin + 5, info, Black, NULL, 1.0f, 20);

    sprintf(info, "StdCells: %d  Macros: %d", stdCellCount, macroCount);
    img.draw_text(xMargin + 10, yMargin + 30, info, Black, NULL, 1.0f, 16);

    // ========== 6. 保存 BMP 文件 ==========
    string fullPath = plotPath + imageName + ".bmp";
    img.save_bmp(fullPath.c_str());

    printf("[绘图] 已保存: %s  (宽度=%d, 高度=%d)\n", fullPath.c_str(), imageWidth, imageHeight);
    printf("[绘图] 标准单元: %d 个, 宏单元: %d 个\n", stdCellCount, macroCount);

#else
    // Windows 下不支持 CImg
    printf("[绘图] CImg 仅支持 Linux/WSL，Windows 下请使用 GDSII 输出\n");
    printf("[绘图] 请在 WSL 中编译运行以生成 BMP 图片\n");
#endif
}
