#ifndef CMOCBLENDSHAPECONSTRAINTBUILDER_H
#define CMOCBLENDSHAPECONSTRAINTBUILDER_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CMocBlendShapeConstraintBuilder
{
public:
    struct Key {
        int constraintIndex = -1;

        bool operator==(const Key& other) const { return constraintIndex == other.constraintIndex; }
    };
    struct Value {
        float key = 0.0f;
        float weight = 0.0f;

        bool operator==(const Value& other) const { return key == other.key && weight == other.weight; }
    };
    struct Frame {
        Key key;
        QByteArray sourceDigest;
        int parameterIndex = -1;
        QString parameterId;
        float parameterMinimum = 0.0f;
        float parameterMaximum = 0.0f;
        QVector<Value> values;

        bool operator==(const Frame& other) const
        {
            return key == other.key && sourceDigest == other.sourceDigest && parameterIndex == other.parameterIndex &&
                parameterId == other.parameterId && parameterMinimum == other.parameterMinimum &&
                parameterMaximum == other.parameterMaximum && values == other.values;
        }
    };
    struct Request {
        Frame before;
        QVector<Value> values;
    };
    struct Result {
        QByteArray bytes;
        Frame frame;
    };

    static bool Capture(const QByteArray& bytes, const Key& key, Frame& result, QString* error = nullptr);
    static bool Build(const QByteArray& bytes, const Request& request, Result& result, QString* error = nullptr);
};
}

#endif
