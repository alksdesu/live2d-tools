#ifndef CSECTIONVALIDATION_H
#define CSECTIONVALIDATION_H

namespace live2dstruct {
class CReader;
class CSectionOffsetTable;
class CCount_info;

class CSectionValidation
{
public:
    static constexpr int OffsetTableStart = 0x40;
    static constexpr int OffsetTableCount = 137;
    static constexpr int OffsetTableEnd = OffsetTableStart + OffsetTableCount * 4;

    static bool ValidateMetadata(CReader& reader, const CSectionOffsetTable& offsets);
    static bool ValidateArrays(CReader& reader, const CSectionOffsetTable& offsets, const CCount_info& info);
};
}

#endif
