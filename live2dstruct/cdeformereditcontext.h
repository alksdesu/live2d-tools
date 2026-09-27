#ifndef CDEFORMEREDITCONTEXT_H
#define CDEFORMEREDITCONTEXT_H

#include <QMap>
#include <QPointF>
#include <QString>
#include <QVector>
#include <memory>
#include "crotationframegeometry.h"

namespace live2dstruct {
class CModelDocument;
struct CModelRotationKeyform;

class CDeformerEditContext
{
public:
    enum class ReflectionPolicy { Preserve, AllowChange };
    bool Build(const CModelDocument& document, const QString& id, const QString& type,
               int keyform, const QMap<QString, float>& fullPose, QString* error = nullptr);
    bool ToModel(const QVector<float>& local, QVector<float>& world, QString* error = nullptr) const;
    bool FromModel(const QVector<float>& world, QVector<float>& local, QString* error = nullptr) const;
    const QVector<float>& ModelPoints() const;
    QPointF RotationDirection() const;
    bool CurrentRotationFrame(CRotationFrameGeometry::Frame& result, QString* error = nullptr) const;
    bool ResolveRotation(const CModelRotationKeyform& keyform,
                         CRotationFrameGeometry::Frame& result, QString* error = nullptr) const;
    bool SolveRotation(const CRotationFrameGeometry::Frame& desired, const CModelRotationKeyform& reference,
                       CModelRotationKeyform& result, QString* error = nullptr,
                       ReflectionPolicy reflection = ReflectionPolicy::Preserve) const;
    bool IsValid() const { return bool(m_state); }

private:
    struct State;
    std::shared_ptr<const State> m_state;
};
}

#endif
