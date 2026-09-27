#ifndef CMODELDOCUMENT_H
#define CMODELDOCUMENT_H

#include <QSize>
#include <QString>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QVariant>
#include <QMetaType>
#include <QByteArray>
#include <QMap>
#include <memory>
#include <optional>
#include "cmocparameterstructurebuilder.h"
#include "cmocmeshstructurebuilder.h"
#include "cdeformerpreserverequest.h"
#include "ckeyformgridsnapshot.h"
#include "cparameterorganization.h"
#include "cmodelgeometryedit.h"
#include "cmodelmaterialedit.h"
#include "cmodelatlasedit.h"
#include "cgeometryobjectpairs.h"

namespace live2dstruct {
class CReader;
class CKeyformBindingGrid;
class CMocPreviewBuilder;

struct CModelDiagnostic
{
    enum Severity { Warning, Error };
    Severity severity = Error;
    QString code;
    QString message;
    int index = -1;
};

struct CModelParameter
{
    QString id;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    int decimalPlaces = 0;
    bool repeat = false;
    int type = 0;
    QVector<float> keyValues;
};

struct CModelParameterMetadata
{
    QString displayName;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    int decimalPlaces = 0;
    bool repeat = false;

    bool operator==(const CModelParameterMetadata& other) const
    {
        return displayName == other.displayName && minimum == other.minimum && maximum == other.maximum &&
               defaultValue == other.defaultValue && decimalPlaces == other.decimalPlaces && repeat == other.repeat;
    }
};

struct CModelKeyform
{
    QVector<float> positions;
    float opacity = 1.0f;
    float drawOrder = 0.0f;
};

struct CModelPart
{
    QString id;
    int parentIndex = -1;
    bool visible = true;
    bool enabled = true;
};

struct CModelRotationKeyform
{
    float opacity = 1.0f;
    float angle = 0.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    float scale = 1.0f;
    bool reflectX = false;
    bool reflectY = false;

    bool IsValid() const;

    bool operator==(const CModelRotationKeyform& other) const
    {
        return opacity == other.opacity && angle == other.angle && originX == other.originX &&
               originY == other.originY && scale == other.scale && reflectX == other.reflectX &&
               reflectY == other.reflectY;
    }
};

struct CModelWarpKeyform
{
    QVector<float> positions;
    float opacity = 1.0f;

