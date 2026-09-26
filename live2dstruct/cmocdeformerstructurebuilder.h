#ifndef CMOCDEFORMERSTRUCTUREBUILDER_H
#define CMOCDEFORMERSTRUCTUREBUILDER_H

#include "cmocparameterstructurebuilder.h"
#include <variant>

namespace live2dstruct {
class CMocDeformerStructureBuilder
{
public:
    using Owner = CMocParameterStructureBuilder::Owner;
    struct Warp {
        int rows = 2;
        int columns = 2;
        bool quad = false;
        QVector<float> positions;
    };
    struct Rotation {
        float originX = 0;
        float originY = 0;
        float angle = 0;
        float baseAngle = 0;
        float scale = 1;
        bool reflectX = false;
        bool reflectY = false;
    };
    struct InsertRequest {
        QString id;
        int partIndex = -1;
        QString parentId;
        QVector<Owner> children;
        std::variant<Warp, Rotation> shape = Warp{};
    };
    struct RemoveRequest {
        QString id;
    };
    struct Impact {
        QString id;
        QString parentId;
        int partIndex = -1;
        int insertedIndex = -1;
        QVector<Owner> reparented;
        QVector<int> oldDeformerToNew;
    };
    struct Result {
        QByteArray bytes;
        CMocParameterStructureBuilder::ColumnState state;
        Impact impact;
    };

    static bool AnalyzeInsert(const QByteArray&, const InsertRequest&, Impact&, QString* error = nullptr);
    static bool Insert(const QByteArray&, const InsertRequest&, Result&, QString* error = nullptr);
    static bool AnalyzeRemove(const QByteArray&, const RemoveRequest&, Impact&, QString* error = nullptr);
    static bool Remove(const QByteArray&, const RemoveRequest&, Result&, QString* error = nullptr);
};
}

#endif
