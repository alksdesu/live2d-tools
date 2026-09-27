#ifndef CPSDSNAPSHOT_H
#define CPSDSNAPSHOT_H

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <optional>

struct CPsdDiagnostic { QString code, message; };
struct CPsdAdditionalInfo { QByteArray key, data; };

struct CPsdLayer
{
    enum class Kind { Raster, Group, GroupEnd };
    int recordIndex = -1, parentIndex = -1;
    Kind kind = Kind::Raster;
    std::optional<quint32> layerId;
    QString name;
    QRect bounds;
    quint8 opacity = 255;
    QByteArray blendMode = "norm";
    bool visible = true, clipping = false, pixelEquivalent = true;
    QImage image;
    QVector<CPsdAdditionalInfo> additionalInfo;
    QVector<CPsdDiagnostic> diagnostics;
};

struct CPsdSnapshot
{
    QByteArray bytes, sha256, iccProfile;
    QString canonicalPath;
    QSize canvasSize;
    int version = 0, depth = 0, colorMode = 0, channels = 0;
    bool hasMergedTransparency = false;
    QImage mergedImage;
    QVector<CPsdLayer> layers;
    QVector<int> displayOrder;
    QVector<CPsdDiagnostic> diagnostics;
};

#endif
