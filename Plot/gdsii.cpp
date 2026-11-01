#include "gdsii.h"
#include <ctime>

using namespace PLOTTING;
using namespace GDSII_INTERNAL;

// ========== 写入 2 字节整数（Big-Endian） ==========
void GDSII_INTERNAL::writeInt16(ofstream& file, int16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (value >> 8) & 0xFF;  // 高字节在前
    bytes[1] = value & 0xFF;         // 低字节在后
    file.write(reinterpret_cast<char*>(bytes), 2);
}

// ========== 写入 4 字节整数（Big-Endian） ==========
void GDSII_INTERNAL::writeInt32(ofstream& file, int32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (value >> 24) & 0xFF;
    bytes[1] = (value >> 16) & 0xFF;
    bytes[2] = (value >> 8) & 0xFF;
    bytes[3] = value & 0xFF;
    file.write(reinterpret_cast<char*>(bytes), 4);
}

// ========== 写入 8 字节浮点数（Big-Endian，GDSII 专用 Real_8 格式） ==========
// 【重要】GDSII 的 8 字节实数（Real_8）不是 IEEE 754！格式为：
//   bit63    : 符号位（1 = 负）
//   bit62~56 : 指数（excess-64 偏移，基 16）
//   bit55~0  : 尾数（56 位，值 = 尾数 / 2^56）
//   数值 = (-1)^符号 × 16^(指数-64) × (尾数 / 2^56)
// 若用 IEEE 754 的 double 直接写入，KLayout 会解析成完全错误的值。
void GDSII_INTERNAL::writeFloat64(ofstream& file, double value)
{
    uint64_t bits = 0; // 拼好的 64 位结果

    if (value < 0)
    {
        bits |= (1ULL << 63); // 符号位
        value = -value;
    }

    if (value != 0.0)
    {
        // 规范化：把 value 调整到 [1/16, 1)，指数从 64 开始计数
        // 注意顺序：先除后乘。若先乘后除，value 会停在 >=1（如 1e-6 → 1.048576），
        // 尾数溢出被截断，编码结果错误（曾实测解码出 4.6e-8 而非 1e-6）。
        int exp = 64;
        while (value >= 1.0)     { value /= 16.0; exp++; } // 先缩小到 <1
        while (value < 1.0/16.0) { value *= 16.0; exp--; } // 再放大到 >=1/16
        if (exp < 0)  exp = 0;
        if (exp > 127) exp = 127;

        // 尾数 = value × 2^56（取 56 位）
        uint64_t mant = (uint64_t)(value * (double)(1ULL << 56));
        bits |= ((uint64_t)(exp & 0x7F)) << 56;
        bits |= mant & 0xFFFFFFFFFFFFFFULL;
    }

    // Big-Endian 写出（GDSII 全程大端字节序）
    for (int i = 0; i < 8; i++)
        file.put((char)((bits >> (56 - 8 * i)) & 0xFF));
}

// ========== 写入记录头（长度 + 类型 + 数据类型） ==========
// GDSII 记录格式：[2字节长度] [1字节类型] [1字节数据类型] [数据...]
// 长度 = 4 + 数据长度
void GDSII_INTERNAL::writeRecordHeader(ofstream& file, uint16_t length, uint8_t recType, uint8_t dataType)
{
    writeInt16(file, length);
    file.put(recType);
    file.put(dataType);
}

// ========== 写入短整数记录 ==========
// LAYER / DATATYPE / WIDTH 等记录：数据类型必须是 0x02（2字节整数）。
// 原代码误用 0x01（位数组），KLayout 解析 LAYER/DATATYPE 时会直接报格式错误。
void GDSII_INTERNAL::writeShortRecord(ofstream& file, uint8_t recType, int16_t value)
{
    writeRecordHeader(file, 6, recType, 0x02);  // 0x02 = 2字节整数
    writeInt16(file, value);
}

