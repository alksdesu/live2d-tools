#ifndef CKEYFORMEDITCONTROLLER_H
#define CKEYFORMEDITCONTROLLER_H

#include "cmodeldocument.h"
#include "ckeyformmappingcontext.h"
#include "cdeformereditcontext.h"
#include "cgeometrypairplanner.h"
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
    using DeformerPublisher = std::function<bool(const CModelDocument&, const CModelEdit&, bool, QString*)>;
    struct EditorState
    {
        QString target;
        QString type;
        int keyform = -1;
        QMap<QString, float> pose;
    };
    struct LogicalState {
        EditorState editor;
        QByteArray sourceDigest;
    };
    class PreparedLogicalState {
    public:
        bool IsValid() const { return m_owner != nullptr; }
    private:
        friend class CKeyformEditController;
        const CKeyformEditController* m_owner = nullptr;
        const CModelDocument* m_document = nullptr;
        quint64 m_revision = 0;
        LogicalState m_requested, m_expected;
    };

    bool CaptureLogicalState(LogicalState& result, QString* error = nullptr) const;
    bool PrepareLogicalState(const LogicalState& state, PreparedLogicalState& result, QString* error = nullptr) const;
    bool ValidatePreparedLogicalState(const PreparedLogicalState& state, QString* error = nullptr) const;
    void AdoptLogicalState(PreparedLogicalState& state) noexcept;

    void SetDocument(CModelDocument* document);
    void SetPublisher(Publisher publisher, PreviewEnder ender);
    void SetDefinitionPublisher(DefinitionPublisher publisher);
    void SetStructurePublisher(StructurePublisher publisher, StructurePoseProvider poseProvider = {});
    void SetMeshPublisher(MeshPublisher publisher);
    void SetDeformerPublisher(DeformerPublisher publisher);
    static bool ReadDeformerEditorState(const CModelEdit& command, bool forward,
                                        EditorState& state, QString* error = nullptr);
    bool Activate(const QString& id, int keyform, const QMap<QString, float>& currentValues, QString* error = nullptr);
    bool Activate(const QString& id, const QString& type, int keyform,
                  const QMap<QString, float>& currentValues, QString* error = nullptr);
    bool Preview(const QString& id, const QString& type, int keyform,
                 const QMap<QString, float>& currentValues, QString* error = nullptr);
    static bool Handles(const CModelEdit& edit);
    bool End(QString* error = nullptr);
    bool Submit(const CModelEdit& edit, QString* error = nullptr);
    bool Submit(const CModelEdit& edit, const QMap<QString, float>& currentValues,
                int previewKeyform, QString* error = nullptr);
    bool History(bool undo, CModelEdit& edit, QString* error = nullptr);
    bool Restore(CModelDocument candidate, QString* error = nullptr, bool forceStructure = false);
    bool BeginDrag(const QVector<int>& selected, const QVector<float>& runtimePositions, QString* error = nullptr);
    bool BeginWeightedDrag(const QVector<double>& weights, const QVector<float>& runtimePositions, QString* error = nullptr);
    bool DragBy(const QPointF& modelDelta, QString* error = nullptr);
    bool BeginDeformerDrag(DeformerHandle handle, const QVector<int>& selected, QString* error = nullptr);
    bool BeginWeightedWarpDrag(const QVector<double>& weights, QString* error = nullptr);
    bool MoveDeformerDrag(const QPointF& modelDelta, double angleDelta, QString* error = nullptr);
    bool CancelDrag(QString* error = nullptr);
    void FinishDrag();
    bool DragHasChanges() const;
    bool IsActive() const { return !m_target.isEmpty(); }
    bool IsPublishing() const { return m_publishing; }
    bool IsDragging() const { return m_drag.has_value() || m_deformerDrag.has_value(); }
    const QString& Target() const { return m_target; }
    const QString& Type() const { return m_type; }
    int Keyform() const { return m_keyform; }
    const QMap<QString, float>& Pose() const { return m_pose; }

private:
    bool ActivateState(const QString& id, const QString& type, int keyform,
                       const QMap<QString, float>& currentValues, bool requireEditable, QString* error);
    bool PublishDefinitions(CModelDocument candidate, bool restoreSession, QString* error);
    bool PublishStructure(CModelDocument candidate, const CModelEdit& command, bool forward,
                          bool restoreSession, QString* error);
    bool PublishMesh(CModelDocument candidate, const CModelEdit& command, bool forward, QString* error);
    bool PublishDeformer(CModelDocument candidate, const CModelEdit& command, bool forward, QString* error);
    struct Drag
    {
        CModelDocument start;
        CKeyformMappingContext mapping;
        QVector<float> local;
        QVector<float> model;
        QVector<int> selected;
        QVector<double> weights;
        quint64 revision = 0;
        std::optional<CGeometryPairPlanner::SynchronizationContext> paired;
    };
    struct DeformerDrag
    {
        CModelDocument start;
        CDeformerEditContext mapping;
        DeformerHandle handle = DeformerHandle::WarpPoints;
        QVector<int> selected;
        QVector<double> weights;
        QVector<float> model;
        QVector<CModelWarpKeyform> warp;
        QVector<CModelRotationKeyform> rotation;
        quint64 revision = 0;
        std::optional<CGeometryPairPlanner::SynchronizationContext> paired;
    };
    bool CanEdit(const QString& id, int keyform, QString* error) const;
    bool BeginDeformerDragState(DeformerHandle handle, const QVector<int>& selected,
                               const QVector<double>* weights, QString* error);
    bool CanEdit(const QString& id, const QString& type, int keyform, QString* error) const;
    static bool CanEditIn(const CModelDocument& document, const QString& id, const QString& type,
                          int keyform, QString* error);
    bool MakePose(const QString& id, const QString& type, int keyform, const QMap<QString, float>& base,
                  QMap<QString, float>& pose, QString* error) const;
    static bool PreparePairedEdit(const CModelDocument& document, const CModelEdit& edit,
                                  const QMap<QString, float>& pose, int driverKeyform,
                                  CModelEdit& result, QString* error);
    static bool PreparePairSynchronization(const CModelDocument& document, const QString& id, const QString& type,
                                           int keyform, const QMap<QString, float>& pose,
                                           std::optional<CGeometryPairPlanner::SynchronizationContext>& result, QString* error);
    static bool PreparePairedDrag(const CModelDocument& document, const CModelEdit& edit, int keyform,
                                  const CGeometryPairPlanner::SynchronizationContext& context,
                                  CModelEdit& result, QString* error);
    bool Publish(CModelDocument candidate, const QMap<QString, float>& pose, bool editing, QString* error);
    bool Ready(QString* error) const;
    CModelDocument* m_document = nullptr;
    Publisher m_publish;
    DefinitionPublisher m_publishDefinitions;
    StructurePublisher m_publishStructure;
    StructurePoseProvider m_structurePose;
    MeshPublisher m_publishMesh;
    DeformerPublisher m_publishDeformer;
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
