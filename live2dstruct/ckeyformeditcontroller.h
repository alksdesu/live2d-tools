#ifndef CKEYFORMEDITCONTROLLER_H
#define CKEYFORMEDITCONTROLLER_H

#include "cmodeldocument.h"
#include "ckeyformmappingcontext.h"
#include <QPointF>
#include <functional>
#include <optional>

namespace live2dstruct {
class CKeyformEditController
{
public:
    using Publisher = std::function<bool(const CModelDocument&, const QMap<QString, float>&, bool, QString*)>;
    using PreviewEnder = std::function<bool(QString*)>;

    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher, PreviewEnder ender);
    bool Activate(const QString& id, int keyform, const QMap<QString, float>& currentValues, QString* error = nullptr);
    bool End(QString* error = nullptr);
    bool Submit(const CModelEdit& edit, QString* error = nullptr);
    bool Submit(const CModelEdit& edit, const QMap<QString, float>& currentValues,
                int previewKeyform, QString* error = nullptr);
    bool History(bool undo, CModelEdit& edit, QString* error = nullptr);
    bool Restore(CModelDocument candidate, QString* error = nullptr);
    bool BeginDrag(const QVector<int>& selected, const QVector<float>& runtimePositions, QString* error = nullptr);
    bool DragBy(const QPointF& modelDelta, QString* error = nullptr);
    bool CancelDrag(QString* error = nullptr);
    void FinishDrag();
    bool IsActive() const { return !m_target.isEmpty(); }
    bool IsDragging() const { return m_drag.has_value(); }
    const QString& Target() const { return m_target; }
    int Keyform() const { return m_keyform; }
    const QMap<QString, float>& Pose() const { return m_pose; }

private:
    struct Drag
    {
        CModelDocument start;
        CKeyformMappingContext mapping;
        QVector<float> local;
        QVector<float> model;
        QVector<int> selected;
        quint64 revision = 0;
    };
    bool CanEdit(const QString& id, int keyform, QString* error) const;
    bool MakePose(const QString& id, int keyform, const QMap<QString, float>& base,
                  QMap<QString, float>& pose, QString* error) const;
    bool Publish(CModelDocument candidate, const QMap<QString, float>& pose, bool editing, QString* error);
    bool Ready(QString* error) const;
    CModelDocument* m_document = nullptr;
    Publisher m_publish;
    PreviewEnder m_end;
    QString m_target;
    int m_keyform = -1;
    QMap<QString, float> m_pose;
    QByteArray m_source;
    bool m_publishing = false;
    std::optional<Drag> m_drag;
};
}

#endif