// ========== 写入字符串记录 ==========
void GDSII_INTERNAL::writeStringRecord(ofstream& file, uint8_t recType, const string& str)
{
    uint16_t length = 4 + str.length();
    if (length % 2 != 0) length++;  // 偶数对齐
    writeRecordHeader(file, length, recType, 0x06);  // 0x06 = 字符串
    file.write(str.c_str(), str.length());
    if (str.length() % 2 != 0) file.put(0);  // 填充字节
}

// ========== 写入 XY 坐标记录 ==========
void GDSII_INTERNAL::writeXYRecord(ofstream& file, const vector<pair<int32_t, int32_t>>& coords)
{
    uint16_t length = 4 + coords.size() * 8;  // 每个坐标 8 字节
    writeRecordHeader(file, length, XY, 0x03);  // 0x03 = 4字节整数
    for (auto& coord : coords)
    {
        writeInt32(file, coord.first);   // x
        writeInt32(file, coord.second);  // y
    }
}

// ========== 获取当前时间（GDSII 格式） ==========
void GDSII_INTERNAL::getCurrentTime(int16_t& year, int16_t& month, int16_t& day,
                                   int16_t& hour, int16_t& minute, int16_t& second)
{
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);

    year   = t->tm_year + 1900;
    month  = t->tm_mon + 1;
    day    = t->tm_mday;
    hour   = t->tm_hour;
    minute = t->tm_min;
    second = t->tm_sec;
}

