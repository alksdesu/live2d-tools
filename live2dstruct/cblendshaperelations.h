#ifndef CBLENDSHAPERELATIONS_H
#define CBLENDSHAPERELATIONS_H

#include <QString>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CSectionTable;

class CBlendShapeRelations
{
public:
    enum TargetType : int { ArtMesh, Warp, Rotation, Part };
    struct Slice { int begin = 0; int count = 0; };
    struct Parameter {
        int index = -1;
        QString id;
        int type = 0;
        float minimum = 0, maximum = 1, defaultValue = 0;
        bool selectable = false;
    };
    struct ParameterBinding {
        int index = -1;
        Slice keys;
        int baseKeyIndex = -1;
        bool valid = false;
    };
    struct Binding {
        int index = -1;
        int parameterBindingIndex = -1;
        int parameterIndex = -1;
        Slice keyforms;
        Slice constraints;
        bool valid = false;
    };
    struct Target {
        TargetType type = ArtMesh;
        int objectIndex = -1;
        int sourceIndex = -1;
        QString id;
        bool selectable = false;
    };
    struct Shape {
        TargetType type = ArtMesh;
        int index = -1;
        int sourceIndex = -1;
        Slice bindings;
        bool valid = false;
    };
    struct ConstraintValue { float key = 0, weight = 0; };
    struct Limiter {
        int constraintIndex = -1;
        int parameterIndex = -1;
        Slice values;
        bool valid = false;
    };
    struct Diagnostic {
        QString code;
        QString message;
        QString table;
        int index = -1;
    };

    bool Build(const CSectionTable& table, QString* error = nullptr);
    const QVector<Parameter>& Parameters() const;
    const QVector<int>& BlendShapeParameterIndices() const;
    const QVector<ParameterBinding>& ParameterBindings() const;
    const QVector<Binding>& Bindings() const;
    const QVector<Target>& Targets() const;
    const QVector<Shape>& Shapes() const;
    const QVector<float>& Keys() const;
    const QVector<ConstraintValue>& ConstraintValues() const;
    const QVector<Diagnostic>& Diagnostics() const;
    int ParameterIndex(const QString& id) const;
    QVector<Target> TargetsForParameter(int parameterIndex) const;
    QVector<Target> TargetsForParameter(const QString& id) const;
    QVector<int> ParametersForTarget(TargetType type, const QString& id) const;
    QVector<int> BindingsForTarget(TargetType type, const QString& id) const;
    QVector<Limiter> LimitersForBinding(int bindingIndex) const;

private:
    struct State;
    std::shared_ptr<const State> m_state;
};
}

#endif
