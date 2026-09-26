#ifndef CMOC_PARAMETER_STRUCTURE_BUILDER_H
#define CMOC_PARAMETER_STRUCTURE_BUILDER_H

#include <QByteArray>
#include <QMap>
#include <QString>
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

    struct ColumnState
    {
        QMap<int, QByteArray> columns;
        QMap<int, int> counts;

        bool operator==(const ColumnState& other) const { return columns == other.columns && counts == other.counts; }
        bool operator!=(const ColumnState& other) const { return !(*this == other); }
    };

    struct KeyformRemap
    {
        Owner owner;
        int oldKeyformCount = 0;
        int newKeyformCount = 0;
        int newAxisKeyCount = 0;
        QVector<int> newLocalToOldLocal;
    };

    struct Result
    {
        QByteArray bytes;
        QVector<int> oldParameterToNew;
        QVector<KeyformRemap> keyforms;
    };

    struct Reference
    {
        QString kind;
        QString ownerType;
        QString ownerId;
        QString detail;
    };

    static bool Create(const QByteArray& base, const CreateRequest& request,
                       Result& result, QString* error = nullptr);
    static bool BindExistingParameter(const QByteArray& base, const QString& parameterId,
                                      const QVector<BindingRequest>& bindings,
                                      Result& result, QString* error = nullptr);
    static bool AnalyzeDelete(const QByteArray& base, const QString& parameterId,
                              QVector<Reference>& references, QString* error = nullptr);
    static bool DeleteUnreferenced(const QByteArray& base, const QString& parameterId,
                                  Result& result, QString* error = nullptr);
    static bool CaptureColumnState(const QByteArray& original, const QByteArray& effective,
                                   ColumnState& state, QString* error = nullptr);
    static bool BuildColumnState(const QByteArray& original, const ColumnState& state,
                                 QByteArray& bytes, QString* error = nullptr);
};
}

#endif
