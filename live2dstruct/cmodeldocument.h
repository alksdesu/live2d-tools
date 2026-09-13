#ifndef CMODELDOCUMENT_H
#define CMODELDOCUMENT_H

#include <QSize>
#include <QString>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QVariant>

namespace live2dstruct {

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

struct CModelEdit
{
    enum Field { Visibility, Opacity, DrawOrder, ParameterValue, ParameterMetadata, KeyformPositions, MeshIndices, MaskIndices, MaskInverted, DeformerParent };
    QString name;
    QString type;
    Field field = Visibility;
    QVariant before;
    QVariant after;
    int index = -1;
    bool coalesce = true;
};

class CModelDocument
{
public:
    bool Load(const QString& mocPath, QString* error = nullptr);
    void Clear();

    bool IsLoaded() const { return m_loaded; }
    const QString& SourcePath() const { return m_sourcePath; }
    const QSize& CanvasPixels() const { return m_canvasPixels; }
    const QVector<CModelParameter>& Parameters() const { return m_parameters; }
    bool SetParameterValue(const QString& id, float value);
    float ParameterValue(const QString& id) const;
    CModelParameterMetadata ParameterMetadata(const QString& id) const;
    bool SetParameterMetadata(const QString& id, const CModelParameterMetadata& metadata);
    const QHash<QString, CModelParameterMetadata>& ParameterMetadataOverrides() const { return m_parameterMetadataOverrides; }
    const QHash<QString, float>& ParameterValues() const { return m_parameterValues; }
    QVector<float> DrawableKeyformPositions(const QString& id, int keyformIndex) const;
    bool SetDrawableKeyformPositions(const QString& id, int keyformIndex, const QVector<float>& positions);
    const QHash<QString, QVector<QVector<float>>>& DrawableKeyformPositionOverrides() const {
        return m_drawableKeyformPositionOverrides;
    }
    const QVector<CModelPart>& Parts() const { return m_parts; }
    const QVector<CModelDeformer>& Deformers() const { return m_deformers; }
    int DeformerParentIndex(const QString& id) const;
    bool SetDeformerParentIndex(const QString& id, int parentIndex);
    bool SetDeformerParentIndices(const QHash<QString, int>& parents);
    const QHash<QString, int>& DeformerParentOverrides() const { return m_deformerParentOverrides; }
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
    bool SetDrawableOpacity(const QString& id, float opacity);
    float DrawableOpacity(const CModelDrawable& drawable) const;
    const QHash<QString, float>& DrawableOpacityOverrides() const { return m_drawableOpacityOverrides; }
    bool SetDrawableDrawOrder(const QString& id, int drawOrder);
    float DrawableDrawOrder(const CModelDrawable& drawable) const;
    const QHash<QString, int>& DrawableDrawOrderOverrides() const { return m_drawableDrawOrderOverrides; }
    void SetLocked(const QString& type, const QString& id, bool locked);
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
    bool m_loaded = false;
    QString m_sourcePath;
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
    QHash<QString, QVector<QVector<float>>> m_drawableKeyformPositionOverrides;
    QHash<QString, QVector<quint16>> m_drawableIndexOverrides;
    QHash<QString, QVector<int>> m_drawableMaskIndexOverrides;
    QHash<QString, int> m_deformerParentOverrides;
    QHash<QString, bool> m_drawableMaskInvertedOverrides;
    QSet<QString> m_lockedKeys;
    QVector<CModelEdit> m_editHistory;
    int m_editIndex = 0;

    bool ApplyEdit(const CModelEdit& edit, bool forward);
};

}

#endif
