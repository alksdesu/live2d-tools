#ifndef CMOCDEFORMERSTRUCTUREBUILDER_H
#define CMOCDEFORMERSTRUCTUREBUILDER_H

#include "cmocparameterstructurebuilder.h"
#include <optional>
#include <variant>

class CExportControl;

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
    struct ResizeWarpRequest
    {
        QString id;
        int rows = 2;
        int columns = 2;
    };
    struct WarpGridImpact
    {
        int oldRows = 0;
        int oldColumns = 0;
        int newRows = 0;
        int newColumns = 0;
        int ordinaryKeyforms = 0;
        int blendKeyforms = 0;
        QVector<int> oldToNew;
        bool exactInteriorSubdivision = false;
        QStringList limitations;
    };
    struct Impact
    {
        QString id;
        QString parentId;
        int partIndex = -1;
        int insertedIndex = -1;
        QVector<Owner> reparented;
        QVector<int> oldDeformerToNew;
        std::optional<WarpGridImpact> warpGrid;
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
    static bool AnalyzeResizeWarp(const QByteArray&, const ResizeWarpRequest&, Impact&, QString* error = nullptr,
        CExportControl* control = nullptr);
    static bool ResizeWarp(const QByteArray&, const ResizeWarpRequest&, Result&, QString* error = nullptr,
        CExportControl* control = nullptr);
};
}

#endif
