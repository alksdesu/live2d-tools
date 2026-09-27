#include "ctreemanage.h"
#include "ckeyformbindinggrid.h"
#include "cmocruntimesnapshot.h"

#include <QCryptographicHash>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool evaluated(C2DTreeNode* node)
{
    if (!node || node->GetCanvasPositions().empty()) return false;
    if (auto* warp = dynamic_cast<CWarpNode*>(node)) {
        CWarpCoorType transform(warp->GetColumns(), warp->GetRows(), warp->GetCurKeyForm().GetPosition(), warp->GetIs_quad_source());
        return transform.transformLocalToCanvas(nullptr, nullptr, 0, 0, 2);
    }
    if (auto* rotation = dynamic_cast<CRotationNode*>(node)) {
        CRotationCoorType transform(rotation->GetBaseAngles(), rotation->GetCurKeyForm());
        return transform.transformLocalToCanvas(nullptr, nullptr, 0, 0, 2);
    }
    return false;
}
}

CTreeManage::Snapshot CTreeManage::m_snapshot;

void CTreeManage::Snapshot::Swap(Snapshot& other) noexcept
{
    tree.swap(other.tree);
    unsupportedDeformers.swap(other.unsupportedDeformers);
    m_unsupportedBindings.swap(other.m_unsupportedBindings);
    m_chainOrder.swap(other.m_chainOrder);
    m_deformers.swap(other.m_deformers);
    m_parameters.swap(other.m_parameters);
    m_digest.swap(other.m_digest);
    m_pose.swap(other.m_pose);
    std::swap(m_poseRevision, other.m_poseRevision);
    std::swap(m_evaluationValid, other.m_evaluationValid);
}

bool CTreeManage::PrepareSnapshot(const QByteArray& bytes, const QMap<QString, float>& pose,
                                  Snapshot& result, QString* error)
{
    return PrepareSnapshotData(bytes, nullptr, pose, result, error);
}

bool CTreeManage::PrepareSnapshot(const CMocRuntimeSnapshot& runtime, const QMap<QString, float>& pose,
                                  Snapshot& result, QString* error)
{
    if (!runtime.IsValid()) return fail(error, QStringLiteral("运行时 MOC 快照尚未准备"));
    return PrepareSnapshotData(runtime.Bytes(), &runtime.Digest(), pose, result, error);
}

bool CTreeManage::PrepareSnapshotData(const QByteArray& bytes, const QByteArray* digest,
                                      const QMap<QString, float>& pose, Snapshot& result, QString* error)
{
    if (error) error->clear();
    try {
        CReader reader = CReader::FromBytes(bytes);
        CSectionTable section(reader);
        if (!section.IsValid()) return fail(error, reader.Error());
        if (pose.size() != section.parameters.size())
            return fail(error, QStringLiteral("变形器预览需要完整参数姿态"));
        QSet<QString> parameterIds;
        for (const auto* parameter : section.parameters) {
            const auto value = pose.constFind(parameter->ids);
            if (parameter->ids.isEmpty() || parameterIds.contains(parameter->ids) || value == pose.cend() ||
                !std::isfinite(value.value()))
                return fail(error, QStringLiteral("变形器预览参数无效：%1").arg(parameter->ids));
            parameterIds.insert(parameter->ids);
        }
        CKeyformBindingGrid bindings;
        if (!bindings.Build(section, error)) return false;
        Snapshot candidate;
        candidate.m_digest = digest ? *digest : QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const int count = section.deformers.size();
        QVector<int> firstChild(count, -1), nextSibling(count, -1), pending;
        QSet<QString> ids;
        pending.reserve(count);
        for (int index = 0; index < count; ++index) {
            const auto* deformer = section.deformers[index];
            const int parent = deformer->parent_deformer_indices;
            if (deformer->ids.isEmpty() || ids.contains(deformer->ids) || parent < -1 || parent >= count || parent == index)
                return fail(error, QStringLiteral("变形器标识或父级无效：%1").arg(deformer->ids));
            ids.insert(deformer->ids);
            const auto type = deformer->types == 0 ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
            if ((deformer->types != 0 && deformer->types != 1) || !bindings.Diagnostic(type, deformer->ids).isEmpty())
                candidate.unsupportedDeformers.insert(deformer->ids);
            if (parent == -1) pending.append(index);
            else {
                nextSibling[index] = firstChild[parent];
                firstChild[parent] = index;
            }
        }
        for (int next = 0; next < pending.size(); ++next) {
            const int parent = pending[next];
            const bool unsupported = candidate.unsupportedDeformers.contains(section.deformers[parent]->ids);
            for (int child = firstChild[parent]; child != -1; child = nextSibling[child]) {
                if (unsupported) candidate.unsupportedDeformers.insert(section.deformers[child]->ids);
                pending.append(child);
            }
        }
        if (pending.size() != count) return fail(error, QStringLiteral("变形器父链形成循环"));
        candidate.m_unsupportedBindings = candidate.unsupportedDeformers;
        candidate.m_chainOrder.reserve(count);
        for (int index : pending) {
            const auto* deformer = section.deformers[index];
            const int parent = deformer->parent_deformer_indices;
            candidate.m_chainOrder.append({deformer->ids, parent == -1 ? QString() : section.deformers[parent]->ids});
        }
        candidate.tree = std::make_unique<C2DTree>(section);
        QSet<QString> assigned;
        for (auto* node : candidate.tree->getParametersTree()) {
            auto* parameter = dynamic_cast<CParameterNode*>(node);
            if (!parameter || assigned.contains(parameter->getName())) continue;
            parameter->SetCurValue(pose.value(parameter->getName()));
            assigned.insert(parameter->getName());
            candidate.m_parameters.push_back(parameter);
        }
        if (!EvaluateSnapshot(candidate, error)) return false;
        candidate.Swap(result);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备变形器树，原预览保持不变"));
    }
}

