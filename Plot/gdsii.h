#ifndef GDSII_H
#define GDSII_H

#include "common.h"
#include "placedata.h"
#include <cstdint>  // 用于 int16_t, int32_t, uint8_t 等类型

namespace PLOTTING
{
    /**
     * 输出 GDSII 格式文件
     * GDSII 是芯片版图的工业标准二进制格式
     * @param filename 输出文件名（含 .gds 扩展名）
     * @param db       布局数据
     */
    void writeGDSII(const string& filename, PlaceData* db);

    // ========== GDSII 内部工具函数（不对外暴露） ==========
    namespace GDSII_INTERNAL
    {
        // GDSII 记录类型
        enum RecordType
        {
            HEADER    = 0x00,
            BGNLIB    = 0x01,
            LIBNAME   = 0x02,
            UNITS     = 0x03,
            ENDLIB    = 0x04,
            BGNSTR    = 0x05,
            STRNAME   = 0x06,
            ENDSTR    = 0x07,
            BOUNDARY  = 0x08,
            PATH      = 0x09,
            SREF      = 0x0A,
            AREF      = 0x0B,
            TEXT      = 0x0C,
            LAYER     = 0x0D,
            DATATYPE  = 0x0E,
            WIDTH     = 0x0F,
            XY        = 0x10,
            ENDEL     = 0x11,
            SNAME     = 0x12,
            COLROW    = 0x13,
            TEXTNODE   = 0x14,
            NODE      = 0x15,
            TEXTTYPE  = 0x16,
            PRESENTATION = 0x17,
            STRING    = 0x19,
            STRANS    = 0x1A,
            MAG       = 0x1B,
            ANGLE     = 0x1C,
            PATHTYPE  = 0x21,
            GENERATIONS = 0x22,
            ATTRTABLE = 0x23,
            STYPTABLE = 0x24,
            STRTYPE   = 0x25,
            ELFLAGS   = 0x26,
            ELKEY     = 0x27,
            NODETYPE  = 0x2A,
            PROPATTR  = 0x2B,
            PROPVALUE = 0x2C,
            BOX       = 0x2D,
            BOXTYPE   = 0x2E
        };

        // 写入 2 字节整数（Big-Endian）
        void writeInt16(ofstream& file, int16_t value);

        // 写入 4 字节整数（Big-Endian）
        void writeInt32(ofstream& file, int32_t value);

        // 写入 8 字节浮点数（Big-Endian，GDSII Real_8 自定义格式，非 IEEE 754）
        void writeFloat64(ofstream& file, double value);

        // 写入记录头（长度 + 类型 + 数据类型）
        void writeRecordHeader(ofstream& file, uint16_t length, uint8_t recType, uint8_t dataType);

        // 写入短整数记录
        void writeShortRecord(ofstream& file, uint8_t recType, int16_t value);

        // 写入字符串记录
        void writeStringRecord(ofstream& file, uint8_t recType, const string& str);

        // 写入 XY 坐标记录
        void writeXYRecord(ofstream& file, const vector<pair<int32_t, int32_t>>& coords);

        // 获取当前时间（GDSII 格式）
        void getCurrentTime(int16_t& year, int16_t& month, int16_t& day,
                           int16_t& hour, int16_t& minute, int16_t& second);
    }
}

#endif // GDSII_H
