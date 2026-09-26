#ifndef CDEFORMEREDITCONTEXT_H
#define CDEFORMEREDITCONTEXT_H

#include <QMap>
#include <QPointF>
#include <QString>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CModelDocument;

class CDeformerEditContext
{
public:
    bool Build(const CModelDocument& document, const QString& id, const QString& type,
               int keyform, const QMap<QString, float>& fullPose, QString* error = nullptr);
    bool ToModel(const QVector<float>& local, QVector<float>& world, QString* error = nullptr) const;
    bool FromModel(const QVector<float>& world, QVector<float>& local, QString* error = nullptr) const;
    const QVector<float>& ModelPoints() const;
    QPointF RotationDirection() const;
    bool IsValid() const { return bool(m_state); }

private:
    struct State;
    std::shared_ptr<const State> m_state;
};
}

#endif
