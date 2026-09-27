#ifndef CMODELSESSION_H
#define CMODELSESSION_H

#include <QMap>
#include <QJsonObject>
#include <QHash>
#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QVector>
#include <optional>
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cparameterresourceedits.h"
#include "live2dstruct/ctextureassets.h"

namespace live2dstruct {

struct CModelSession
{
    struct PhysicsDraft {
        QString sourcePath;
        QJsonObject document;
        bool dirty = false;
    };
    int version = 1;
    QJsonObject animation;
    QString sourceMoc;
    QByteArray sourceDigest;
    CMocParameterStructureBuilder::ColumnState structure;
    bool meshStructure = false;
    bool materialStructure = false;
    CModelMaterialOverrides drawableMaterials;
    bool deformerStructure = false;
    QSet<QString> sourceDeformerIds;
    CParameterResourceEdits::State parameterResources;
    std::optional<PhysicsDraft> physicsDraft;
    CTextureAssets::Draft textures;
    std::optional<CTextureWorkspace::Draft> textureWorkspace;
    CModelArtwork::State artwork;
    QMap<QString, float> parameters;
    QHash<QString, CModelParameterMetadata> parameterMetadata;
    std::optional<CDeformerDisplayNames::State> deformerDisplayNames;
    CParameterOrganization::State parameterOrganization;
    CGeometryObjectPairs::State geometryObjectPairs;
    QMap<QString, bool> visibility;
    QMap<QString, bool> locked;
    QHash<QString, float> drawableOpacity;
    QHash<QString, int> drawableDrawOrder;
    QHash<QString, QVector<QVector<float>>> drawableKeyformPositions;
    QHash<QString, QVector<quint16>> drawableIndices;
    QHash<QString, QVector<int>> drawableMaskIndices;
    QHash<QString, int> deformerParentIndices;
    QHash<QString, QVector<CModelRotationKeyform>> deformerRotationKeyforms;
    QHash<QString, QVector<CModelWarpKeyform>> deformerWarpKeyforms;
    QHash<QString, bool> drawableMaskInverted;
    float viewScale = 1.0f;
    QPointF viewPosition;

    QByteArray ToJson() const;
    static bool FromJson(const QByteArray& data, CModelSession* session, QString* error = nullptr);
    bool RestoreDocument(const CModelDocument& loaded, CModelDocument& result, QString* error = nullptr) const;
    static QByteArray ComputeSourceDigest(const QString& path);
    bool Save(const QString& path, QString* error = nullptr) const;
    static bool Load(const QString& path, CModelSession* session, QString* error = nullptr);
private:
    friend class CModelDocument;
    bool RestorePreparedDocument(const CModelDocument& loaded, const CModelDocument& prepared,
                                 CModelDocument& result, QString* error) const;
    bool RestoreDocumentImpl(const CModelDocument& loaded, const CModelDocument* prepared,
                             CModelDocument& result, QString* error) const;
};

}

#endif
