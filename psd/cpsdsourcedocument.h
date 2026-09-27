#ifndef CPSDSOURCEDOCUMENT_H
#define CPSDSOURCEDOCUMENT_H

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QVector>
#include <optional>

struct CPsdSourceDiagnostic
{
    enum class Severity { Information, Warning, Error };
    Severity severity = Severity::Warning;
    QString code, message, layerId;
};

struct CPsdSourceLayer
{
    enum class Kind { Raster, Group };
    Kind kind = Kind::Raster;
    QString id, name;
    int parentIndex = -1;
    QRect bounds;
    QByteArray png;
    bool visible = true, clipping = false;
    quint8 opacity = 255;
    QByteArray blendMode = "norm";
    std::optional<quint32> externalId;
};

struct CPsdSourceDocument
{
    QString id, name;
    QSize canvasSize;
    QByteArray iccProfile;
    QVector<CPsdSourceLayer> layers;
    QVector<CPsdSourceDiagnostic> diagnostics;
};

struct CPsdSourceCatalog
{
    QString sourcePath;
    QByteArray sourceDigest;
    QString sourceVersion;
    QVector<CPsdSourceDocument> documents;
    QVector<CPsdSourceDiagnostic> diagnostics;
};

#endif
