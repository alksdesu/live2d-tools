#ifndef CTREEMANAGE_H
#define CTREEMANAGE_H
#include "live2dstruct/tree/c2dtree.h"
#include <QMap>
#include <QSet>
#include <memory>

namespace live2dstruct {
class CMocRuntimeSnapshot;
class CTreeManage
{
public:
    struct Snapshot {
        std::unique_ptr<C2DTree> tree;
        QSet<QString> unsupportedDeformers;
        void Swap(Snapshot& other) noexcept;
        const std::vector<C2DTreeNode*>& Deformers() const noexcept { return m_deformers; }
        const QByteArray& MocDigest() const noexcept { return m_digest; }
        const QMap<QString, float>& Pose() const noexcept { return m_pose; }
        quint64 PoseRevision() const noexcept { return m_poseRevision; }
        bool HasValidEvaluation() const noexcept { return m_evaluationValid; }

    private:
        friend class CTreeManage;
        QSet<QString> m_unsupportedBindings;
        QVector<QPair<QString, QString>> m_chainOrder;
        std::vector<C2DTreeNode*> m_deformers;
        std::vector<CParameterNode*> m_parameters;
        QByteArray m_digest;
        QMap<QString, float> m_pose;
        quint64 m_poseRevision = 0;
        bool m_evaluationValid = false;
    };

    static bool PrepareSnapshot(const QByteArray& bytes, const QMap<QString, float>& pose,
                                Snapshot& result, QString* error = nullptr);
    static bool PrepareSnapshot(const CMocRuntimeSnapshot& runtime, const QMap<QString, float>& pose,
                                Snapshot& result, QString* error = nullptr);
    static void SwapSnapshot(Snapshot& candidate) noexcept;
    static bool EvaluateSnapshot(Snapshot& snapshot, QString* error = nullptr);
    static bool RefreshCurrentSnapshot(QString* error = nullptr);
    static bool SyncPose(Snapshot& snapshot, const QMap<QString, float>& values,
                         bool& changed, QString* error = nullptr);
    static bool SyncCurrentPose(const QMap<QString, float>& values, bool& changed, QString* error = nullptr);
    static const Snapshot& CurrentSnapshot() noexcept;
    static const std::vector<C2DTreeNode*>& Deformers() noexcept;
    static C2DTree* GetInstance() noexcept;
    static const QSet<QString>& UnsupportedDeformers() noexcept;
    static void ReleaseInstance() noexcept;
    static bool InitOpenLive2d(const QString& name, QString* error = nullptr);

private:
    static bool PrepareSnapshotData(const QByteArray& bytes, const QByteArray* digest,
                                    const QMap<QString, float>& pose, Snapshot& result, QString* error);
    static Snapshot m_snapshot;
};
}
#endif // CTREEMANAGE_H