    bool operator==(const CModelWarpKeyform& other) const
    {
        return positions == other.positions && opacity == other.opacity;
    }
};

struct CModelDeformer
{
    enum Type { Warp, Rotation };
    QString id;
    Type type = Warp;
    int parentPartIndex = -1;
    int parentDeformerIndex = -1;
    bool visible = true;
    bool enabled = true;
    int rows = 0;
    int columns = 0;
    float baseAngle = 0.0f;
    bool quadTransform = false;
    QVector<CModelWarpKeyform> warpKeyforms;
    QVector<CModelRotationKeyform> rotationKeyforms;
};

struct CModelDrawable
{
    QString id;
    int parentPartIndex = -1;
    int parentDeformerIndex = -1;
    int textureIndex = -1;
    int flags = 0;
    int blendMode = 0;
    bool culling = true;
    int vertexCount = 0;
    bool visible = true;
    bool enabled = true;
    float opacity = 1.0f;
    float drawOrder = 0.0f;
    QVector<float> uvs;
    QVector<quint16> indices;
    QVector<int> maskIndices;
    QVector<CModelKeyform> keyforms;
};

struct CModelDrawOrderGroupObject
{
    int type = -1;
    int index = -1;
    int selfIndex = -1;
};

struct CModelDrawOrderGroup
{
    int minimumDrawOrder = 0;
    int maximumDrawOrder = 0;
    int objectCount = 0;
    int totalObjectCount = 0;
    bool objectsValid = true;
    QVector<CModelDrawOrderGroupObject> objects;
};

struct CModelEditRuntimeState
{
    QVariant before;
    QVariant after;
    QVariant beforeEditor;
    QVariant afterEditor;
};

struct CModelEditStorageState
{
    QVariantMap before;
    QVariantMap after;
};

struct CModelEdit
{
    enum Field { Visibility, Opacity, DrawOrder, ParameterValue, ParameterValues, ParameterMetadata, KeyformPositions, MeshIndices, MaskIndices, MaskInverted, DeformerParent, RotationKeyforms, WarpKeyforms, VisibilityValues, ParameterStructure, MeshStructure, DeformerStructure, ParameterOrganization, GeometryKeyforms, GeometryObjectPairs, DrawableMaterials, AtlasLayout };
    QString name;
    QString type;
    Field field = Visibility;
    QVariant before;
    QVariant after;
    int index = -1;
    bool coalesce = true;
    std::shared_ptr<CModelEditRuntimeState> runtime;
    std::shared_ptr<const CModelEditStorageState> storage;
    enum class ParameterChange { None, Create, Bind, Delete, BindingKeys };
    ParameterChange parameterChange = ParameterChange::None;
    std::optional<float> freezeValue;
    QVector<CMocMeshStructureBuilder::MeshRemap> meshRemaps;
    std::shared_ptr<const CMocMeshStructureBuilder::Request> meshRequest;
    std::shared_ptr<const CMocParameterStructureBuilder::EditBindingKeysRequest> bindingKeysRequest;
    std::shared_ptr<const CDeformerPreserveRequest> deformerRequest;
};

class CModelDocument
{
public:
    bool Load(const QString& mocPath, QString* error = nullptr);
    bool LoadBytes(const QByteArray& bytes, const QString& sourcePath, QString* error = nullptr);
    void Clear();
    quint64 Revision() const { return m_revision; }
    void CommitCandidate(CModelDocument&& candidate) noexcept;
    bool CaptureGeometryKeyform(CModelGeometryKeyform::Type type, const QString& id, int keyform,
                                CModelGeometryKeyform& result, QString* error = nullptr) const;
    bool MakeGeometryEdit(const CModelGeometryState& before, const CModelGeometryState& after,
                          const CModelGeometryKeyform& driver, CModelEdit& edit, QString* error = nullptr) const;

