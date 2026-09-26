#ifndef CUBISMPHYSICSOBSERVATION_H
#define CUBISMPHYSICSOBSERVATION_H

#include <QByteArray>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>

namespace Live2D { namespace Cubism { namespace Framework {

struct PhysicsInputObservation
{
    QString id, type;
    bool active = false, sampled = false, hasRange = false;
    float value = 0, minimum = 0, maximum = 0, defaultValue = 0;
};

struct PhysicsOutputObservation : PhysicsInputObservation
{
    float rawValue = 0, clampedValue = 0;
    bool clipped = false;
};

struct PhysicsObservation
{
    int groupIndex = -1;
    QString groupId;
    QByteArray sourceDigest;
    quint64 generation = 0, sequence = 0;
    double time = 0;
    bool sampled = false;
    QVector<QPointF> particles;
    QVector<PhysicsInputObservation> inputs;
    QVector<PhysicsOutputObservation> outputs;
};

}}}

Q_DECLARE_METATYPE(Live2D::Cubism::Framework::PhysicsObservation)

#endif
