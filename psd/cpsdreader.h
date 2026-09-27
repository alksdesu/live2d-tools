#ifndef CPSDREADER_H
#define CPSDREADER_H

#include "cpsdsnapshot.h"

class CExportControl;

struct CPsdReadLimits
{
    qint64 maximumFileBytes = 512LL * 1024 * 1024;
    qint64 maximumDecodedBytes = 1024LL * 1024 * 1024;
    qint64 maximumLayerPixels = 64LL * 1024 * 1024;
    qint64 maximumMetadataBytes = 64LL * 1024 * 1024;
    int maximumLayers = 4096, maximumDepth = 128, maximumDimension = 30000;
};

class CPsdReader
{
public:
    static bool ReadBytes(const QByteArray& bytes, CPsdSnapshot& result, QString* error = nullptr,
                          CExportControl* control = nullptr, const CPsdReadLimits& limits = {});
    static bool ReadFile(const QString& path, CPsdSnapshot& result, QString* error = nullptr,
                         CExportControl* control = nullptr, const CPsdReadLimits& limits = {});
};

#endif
