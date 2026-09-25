#ifndef CSECTIONVALIDATION_H
#define CSECTIONVALIDATION_H

#include <QtGlobal>
#include <QVector>

namespace live2dstruct {
class CReader;
class CSectionOffsetTable;
class CCount_info;

class CSectionValidation
{
public:
    struct ColumnSpan
    {
        const char* name;
        qint64 offset;
        qint64 count;
        qint64 width = 4;
        bool optional = false;
    };

    static constexpr int OffsetTableStart = 0x40;
    static constexpr int OffsetTableCount = 137;
    static constexpr int OffsetTableEnd = OffsetTableStart + OffsetTableCount * 4;

    static bool ValidateMetadata(CReader& reader, const CSectionOffsetTable& offsets);
    static QVector<ColumnSpan> ArraySpans(const CSectionOffsetTable& offsets, const CCount_info& info);
    static bool ValidateArrays(CReader& reader, const CSectionOffsetTable& offsets, const CCount_info& info);
};
}

#endif
