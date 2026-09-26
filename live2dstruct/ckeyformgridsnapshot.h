#ifndef CKEYFORMGRIDSNAPSHOT_H
#define CKEYFORMGRIDSNAPSHOT_H

#include "ckeyformbindinggrid.h"
#include <QByteArray>
#include <memory>

namespace live2dstruct {
class CModelDocument;

class CKeyformGridSnapshot
{
public:
    struct Axis {
        int bindingIndex = -1;
        QString parameterId;
        QString displayName;
        int parameterType = 0;
        QVector<float> keys;
        float minimum = 0;
        float maximum = 0;
    };
    QString objectId;
    QString objectType;
    quint64 revision = 0;
    QByteArray sourceDigest;
    QVector<Axis> axes;
    int keyformCount = 0;
    qint64 expectedKeyformCount = 0;
    bool axesValid = false;
    QString diagnostic;

    bool Pose(int keyform, QMap<QString, float>& values, QString* error = nullptr) const;

private:
    friend class CModelDocument;
    std::shared_ptr<const CKeyformBindingGrid> m_bindings;
    CKeyformBindingGrid::ObjectType m_type = CKeyformBindingGrid::ArtMesh;
    QHash<QString, QPair<float, float>> m_ranges;
};
}

#endif
