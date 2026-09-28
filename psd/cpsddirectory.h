#ifndef CPSDDIRECTORY_H
#define CPSDDIRECTORY_H

#include "cpsdsnapshot.h"

struct CPsdDirectoryLayer
{
    int recordIndex = -1, parentIndex = -1;
    CPsdLayer::Kind kind = CPsdLayer::Kind::Raster;
    std::optional<quint32> layerId;
    QString name;
    QRect bounds;
    quint8 opacity = 255;
    QByteArray blendMode = "norm";
    bool visible = true, clipping = false, pixelEquivalent = true;
    QVector<CPsdAdditionalInfo> additionalInfo;
    QVector<CPsdDiagnostic> diagnostics;
};

struct CPsdDirectory
{
    QByteArray bytes, sha256, iccProfile;
    QString canonicalPath;
    QSize canvasSize;
    int version = 0, depth = 0, colorMode = 0, channels = 0;
    bool hasMergedTransparency = false, hasMergedPreview = false;
    QVector<CPsdDirectoryLayer> layers;
    QVector<int> displayOrder;
    QVector<CPsdDiagnostic> diagnostics;
};

#endif
