#ifndef CCMO3PROJECT_H
#define CCMO3PROJECT_H

#include "ccmo3namereader.h"
#include "psd/cpsdsourcedocument.h"
#include <QPointF>
#include <QTransform>
#include <memory>

namespace cmo3 {
struct CCmo3MaterialGraph
{
    struct LayerInput {
        QString documentGuid, layerGuid, layerName;
        int sourceIndex = -1;
        bool group = false;
        QTransform affine;
        QString clippingType;
        QStringList diagnostics;
    };
    struct Candidate {
        QString documentGuid;
        bool current = false;
        QVector<LayerInput> layers;
    };
    struct ModelImage {
        QString guid, name, groupId, currentDocumentGuid;
        QString filterId;
        QStringList filterTypes, filterGuids;
        QVector<Candidate> candidates;
        QTransform materialLocalToCanvas;
        QString imageResourcePath;
        QSize imageSize;
        QByteArray imageDigest;
        bool materialTransformPresent = false;
        bool directRaster = false;
        QStringList diagnostics;
    };
    struct TextureInput {
        QString identity, type, modelImageGuid;
        bool current = false;
        QTransform optionalTransformOnCanvas;
        QStringList diagnostics;
    };
    struct Mesh {
        QString id, guid, name, textureState;
        QVector<TextureInput> inputs;
        QVector<QPointF> materialPositions, materialUvs;
        QVector<int> indices;
        QStringList diagnostics;
    };
    QVector<ModelImage> images;
    QVector<Mesh> meshes;
    QStringList diagnostics;
};

class CCmo3Project
{
public:
    static bool OpenBytes(const QByteArray& bytes, const QString& sourcePath, CCmo3Project& result,
                          QString* error = nullptr, CExportControl* control = nullptr);
    static bool OpenFile(const QString& path, CCmo3Project& result,
                         QString* error = nullptr, CExportControl* control = nullptr);
    bool IsValid() const { return bool(m_data); }
    const QByteArray& Bytes() const;
    QString SourcePath() const;
    QByteArray SourceDigest() const;
    QString SourceVersion() const;
    const CPsdSourceCatalog* Artwork() const;
    const CCmo3NameReader::Catalog* Names() const;
    const CCmo3MaterialGraph* Materials() const;
    QString ArtworkError() const;
    QString NamesError() const;
    QString MaterialsError() const;

private:
    struct Data;
    static bool OpenOwned(QByteArray bytes, QString sourcePath, CCmo3Project& result,
                          QString* error, CExportControl* control);
    std::shared_ptr<const Data> m_data;
};
}

#endif
