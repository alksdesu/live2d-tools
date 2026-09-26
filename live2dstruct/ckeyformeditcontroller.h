#ifndef CKEYFORMEDITCONTROLLER_H
#define CKEYFORMEDITCONTROLLER_H

#include "cmodeldocument.h"
#include "ckeyformmappingcontext.h"
#include "cdeformereditcontext.h"
#include <QPointF>
#include <functional>
#include <optional>

namespace live2dstruct {
class CKeyformEditController
{
public:
    enum class DeformerHandle { WarpPoints, RotationOrigin, RotationAngle };
    using Publisher = std::function<bool(const CModelDocument&, const QMap<QString, float>&, bool, QString*)>;
    using PreviewEnder = std::function<bool(QString*)>;
    using DefinitionPublisher = std::function<bool(const CModelDocument&, const QMap<QString, float>&, bool, QString*)>;
    using StructurePublisher = std::function<bool(const CModelDocument&, const CModelEdit&, bool, bool,
        const QMap<QString, float>&, bool, QMap<QString, float>&, QString*)>;
    using StructurePoseProvider = std::function<bool(const CModelDocument&, const CModelEdit&, bool,
        QMap<QString, float>&, QString*)>;
    using MeshPublisher = std::function<bool(const CModelDocument&, const CModelEdit&, bool, QString*)>;

    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher, PreviewEnder ender);
    void SetDefinitionPublisher(DefinitionPublisher publisher);
    void SetStructurePublisher(StructurePublisher publisher, StructurePoseProvider poseProvider = {});
    void SetMeshPublisher(MeshPublisher publisher);
    bool Activate(const QString& id, int keyform, const QMap<QString, float>& currentValues, QString* error = nullptr);
    bool Activate(const QString& id, const QString& type, int keyform,
                  const QMap<QString, float>& currentValues, QString* error = nullptr);
    static bool Handles(const CModelEdit& edit);
    bool End(QString* error = nullptr);
    bool Submit(const CModelEdit& edit, QString* error = nullptr);
    bool Submit(const CModelEdit& edit, const QMap<QString, float>& currentValues,
                int previewKeyform, QString* error = nullptr);
    bool History(bool undo, CModelEdit& edit, QString* error = nullptr);
    bool Restore(CModelDocument candidate, QString* error = nullptr, bool forceStructure = false);
    bool BeginDrag(const QVector<int>& selected, const QVector<float>& runtimePositions, QString* error = nullptr);
    bool DragBy(const QPointF& modelDelta, QString* error = nullptr);
    bool BeginDeformerDrag(DeformerHandle handle, const QVector<int>& selected, QString* error = nullptr);
    bool MoveDeformerDrag(const QPointF& modelDelta, double angleDelta, QString* error = nullptr);
    bool CancelDrag(QString* error = nullptr);
    void FinishDrag();
    bool DragHasChanges() const;
    bool IsActive() const { return !m_target.isEmpty(); }
    bool IsDragging() const { return m_drag.has_value() || m_deformerDrag.has_value(); }
    const QString& Target() const { return m_target; }
    const QString& Type() const { return m_type; }
    int Keyform() const { return m_keyform; }
    const QMap<QString, float>& Pose() const { return m_pose; }

private:
    bool PublishDefinitions(CModelDocument candidate, bool restoreSession, QString* error);
    bool PublishStructure(CModelDocument candidate, const CModelEdit& command, bool forward,
                          bool restoreSession, QString* error);
    bool PublishMesh(CModelDocument candidate, const CModelEdit& command, bool forward, QString* error);
    struct Drag
    {
        CModelDocument start;
        CKeyformMappingContext mapping;
        QVector<float> local;
        QVector<float> model;
        QVector<int> selected;
        quint64 revision = 0;
    };
    struct DeformerDrag
    {
        CModelDocument start;
        CDeformerEditContext mapping;
        DeformerHandle handle = DeformerHandle::WarpPoints;
        QVector<int> selected;
        QVector<float> model;
        QVector<CModelWarpKeyform> warp;
        QVector<CModelRotationKeyform> rotation;
        quint64 revision = 0;
    };
    bool CanEdit(const QString& id, int keyform, QString* error) const;
    bool CanEdit(const QString& id, const QString& type, int keyform, QString* error) const;
    static bool CanEditIn(const CModelDocument& document, const QString& id, const QString& type,
                          int keyform, QString* error);
    bool MakePose(const QString& id, const QString& type, int keyform, const QMap<QString, float>& base,
                  QMap<QString, float>& pose, QString* error) const;
    bool Publish(CModelDocument candidate, const QMap<QString, float>& pose, bool editing, QString* error);
    bool Ready(QString* error) const;
    CModelDocument* m_document = nullptr;
    Publisher m_publish;
    DefinitionPublisher m_publishDefinitions;
    StructurePublisher m_publishStructure;
    StructurePoseProvider m_structurePose;
    MeshPublisher m_publishMesh;
    PreviewEnder m_end;
    QString m_target;
    QString m_type;
    int m_keyform = -1;
    QMap<QString, float> m_pose;
    QByteArray m_source;
    bool m_publishing = false;
    std::optional<Drag> m_drag;
    std::optional<DeformerDrag> m_deformerDrag;
};
}

#endif
