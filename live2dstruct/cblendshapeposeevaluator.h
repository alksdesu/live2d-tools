#ifndef CBLENDSHAPEPOSEEVALUATOR_H
#define CBLENDSHAPEPOSEEVALUATOR_H

#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CSectionTable;

class CBlendShapePoseEvaluator
{
public:
    struct RotationDelta {
        float opacity = 0.0f;
        float angle = 0.0f;
        float originX = 0.0f;
        float originY = 0.0f;
        float scale = 0.0f;
        bool reflectionConflict = false;
    };

    struct Overrides {
        QHash<QString, QVector<float>> artMeshPositions;
        QHash<QString, QVector<float>> warpPositions;
        QHash<QString, RotationDelta> rotations;
        QSet<QString> reflectionConflicts;
    };

    bool Build(const CSectionTable& table, const QMap<QString, float>& fullPose,
               Overrides& result, QString* error = nullptr) const;
};
}

#endif