    bool IsLoaded() const { return m_loaded; }
    const QString& SourcePath() const { return m_sourcePath; }
    const QByteArray& SourceBytes() const { return m_sourceBytes; }
    const QByteArray& SourceDigest() const { return m_sourceDigest; }
    bool SourceMatchesDisk(QString* error = nullptr) const;
    bool KeyformParameterValues(const QString& id, const QString& type, int keyformIndex,
                               QMap<QString, float>& values, QString* error = nullptr) const;
    bool DescribeKeyformGrid(const QString& id, const QString& type, CKeyformGridSnapshot& snapshot,
                             QString* error = nullptr) const;
    bool BuildPreviewMoc(QByteArray& bytes, QString* error = nullptr) const;
    bool BuildRuntimeMoc(QByteArray& bytes, QString* error = nullptr) const;
    const QByteArray& WorkingBaseBytes() const { return m_workingBaseBytes; }
    const CMocParameterStructureBuilder::ColumnState& ParameterStructure() const { return m_structure; }
    bool SetParameterStructure(const CMocParameterStructureBuilder::ColumnState& structure, QString* error = nullptr);
    bool CaptureDrawableMaterials(CModelMaterialState& result, QString* error = nullptr) const;
    bool CaptureDrawableMaterial(const QString& id, CModelDrawableMaterial& result, QString* error = nullptr) const;
    bool MakeMaterialEdit(const CModelMaterialState& before, const CModelMaterialState& after,
                          CModelEdit& edit, QString* error = nullptr) const;
    bool SetDrawableMaterials(const CModelMaterialState& state, QString* error = nullptr);
    bool InitializeTextureWorkspace(const CTextureAssets::Catalog& sources, const CTextureAssets::State& legacy = {},
                                    QString* error = nullptr);
    const CTextureAssets::Catalog& TextureSources() const { return m_textureSources; }
    const CTextureWorkspace::State& TextureWorkspace() const { return m_textureWorkspace; }
    const QByteArray& TextureWorkspaceFingerprint() const { return m_textureWorkspaceFingerprint; }
    bool HasTextureWorkspace() const { return !m_textureWorkspace.sourceFingerprint.isEmpty(); }
    bool SetTextureWorkspace(const CTextureWorkspace::State& state, QString* error = nullptr);
    const CModelArtwork::State& Artwork() const { return m_artwork; }
    const QByteArray& ArtworkFingerprint() const { return m_artworkFingerprint; }
    bool SetArtwork(const CModelArtwork::State& state, QString* error = nullptr);
    bool ValidateArtworkReferences(const CModelArtwork::State& state, QString* error = nullptr) const;
    bool CaptureAtlasState(CModelAtlasState& result, QString* error = nullptr) const;
    bool MakeAtlasEdit(const CModelAtlasState& before, const CModelAtlasState& after,
                       CModelEdit& edit, QString* error = nullptr) const;
    const CModelMaterialOverrides& DrawableMaterialOverrides() const { return m_drawableMaterialOverrides; }
    bool HasMaterialStructure() const { return m_hasMaterialStructure; }
    bool SetMaterialStructure(const CMocParameterStructureBuilder::ColumnState& structure, bool meshStructure,
                              bool deformerStructure, const QSet<QString>& sourceIds, QString* error = nullptr);
    bool HasMeshStructure() const { return m_hasMeshStructure; }
    bool HasDeformerStructure() const { return m_hasDeformerStructure; }
    const QSet<QString>& SourceDeformerIds() const { return m_sourceDeformerIds; }
    QVariantMap DeformerStructureState() const;
    bool SetDeformerStructure(const CMocParameterStructureBuilder::ColumnState& structure,
                              bool meshStructure, const QSet<QString>& sourceIds, QString* error = nullptr);
    bool MakeDeformerStructureEdit(const CDeformerPreserveRequest& request, CModelEdit& edit,
                                   QString* error = nullptr) const;
    bool SetAuthoringStructure(const CMocParameterStructureBuilder::ColumnState& structure, QString* error = nullptr);
    QVariantMap AuthoringStructureState() const;
    QVariantMap ParameterStructureState() const { return AuthoringStructureState(); }
    bool MakeMeshStructureEdit(const CMocMeshStructureBuilder::Request& request, CModelEdit& edit,
                               QString* error = nullptr) const;
    bool AnalyzeMeshStructureEdit(const CMocMeshStructureBuilder::Request& request,
                                  CMocMeshStructureBuilder::Impact& impact, QString* error = nullptr) const;
    bool MakeCreateParameterEdit(const CMocParameterStructureBuilder::CreateRequest& request,
                                 const QString& displayName, CModelEdit& edit, QString* error = nullptr) const;
    bool MakeBindParameterEdit(const QString& id, const QVector<CMocParameterStructureBuilder::BindingRequest>& bindings,
                               CModelEdit& edit, QString* error = nullptr) const;
    bool MakeBindingKeysEdit(const CMocParameterStructureBuilder::EditBindingKeysRequest& request,
                             CModelEdit& edit, QString* error = nullptr) const;
    bool MakeDeleteParameterEdit(const QString& id, CModelEdit& edit, QString* error = nullptr) const;
    bool MakeDeleteParameterEdit(const CMocParameterStructureBuilder::DeleteRequest& request,
                                 CModelEdit& edit, QString* error = nullptr) const;
    const QSize& CanvasPixels() const { return m_canvasPixels; }
    const QVector<CModelParameter>& Parameters() const { return m_parameters; }
    bool SetParameterValue(const QString& id, float value);
    float ParameterValue(const QString& id) const;
    CModelParameterMetadata ParameterMetadata(const QString& id) const;
    QVariantMap ParameterMetadataState(const QString& id) const;
    bool MakeParameterMetadataEdit(const QString& id, const CModelParameterMetadata& metadata,
                                   CModelEdit& edit, QString* error = nullptr) const;
    bool SetParameterMetadata(const QString& id, const CModelParameterMetadata& metadata);
    const QHash<QString, CModelParameterMetadata>& ParameterMetadataOverrides() const { return m_parameterMetadataOverrides; }
    const CGeometryObjectPairs::State& GeometryObjectPairs() const { return m_geometryObjectPairs; }
    bool SetGeometryObjectPairs(const CGeometryObjectPairs::State& state, QString* error = nullptr);
    bool MakeGeometryObjectPairsEdit(const CGeometryObjectPairs::State& state, CModelEdit& edit,
                                     QString* error = nullptr) const;
    const CParameterOrganization::State& ParameterOrganization() const { return m_parameterOrganization; }
    bool SetParameterOrganization(const CParameterOrganization::State& state, QString* error = nullptr);
    bool MakeParameterOrganizationEdit(const CParameterOrganization::State& state, CModelEdit& edit,
                                       QString* error = nullptr) const;
    const QHash<QString, float>& ParameterValues() const { return m_parameterValues; }
    QVector<float> DrawableKeyformPositions(const QString& id, int keyformIndex) const;
    bool SetDrawableKeyformPositions(const QString& id, int keyformIndex, const QVector<float>& positions);
    const QHash<QString, QVector<QVector<float>>>& DrawableKeyformPositionOverrides() const {
        return m_drawableKeyformPositionOverrides;
    }
    const QVector<CModelPart>& Parts() const { return m_parts; }
    const QVector<CModelDeformer>& Deformers() const { return m_deformers; }
    int DeformerParentIndex(const QString& id) const;
    int SourceDeformerParentIndex(const QString& id) const { return m_sourceDeformerParents.value(id, -1); }
    QSet<QString> HiddenDeformerDrawables() const;
    bool SetDeformerParentIndex(const QString& id, int parentIndex);
    bool SetDeformerParentIndices(const QHash<QString, int>& parents);
    const QHash<QString, int>& DeformerParentOverrides() const { return m_deformerParentOverrides; }
    QVector<CModelRotationKeyform> DeformerRotationKeyforms(const QString& id) const;
    bool SetDeformerRotationKeyforms(const QString& id, const QVector<CModelRotationKeyform>& keyforms);
    const QHash<QString, QVector<CModelRotationKeyform>>& DeformerRotationKeyformOverrides() const {
        return m_deformerRotationKeyformOverrides;
    }
    QVector<CModelWarpKeyform> DeformerWarpKeyforms(const QString& id) const;
    bool SetDeformerWarpKeyforms(const QString& id, const QVector<CModelWarpKeyform>& keyforms);
    const QHash<QString, QVector<CModelWarpKeyform>>& DeformerWarpKeyformOverrides() const {
        return m_deformerWarpKeyformOverrides;
    }
    const QVector<CModelDrawable>& Drawables() const { return m_drawables; }
    QVector<quint16> DrawableIndices(const QString& id) const;
    bool SetDrawableIndices(const QString& id, const QVector<quint16>& indices);
    const QHash<QString, QVector<quint16>>& DrawableIndexOverrides() const { return m_drawableIndexOverrides; }
    QVector<int> DrawableMaskIndices(const QString& id) const;
    bool SetDrawableMaskIndices(const QString& id, const QVector<int>& maskIndices);
    const QHash<QString, QVector<int>>& DrawableMaskIndexOverrides() const { return m_drawableMaskIndexOverrides; }
    bool DrawableMaskInverted(const QString& id) const;
    bool SetDrawableMaskInverted(const QString& id, bool inverted);
    const QHash<QString, bool>& DrawableMaskInvertedOverrides() const { return m_drawableMaskInvertedOverrides; }
    const QVector<CModelDrawOrderGroup>& DrawOrderGroups() const { return m_drawOrderGroups; }
    bool SetPartVisible(const QString& id, bool visible);
    bool SetDeformerVisible(const QString& id, bool visible);
    bool SetDrawableVisible(const QString& id, bool visible);
    QMap<QString, bool> VisibilityState() const;
    bool SetVisibilityValues(const QMap<QString, bool>& values);
    bool SetDrawableOpacity(const QString& id, float opacity);
    float DrawableOpacity(const CModelDrawable& drawable) const;
    const QHash<QString, float>& DrawableOpacityOverrides() const { return m_drawableOpacityOverrides; }
    bool SetDrawableDrawOrder(const QString& id, int drawOrder);
    float DrawableDrawOrder(const CModelDrawable& drawable) const;
    const QHash<QString, int>& DrawableDrawOrderOverrides() const { return m_drawableDrawOrderOverrides; }
    void SetLocked(const QString& type, const QString& id, bool locked);
    bool SetLockedValues(const QMap<QString, bool>& values);
    void ClearLocked();
    bool IsLocked(const QString& type, const QString& id) const;
    const QSet<QString>& LockedKeys() const { return m_lockedKeys; }
    bool ExecuteEdit(const CModelEdit& edit);
    bool PeekUndoEdit(CModelEdit* edit = nullptr) const;
    bool PeekRedoEdit(CModelEdit* edit = nullptr) const;
    bool UndoEdit(CModelEdit* edit = nullptr);
    bool RedoEdit(CModelEdit* edit = nullptr);
    void ClearEditHistory();
    bool CanUndoEdit() const { return m_editIndex > 0; }
    bool CanRedoEdit() const { return m_editIndex < m_editHistory.size(); }
    QSet<QString> HiddenIds() const;