bool CTreeManage::EvaluateSnapshot(Snapshot& snapshot, QString* error)
{
    if (error) error->clear();
    if (!snapshot.tree) return fail(error, QStringLiteral("变形器树尚未准备"));
    try {
        QMap<QString, float> pose;
        for (const auto* parameter : snapshot.m_parameters) {
            if (!std::isfinite(parameter->GetCurValue())) {
                snapshot.m_evaluationValid = false;
                return fail(error, QStringLiteral("变形器姿态包含无效参数：%1").arg(parameter->getName()));
            }
            pose.insert(parameter->getName(), parameter->GetCurValue());
        }
        snapshot.m_evaluationValid = false;
        auto nodes = snapshot.tree->GetWarpKeys();
        QSet<QString> unsupported = snapshot.m_unsupportedBindings;
        for (auto* node : nodes)
            if (node && !unsupported.contains(node->getName()) && !evaluated(node)) unsupported.insert(node->getName());
        for (const auto& entry : snapshot.m_chainOrder)
            if (unsupported.contains(entry.second)) unsupported.insert(entry.first);
        snapshot.m_deformers.swap(nodes);
        snapshot.unsupportedDeformers.swap(unsupported);
        snapshot.m_pose.swap(pose);
        ++snapshot.m_poseRevision;
        snapshot.m_evaluationValid = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器姿态求值失败"));
    }
}

bool CTreeManage::RefreshCurrentSnapshot(QString* error)
{
    try {
        QMap<QString, float> values;
        for (const auto* parameter : m_snapshot.m_parameters)
            values.insert(parameter->getName(), parameter->GetCurValue());
        bool changed = false;
        return SyncPose(m_snapshot, values, changed, error);
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法读取变形器姿态"));
    }
}

bool CTreeManage::SyncPose(Snapshot& snapshot, const QMap<QString, float>& values, bool& changed, QString* error)
{
    if (error) error->clear();
    changed = false;
    if (!snapshot.tree || values.size() != qsizetype(snapshot.m_parameters.size()))
        return fail(error, QStringLiteral("变形器预览需要完整参数姿态"));
    bool nodesMatch = true;
    for (const auto* parameter : snapshot.m_parameters) {
        const auto value = values.constFind(parameter->getName());
        if (value == values.cend() || !std::isfinite(value.value()))
            return fail(error, QStringLiteral("变形器预览参数无效：%1").arg(parameter->getName()));
        nodesMatch = nodesMatch && parameter->GetCurValue() == value.value();
    }
    if (snapshot.m_evaluationValid && nodesMatch && snapshot.m_pose == values) return true;
    try {
        std::vector<float> previous;
        previous.reserve(snapshot.m_parameters.size());
        for (const auto* parameter : snapshot.m_parameters) previous.push_back(parameter->GetCurValue());
        for (auto* parameter : snapshot.m_parameters) parameter->SetCurValue(values.value(parameter->getName()));
        if (!EvaluateSnapshot(snapshot, error)) {
            for (size_t index = 0; index < snapshot.m_parameters.size(); ++index)
                snapshot.m_parameters[index]->SetCurValue(previous[index]);
            return false;
        }
        changed = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器姿态同步失败"));
    }
}

bool CTreeManage::SyncCurrentPose(const QMap<QString, float>& values, bool& changed, QString* error)
{
    return SyncPose(m_snapshot, values, changed, error);
}

const CTreeManage::Snapshot& CTreeManage::CurrentSnapshot() noexcept
{
    return m_snapshot;
}

const std::vector<C2DTreeNode*>& CTreeManage::Deformers() noexcept
{
    return m_snapshot.Deformers();
}

void CTreeManage::SwapSnapshot(Snapshot& candidate) noexcept
{
    m_snapshot.Swap(candidate);
}

C2DTree* CTreeManage::GetInstance() noexcept
{
    return m_snapshot.tree.get();
}

const QSet<QString>& CTreeManage::UnsupportedDeformers() noexcept
{
    return m_snapshot.unsupportedDeformers;
}

void CTreeManage::ReleaseInstance() noexcept
{
    Snapshot empty;
    SwapSnapshot(empty);
}

bool CTreeManage::InitOpenLive2d(const QString& name, QString* error)
{
    try {
        CReader reader(name);
        CSectionTable section(reader);
        if (!section.IsValid()) return fail(error, reader.Error());
        QMap<QString, float> pose;
        for (const auto* parameter : section.parameters) pose.insert(parameter->ids, parameter->default_values);
        Snapshot candidate;
        if (!PrepareSnapshot(reader.m_byte, pose, candidate, error)) return false;
        SwapSnapshot(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法加载变形器树，原预览保持不变"));
    }
}
}