// ========== 输出 GDSII 格式文件 ==========
void PLOTTING::writeGDSII(const string& filename, PlaceData* db)
{
    ofstream file(filename, ios::binary);
    if (!file.is_open())
    {
        printf("[GDSII] 错误：无法创建文件 %s\n", filename.c_str());
        return;
    }

    // 数据库单位：1nm = 1e-9
    const double dbUnits = 1e-9;
    const double userUnits = 1e-6;  // 1um

    // ========== 1. HEADER（版本号） ==========
    writeRecordHeader(file, 6, HEADER, 0x02);  // 0x02 = 2字节整数
    writeInt16(file, 600);  // GDSII 版本 600

    // ========== 2. BGNLIB（库创建/修改时间） ==========
    writeRecordHeader(file, 28, BGNLIB, 0x02);
    int16_t year, month, day, hour, minute, second;
    getCurrentTime(year, month, day, hour, minute, second);
    // 最后修改时间
    writeInt16(file, year); writeInt16(file, month); writeInt16(file, day);
    writeInt16(file, hour); writeInt16(file, minute); writeInt16(file, second);
    // 最后访问时间
    writeInt16(file, year); writeInt16(file, month); writeInt16(file, day);
    writeInt16(file, hour); writeInt16(file, minute); writeInt16(file, second);

    // ========== 3. LIBRARY（库名） ==========
    writeStringRecord(file, LIBNAME, "MyPlacement");

    // ========== 4. UNITS（单位） ==========
    // GDSII 规范：第一个值是用户单位（1个用户单位等于多少米）
    //            第二个值是数据库单位（1个数据库单位等于多少米）
    // 数据类型必须是 0x05（8字节实数），原代码误用 0x03（4字节整数）
    writeRecordHeader(file, 20, UNITS, 0x05);  // 0x05 = 8字节实数
    writeFloat64(file, userUnits);             // 用户单位 = 1um = 1e-6
    writeFloat64(file, dbUnits);               // 数据库单位 = 1nm = 1e-9

    // ========== 5. BGNSTR（单元开始） ==========
    writeRecordHeader(file, 28, BGNSTR, 0x02);
    getCurrentTime(year, month, day, hour, minute, second);
    writeInt16(file, year); writeInt16(file, month); writeInt16(file, day);
    writeInt16(file, hour); writeInt16(file, minute); writeInt16(file, second);
    writeInt16(file, year); writeInt16(file, month); writeInt16(file, day);
    writeInt16(file, hour); writeInt16(file, minute); writeInt16(file, second);

    // ========== 6. STRNAME（单元名） ==========
    writeStringRecord(file, STRNAME, "TOP");

    // ========== 7. 画所有可移动单元 ==========
    int macroCount = 0, stdCellCount = 0;

    // 缩放因子：坐标单位是微米，GDSII单位是纳米
    // 为了避免 int32 溢出，使用 1000 的缩放因子（微米→纳米）
    const double scale = 1000.0;

    // 调试：打印第一个单元的坐标
    if (!db->Nodes.empty())
    {
        Module* first = db->Nodes[0];
        printf("[GDSII 调试] 第一个单元: origin=(%.2f, %.2f), size=(%.2f, %.2f)\n",
               first->origin.x, first->origin.y, first->width, first->height);
        printf("[GDSII 调试] 缩放后: llx=%d, lly=%d, urx=%d, ury=%d\n",
               (int32_t)(first->origin.x * scale), (int32_t)(first->origin.y * scale),
               (int32_t)((first->origin.x + first->width) * scale),
               (int32_t)((first->origin.y + first->height) * scale));
    }

    for (Module* node : db->Nodes)
    {
        // 计算四个角点坐标（单位：nm）
        int32_t llx = (int32_t)(node->origin.x * scale);
        int32_t lly = (int32_t)(node->origin.y * scale);
        int32_t urx = (int32_t)((node->origin.x + node->width) * scale);
        int32_t ury = (int32_t)((node->origin.y + node->height) * scale);

        // BOUNDARY 记录
        writeRecordHeader(file, 4, BOUNDARY, 0x00);  // 0x00 = 无数据

        // LAYER
        if (node->isMacro)
        {
            writeShortRecord(file, LAYER, 1);      // 宏单元用 Layer 1
            macroCount++;
        }
        else
        {
            writeShortRecord(file, LAYER, 0);      // 标准单元用 Layer 0
            stdCellCount++;
        }

        // DATATYPE
        writeShortRecord(file, DATATYPE, 0);

        // XY（四个角点，闭合：首尾相同）
        vector<pair<int32_t, int32_t>> coords = {
            {llx, lly},  // 左下
            {urx, lly},  // 右下
            {urx, ury},  // 右上
            {llx, ury},  // 左上
            {llx, lly}   // 闭合
        };
        writeXYRecord(file, coords);

        // ENDEL
        writeRecordHeader(file, 4, ENDEL, 0x00);
    }

    // ========== 8. 画固定终端（可选，灰色边框） ==========
    for (Module* term : db->Terminals)
    {
        int32_t llx = (int32_t)(term->origin.x * scale);
        int32_t lly = (int32_t)(term->origin.y * scale);
        int32_t urx = (int32_t)((term->origin.x + term->width) * scale);
        int32_t ury = (int32_t)((term->origin.y + term->height) * scale);

        writeRecordHeader(file, 4, BOUNDARY, 0x00);
        writeShortRecord(file, LAYER, 2);          // 固定终端用 Layer 2
        writeShortRecord(file, DATATYPE, 0);

        vector<pair<int32_t, int32_t>> coords = {
            {llx, lly}, {urx, lly}, {urx, ury}, {llx, ury}, {llx, lly}
        };
        writeXYRecord(file, coords);

        writeRecordHeader(file, 4, ENDEL, 0x00);
    }

    // ========== 9. ENDSTR（单元结束） ==========
    writeRecordHeader(file, 4, ENDSTR, 0x00);

    // ========== 10. ENDLIB（库结束） ==========
    writeRecordHeader(file, 4, ENDLIB, 0x00);

    file.close();

    printf("[GDSII] 已保存: %s\n", filename.c_str());
    printf("[GDSII] 标准单元: %d 个 (Layer 0), 宏单元: %d 个 (Layer 1)\n", stdCellCount, macroCount);
    printf("[GDSII] 用 KLayout 打开查看：https://www.klayout.de/\n");
}