    QVector<CModelDiagnostic> Validate() const;

private:
    static bool HasValidStructureRequests(const CModelEdit& edit);
    QVariantMap EditStorage(const CModelEdit& edit) const;
    bool RestoreEditStorage(const QVariantMap& state);
    bool ValidateEditStorage(const CModelEdit& edit) const;
    bool PrepareParameterStructure(const CMocParameterStructureBuilder::ColumnState& structure,
                                   CModelDocument& candidate, QString* error) const;
    bool PrepareStructure(const CMocParameterStructureBuilder::ColumnState& structure, bool meshStructure,
                           CModelDocument& candidate, QString* error) const;
    bool PrepareStructure(const CMocParameterStructureBuilder::ColumnState& structure, bool meshStructure,
                           bool deformerStructure, const QSet<QString>& sourceIds, bool changeDirectory, bool materialStructure,
                           CModelDocument& candidate, QString* error) const;
    bool ValidateDeformerTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                    bool forward, QString* error) const;
    bool SameDeformerDirectory(const CModelDocument& candidate, QString* error) const;
    bool MakeParameterStructureEdit(const CMocParameterStructureBuilder::Result& result, const QString& id,
                                    const QString* displayName, bool created, CModelEdit& edit, QString* error,
                                    const CMocParameterStructureBuilder::EditBindingKeysRequest* keys = nullptr) const;
    bool ApplyStructureState(const QVariantMap& state, const CModelEdit& edit, bool forward, QString* error = nullptr);
    bool ValidateMeshTransition(const CModelDocument& candidate, const CModelEdit& edit, bool forward, QString* error) const;
    bool ValidateBindingKeysTransition(const CModelDocument& candidate, const CModelEdit& edit,
                                       bool forward, QString* error) const;
    bool PreserveStructureProperties(CModelDocument& candidate, QString* error) const;
    bool BuildMeshBase(const CMocMeshStructureBuilder::Request& request, QByteArray& bytes, QString* error) const;
    bool ApplyParameterMetadataState(const QString& id, const QVariantMap& state);
    bool m_loaded = false;
    quint64 m_revision = 0;
    QString m_sourcePath;
    QByteArray m_sourceBytes;
    QByteArray m_sourceDigest;
    QByteArray m_workingBaseBytes;
    CMocParameterStructureBuilder::ColumnState m_structure;
    bool m_hasMeshStructure = false;
    bool m_hasMaterialStructure = false;
    CModelMaterialOverrides m_drawableMaterialOverrides;
    CTextureAssets::Catalog m_textureSources;
    CTextureWorkspace::State m_textureWorkspace;
    QByteArray m_textureWorkspaceFingerprint;
    CModelArtwork::State m_artwork;
    QByteArray m_artworkFingerprint;
    QHash<QString, int> m_sourceDeformerParents;
    QSet<QString> m_sourceDeformerIds;
    bool m_hasDeformerStructure = false;
    std::shared_ptr<const CKeyformBindingGrid> m_keyformBindings;
    std::shared_ptr<const CMocPreviewBuilder> m_previewBuilder;
    QString m_previewError;
    QSize m_canvasPixels;
    QVector<CModelParameter> m_parameters;
    QVector<CModelPart> m_parts;
    QVector<CModelDeformer> m_deformers;
    QVector<CModelDrawable> m_drawables;
    QVector<CModelDrawOrderGroup> m_drawOrderGroups;
    QHash<QString, float> m_drawableOpacityOverrides;
    QHash<QString, int> m_drawableDrawOrderOverrides;
    QHash<QString, float> m_parameterValues;
    QHash<QString, CModelParameterMetadata> m_parameterMetadataOverrides;
    CParameterOrganization::State m_parameterOrganization;
    CGeometryObjectPairs::State m_geometryObjectPairs;
    QHash<QString, QVector<QVector<float>>> m_drawableKeyformPositionOverrides;
    QHash<QString, QVector<quint16>> m_drawableIndexOverrides;
    QHash<QString, QVector<int>> m_drawableMaskIndexOverrides;
    QHash<QString, int> m_deformerParentOverrides;
    QHash<QString, QVector<CModelRotationKeyform>> m_deformerRotationKeyformOverrides;
    QHash<QString, QVector<CModelWarpKeyform>> m_deformerWarpKeyformOverrides;
    QHash<QString, bool> m_drawableMaskInvertedOverrides;
    QSet<QString> m_lockedKeys;
    QVector<CModelEdit> m_editHistory;
    int m_editIndex = 0;

