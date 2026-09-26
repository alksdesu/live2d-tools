#ifndef CKEYFORMBINDINGGRID_H
#define CKEYFORMBINDINGGRID_H

#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>
#include <array>

namespace live2dstruct {
class CSectionTable;

class CKeyformBindingGrid
{
public:
    enum ObjectType : int { ArtMesh, Warp, Rotation };

    struct Axis
    {
        int bindingIndex = -1;
        QString parameterId;
        int parameterType = 0;
        QVector<float> keys;
    };

    struct Description
    {
        QVector<Axis> axes;
        int keyformCount = 0;
        qint64 expectedKeyformCount = 0;
        bool axesValid = false;
        QString diagnostic;
    };

    bool Build(const CSectionTable& table, QString* error = nullptr);
    bool Pose(ObjectType type, const QString& id, int keyformIndex,
              QMap<QString, float>& values, QString* error = nullptr) const;
    QVector<Axis> Axes(ObjectType type, const QString& id) const;
    QString Diagnostic(ObjectType type, const QString& id) const;
    Description Describe(ObjectType type, const QString& id) const;

private:
    struct Binding
    {
        QString parameterId;
        int parameterType = 0;
        int keyBegin = 0;
        int keyCount = 0;
        bool structureValid = false;
        QString error;
    };

    struct Grid
    {
        int axisBegin = 0;
        int axisCount = 0;
        int keyformCount = 0;
        qint64 expectedKeyformCount = 0;
        bool axesValid = false;
        bool structureValid = false;
        QString error;
    };

    const Grid* Find(ObjectType type, const QString& id) const;
    QVector<Axis> CollectAxes(const Grid& grid) const;
    std::array<QHash<QString, Grid>, 3> m_grids;
    QVector<float> m_keys;
    QVector<int> m_references;
    QVector<Binding> m_bindings;
};
}

#endif
