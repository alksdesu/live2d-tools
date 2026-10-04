#ifndef CMOC_PARAMETER_STRUCTURE_BUILDER_H
#define CMOC_PARAMETER_STRUCTURE_BUILDER_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace live2dstruct {
class CMocParameterStructureBuilder
{
public:
    struct Definition
    {
        QString id;
        float minimum = -1;
        float maximum = 1;
        float defaultValue = 0;
        int decimalPlaces = 2;
        bool repeat = false;
        int type = 0;
    };

    struct Owner
    {
        enum Type : int { ArtMesh, Warp, Rotation };
        Type type = ArtMesh;
        QString id;

        bool operator==(const Owner& other) const { return type == other.type && id == other.id; }
    };

    struct BindingRequest
    {
        Owner owner;
        QVector<float> keys;
    };

    struct CreateRequest
    {
        Definition parameter;
        QVector<BindingRequest> bindings;
    };

    struct BindingKey
    {
        enum NewSource : int { InterpolateOldAxis, CopyOldSlice };
        float value = 0;
        int oldKeyIndex = -1;
        NewSource source = InterpolateOldAxis;
        int copyOldKeyIndex = -1;
    };

    struct EditBindingKeysRequest
    {
        Owner owner;
        int axisIndex = -1;
        QString parameterId;
        QVector<float> expectedKeys;
        QVector<BindingKey> keys;
    };

    struct EditBindingKeysBatchAxis
    {
        int axisIndex = -1;
        QString parameterId;
        QVector<float> expectedKeys;
        QVector<BindingKey> keys;
    };

    struct EditBindingKeysBatchRequest
    {
        Owner owner;
        QString parameterId;
        QVector<EditBindingKeysBatchAxis> axes;
    };

    struct DeleteRequest
    {
        QString id;
        float freezeValue = 0;
    };

    struct DeleteTarget
    {
        enum Type : int { ArtMesh, Warp, Rotation, Part, Glue };
        Type type = ArtMesh;
        QString id;
        int sourceIndex = -1;
        int oldKeyformCount = 0;
        int newKeyformCount = 0;
        bool interpolated = false;
    };

    struct ColumnState
    {
        QMap<int, QByteArray> columns;
        QMap<int, int> counts;

        bool operator==(const ColumnState& other) const { return columns == other.columns && counts == other.counts; }
        bool operator!=(const ColumnState& other) const { return !(*this == other); }
    };

    struct KeyformRemap
    {
        struct Contribution
        {
            int oldLocal = -1;
            double weight = 0;
        };
        Owner owner;
        int oldKeyformCount = 0;
        int newKeyformCount = 0;
        int newAxisKeyCount = 0;
        QVector<int> newLocalToOldLocal;
        QVector<QVector<Contribution>> newLocalSources;
    };

    struct Result
    {
        QByteArray bytes;
        QVector<int> oldParameterToNew;
        QVector<KeyformRemap> keyforms;
        QVector<DeleteTarget> affectedTargets;
    };

    struct Reference
    {
        QString kind;
        QString ownerType;
        QString ownerId;
        QString detail;
    };

    struct DeleteImpact
    {
        QVector<DeleteTarget> targets;
        QVector<Reference> references;
        QStringList blockingReasons;
    };

    static bool Create(const QByteArray& base, const CreateRequest& request,
                       Result& result, QString* error = nullptr);
    static bool BindExistingParameter(const QByteArray& base, const QString& parameterId,
                                      const QVector<BindingRequest>& bindings,
                                      Result& result, QString* error = nullptr);
    static bool EditBindingKeys(const QByteArray& base, const EditBindingKeysRequest& request,
                                Result& result, QString* error = nullptr);
    static bool EditBindingKeysBatch(const QByteArray& base, const EditBindingKeysBatchRequest& request,
                                      Result& result, QString* error = nullptr);
    static bool AnalyzeDelete(const QByteArray& base, const QString& parameterId,
                              QVector<Reference>& references, QString* error = nullptr);
    static bool AnalyzeDelete(const QByteArray& base, const DeleteRequest& request,
                              DeleteImpact& impact, QString* error = nullptr);
    static bool Delete(const QByteArray& base, const DeleteRequest& request,
                       Result& result, QString* error = nullptr);
    static bool DeleteUnreferenced(const QByteArray& base, const QString& parameterId,
                                  Result& result, QString* error = nullptr);
    static bool CaptureColumnState(const QByteArray& original, const QByteArray& effective,
                                   ColumnState& state, QString* error = nullptr);
    static bool BuildColumnState(const QByteArray& original, const ColumnState& state,
                                 QByteArray& bytes, QString* error = nullptr);
};
}

#endif