    bool ReconcileGeometryObjectPairs(CModelDocument& candidate, const QVector<CGeometryObjectPairs::VertexRemap>& vertices,
                                       const CMocParameterStructureBuilder::Result* parameters,
                                       const CMocParameterStructureBuilder::EditBindingKeysRequest* keys,
                                       QString* error) const;
    bool ApplyEdit(const CModelEdit& edit, bool forward);
    bool ApplyMaterialEdit(const CModelEdit& edit, bool forward, QString* error = nullptr, bool updateArtwork = true);
    bool ApplyDrawableMaterials(const CModelMaterialState& state, bool updateArtwork, QString* error);
    bool ReconcileArtworkAppearance(const CModelAtlasState& before, QString* error);
    bool ValidateMaterialEditStorage(const CModelEdit& edit) const;
    bool ApplyAtlasEdit(const CModelEdit& edit, bool forward, QString* error = nullptr);
    bool ValidateAtlasEditStorage(const CModelEdit& edit) const;
    bool ValidateAtlasHistory(const CModelEdit& edit, QString* error) const;
    bool ApplyGeometryEdit(const CModelEdit& edit, bool forward, QString* error = nullptr);
    bool ValidateGeometryEditStorage(const CModelEdit& edit) const;
    QByteArray GeometryEditStorageFingerprint(const CModelEdit& edit) const;
    bool LoadInternal(CReader& reader, const QString& sourcePath, QString* error);
};

}

Q_DECLARE_METATYPE(live2dstruct::CModelParameterMetadata)
Q_DECLARE_METATYPE(live2dstruct::CModelRotationKeyform)
Q_DECLARE_METATYPE(QVector<live2dstruct::CModelRotationKeyform>)
Q_DECLARE_METATYPE(live2dstruct::CModelWarpKeyform)
Q_DECLARE_METATYPE(QVector<live2dstruct::CModelWarpKeyform>)

#endif
