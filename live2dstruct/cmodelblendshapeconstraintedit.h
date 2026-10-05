#ifndef CMODELBLENDSHAPECONSTRAINTEDIT_H
#define CMODELBLENDSHAPECONSTRAINTEDIT_H

#include "cmocblendshapeconstraintbuilder.h"
#include <QMetaType>
#include <QVector>

namespace live2dstruct {
struct CModelBlendShapeConstraintEntry
{
    int constraintIndex = -1;
    QByteArray sourceDigest;
    int parameterIndex = -1;
    QString parameterId;
    float parameterMinimum = 0.0f;
    float parameterMaximum = 0.0f;
    QVector<CMocBlendShapeConstraintBuilder::Value> values;

    bool operator==(const CModelBlendShapeConstraintEntry& other) const
    {
        return constraintIndex == other.constraintIndex && sourceDigest == other.sourceDigest &&
            parameterIndex == other.parameterIndex && parameterId == other.parameterId &&
            parameterMinimum == other.parameterMinimum && parameterMaximum == other.parameterMaximum && values == other.values;
    }
};
using CModelBlendShapeConstraintState = QVector<CModelBlendShapeConstraintEntry>;
}

Q_DECLARE_METATYPE(live2dstruct::CModelBlendShapeConstraintState)

#endif
