#ifndef CMATERIALRESOURCEBUDGET_H
#define CMATERIALRESOURCEBUDGET_H

#include "cmodelatlasedit.h"

namespace live2dstruct {
class CMaterialResourceBudget
{
public:
    struct Limits {
        qint64 maxStorageBytes = CTextureAssets::MaximumBytes;
        qint64 maxDecodedBytes = CTextureWorkspace::MaximumDecodedBytes;
    };
    struct Usage {
        // 元数据按不同完整状态保守核算，不代表进程驻留内存。
        qint64 encodedBytes = 0, metadataBytes = 0, decodedBytes = 0;
        int uniqueBlobs = 0, uniqueImages = 0;
        qint64 StorageBytes() const { return encodedBytes + metadataBytes; }
    };
    static bool Validate(const CTextureAssets::Catalog& sources, const QVector<CModelAtlasState>& snapshots,
                         const Limits& limits, Usage* usage = nullptr, QString* error = nullptr);
    static bool Validate(const CTextureAssets::Catalog& sources, const QVector<CModelAtlasState>& snapshots,
                         Usage* usage = nullptr, QString* error = nullptr);
};
}

#endif
