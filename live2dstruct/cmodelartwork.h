#ifndef CMODELARTWORK_H
#define CMODELARTWORK_H

#include "cmodelmaterialedit.h"
#include <QJsonObject>
#include <QMetaType>
#include <QRectF>
#include <QStringList>
#include <QTransform>
#include <optional>
#include <memory>

namespace live2dstruct {
class CMaterialValidationCache;
class CModelArtwork
{
public:
    enum class SourceKind { Psd, Png, AtlasSnapshot, Cmo3 };
    enum class LayerKind { Raster, Group };
    enum class Boundary { Transparent, Repeat };
    enum class ColorPolicy { PreserveValues, ConvertToSRgb };
    enum class Filter { ExactTexels, LinearLightPremultiplied };
    enum class BindingStatus { Linked, Detached };

    struct Image {
        QSize size;
        QByteArray png;
        bool operator==(const Image& other) const;
    };
    struct Family {
        QString id, name;
        SourceKind kind = SourceKind::Psd;
        QString currentRevision;
        bool operator==(const Family& other) const;
    };
    struct Layer {
        QString id, name;
        int parentIndex = -1;
        int sourceIndex = -1;
        LayerKind kind = LayerKind::Raster;
        std::optional<quint32> externalId;
        QRect bounds;
        bool visible = true, clipping = false, pixelEquivalent = true;
        quint8 opacity = 255;
        QByteArray blendMode = "norm";
        QByteArray imageDigest;
        QStringList diagnostics;
        bool operator==(const Layer& other) const;
    };
    struct Revision {
        QString id, familyId, path, fileName;
        QByteArray sourceDigest, sourceBytes, iccProfile;
        QSize canvasSize;
        ColorPolicy colorPolicy = ColorPolicy::PreserveValues;
        QTransform documentToFamily;
        QVector<Layer> layers;
        QString sourceDocumentId;
        bool operator==(const Revision& other) const;
    };
    struct Material {
        QString id, name, familyId, revisionId, layerId;
        Boundary boundary = Boundary::Transparent;
        bool applyLayerOpacity = true;
        QString missingInRevision;
        bool operator==(const Material& other) const;
    };
    struct Placement {
        QString id, materialId, atlasId;
        QTransform familyToAtlas;
        QRectF sourceDomain;
        int gutter = 1;
        Filter filter = Filter::ExactTexels;
        bool operator==(const Placement& other) const;
    };
    struct Binding {
        QString meshId, placementId;
        QVector<QPointF> familyCoordinates;
        CModelDrawableMaterial sampledMaterial;
        QByteArray topologyDigest;
        BindingStatus status = BindingStatus::Linked;
        QString detachedReason;
        bool operator==(const Binding& other) const;
    };
    struct State {
        QMap<QByteArray, Image> images;
        QMap<QString, Family> families;
        QMap<QString, Revision> revisions;
        QMap<QString, Material> materials;
        QMap<QString, Placement> placements;
        QMap<QString, Binding> bindings;
        bool IsEmpty() const;
        bool operator==(const State& other) const;
        bool operator!=(const State& other) const { return !(*this == other); }
    private:
        struct Verification;
        std::shared_ptr<const Verification> m_verification;
        friend class CModelArtwork;
    };
    struct Archive {
        QJsonObject metadata;
        QMap<QByteArray, QByteArray> blobs;
    };
    struct Usage {
        qint64 encodedBytes = 0;
        qint64 decodedBytes = 0;
        qint64 coordinateCount = 0;
        qint64 metadataBytes = 0;
    };

    static constexpr qint64 MaximumBytes = 2LL * 1024 * 1024 * 1024 - 1;
    static constexpr int MaximumRecords = 100000;
    static constexpr int MaximumCoordinates = CModelMaterialEdit::MaximumValues;
    static constexpr qint64 MaximumMetadataBytes = 64LL * 1024 * 1024;
    static constexpr int MaximumDepth = 128;
    static constexpr int MaximumTopologyIndices = 3000000;
    static bool TopologyDigest(int vertexCount, const QVector<quint16>& indices, QByteArray& result, QString* error = nullptr);
    static bool ValidateTransform(const QTransform& transform, QString* error = nullptr);
    static bool Validate(const State& state, Usage* usage = nullptr, QString* error = nullptr);
    static bool ValidateWithCache(const State& state, CMaterialValidationCache& cache,
                                  Usage* usage = nullptr, QString* error = nullptr);
    static bool Freeze(const State& state, State& result, QString* error = nullptr);
    static bool EncodeArchive(const State& state, Archive& result, QString* error = nullptr);
    static bool DecodeArchive(const Archive& archive, State& result, QString* error = nullptr);
    static bool ToJson(const State& state, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, State& result, QString* error = nullptr);
    static bool Fingerprint(const State& state, QByteArray& result, QString* error = nullptr);
private:
    static bool WriteFingerprint(const State& state, QByteArray& result, QString* error);
};
}
Q_DECLARE_METATYPE(live2dstruct::CModelArtwork::State)
#endif
