#ifndef CPSDREADER_H
#define CPSDREADER_H

#include "cpsdsnapshot.h"
#include "cpsddirectory.h"
#include "cpsdreaddocument.h"

class CExportControl;

struct CPsdReadLimits
{
    qint64 maximumFileBytes = 512LL * 1024 * 1024;
    qint64 maximumDecodedBytes = 1024LL * 1024 * 1024;
    qint64 maximumLayerPixels = 64LL * 1024 * 1024;
    qint64 maximumMetadataBytes = 64LL * 1024 * 1024;
    int maximumLayers = 4096, maximumDepth = 128, maximumDimension = 30000;
    static CPsdReadLimits Directory();
};

struct CPsdStreamLimits
{
    CPsdReadLimits document = CPsdReadLimits::Directory();
    qint64 maximumLayerWorkingBytes = 512LL * 1024 * 1024;
};

class CPsdReader
{
public:
    static bool ReadBytes(const QByteArray& bytes, CPsdSnapshot& result, QString* error = nullptr,
                          CExportControl* control = nullptr, const CPsdReadLimits& limits = {});
    static bool ReadFile(const QString& path, CPsdSnapshot& result, QString* error = nullptr,
                         CExportControl* control = nullptr, const CPsdReadLimits& limits = {});
    static bool ReadDirectoryBytes(const QByteArray& bytes, CPsdDirectory& result, QString* error = nullptr,
                                   CExportControl* control = nullptr, const CPsdReadLimits& limits = CPsdReadLimits::Directory());
    static bool OpenBytes(const QByteArray& bytes, CPsdReadDocument& result, QString* error = nullptr,
                          CExportControl* control = nullptr, const CPsdStreamLimits& limits = {});
    static bool OpenFile(const QString& path, CPsdReadDocument& result, QString* error = nullptr,
                         CExportControl* control = nullptr, const CPsdStreamLimits& limits = {});

private:
    static bool OpenOwned(QByteArray bytes, QString canonicalPath, CPsdReadDocument& result, QString* error,
                          CExportControl& control, const CPsdStreamLimits& limits);
};

#endif
