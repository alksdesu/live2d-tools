#ifndef CPSDSOURCEDOCUMENT_H
#define CPSDSOURCEDOCUMENT_H

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>
#include <variant>
#include <utility>
#include "cpsdoriginal.h"

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
    struct Origin {
        QString sourcePath;
        QByteArray sourceDigest;
        QString familyId, familyName, revisionId, fileName;
        bool currentRevision = false;
        int linkedMeshes = 0, detachedMeshes = 0;
    };
    struct Document {
        std::variant<CPsdSourceDocument, CPsdOriginal> content;
        Origin origin;
        QVector<CPsdSourceDiagnostic> diagnostics;
        Document() = default;
        Document(CPsdSourceDocument document) : content(std::move(document)) {}
        Document(CPsdOriginal document, Origin source) : content(std::move(document)), origin(std::move(source)) {}
        const CPsdSourceDocument* Reconstructed() const { return std::get_if<CPsdSourceDocument>(&content); }
        const CPsdOriginal* Original() const { return std::get_if<CPsdOriginal>(&content); }
    };
    QStringList protectedPaths;
    QVector<Document> documents;
    QVector<CPsdSourceDiagnostic> diagnostics;
};

#endif
