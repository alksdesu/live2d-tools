#include "ckeyformeditcontroller.h"
#include "cparentgraph.h"
#include "cvertexselection.h"
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
bool materialField(CModelEdit::Field field)
{
    return field == CModelEdit::Visibility || field == CModelEdit::VisibilityValues || field == CModelEdit::Opacity ||
        field == CModelEdit::DrawOrder || field == CModelEdit::MeshIndices;
}

QHash<QString, bool> editableObjects(const CModelDocument& document)
{
    QVector<int> partParents, deformerParents;
    for (const auto& part : document.Parts()) partParents.append(part.parentIndex);
    for (const auto& deformer : document.Deformers())
        deformerParents.append(document.DeformerParentOverrides().value(deformer.id, deformer.parentDeformerIndex));
    const CParentGraph parts(partParents), deformers(deformerParents);
    QVector<quint8> partAllowed(partParents.size(), false), deformerAllowed(deformerParents.size(), false);
    const auto allowedPart = [&](int index) {
        return index == -1 || (index >= 0 && index < partAllowed.size() && partAllowed[index]);
    };
    const auto allowedDeformer = [&](int index) {
        return index == -1 || (index >= 0 && index < deformerAllowed.size() && deformerAllowed[index]);
    };
    for (int index : parts.TopologicalOrder())
        partAllowed[index] = allowedPart(partParents[index]) && !document.IsLocked(QStringLiteral("part"), document.Parts()[index].id);
    for (int index : deformers.TopologicalOrder()) {
        const auto& deformer = document.Deformers()[index];
        const auto type = deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        deformerAllowed[index] = allowedDeformer(deformerParents[index]) && allowedPart(deformer.parentPartIndex) &&
            !document.IsLocked(type, deformer.id);
    }
    QHash<QString, bool> result;
    const auto add = [&](const QString& type, const QString& id, bool allowed) {
        const auto key = type + QLatin1Char('|') + id;
        auto found = result.find(key);
        if (found != result.end()) found.value() = false;
        else result.insert(key, !id.isEmpty() && allowed);
    };
    for (int index = 0; index < document.Parts().size(); ++index)
        add(QStringLiteral("part"), document.Parts()[index].id, partAllowed[index]);
    for (int index = 0; index < document.Deformers().size(); ++index) {
        const auto& deformer = document.Deformers()[index];
        add(deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation"), deformer.id, deformerAllowed[index]);
    }
    for (const auto& mesh : document.Drawables())
        add(QStringLiteral("artmesh"), mesh.id, !document.IsLocked(QStringLiteral("artmesh"), mesh.id) &&
            allowedPart(mesh.parentPartIndex) && allowedDeformer(mesh.parentDeformerIndex));
    return result;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

int keyformAtPose(const CKeyformGridSnapshot& grid, const QMap<QString, float>& pose)
{
    if (!grid.axesValid || grid.keyformCount < 1 || grid.expectedKeyformCount != grid.keyformCount) return -1;
    qint64 local = 0, stride = 1;
    for (const auto& axis : grid.axes) {
        const auto value = pose.constFind(axis.parameterId);
        if (value == pose.cend() || axis.keys.isEmpty()) return -1;
        const auto key = std::lower_bound(axis.keys.cbegin(), axis.keys.cend(), *value);
        if (key == axis.keys.cend() || *key != *value || stride > grid.keyformCount / axis.keys.size()) return -1;
        local += stride * (key - axis.keys.cbegin());
        stride *= axis.keys.size();
    }
    QMap<QString, float> corner;
    return local < grid.keyformCount && grid.Pose(int(local), corner) ? int(local) : -1;
}

bool editsBindingOwner(const CMocParameterStructureBuilder::EditBindingKeysRequest& request,
                       const QString& target, const QString& type)
{
    using Owner = CMocParameterStructureBuilder::Owner;
    return request.owner.id == target &&
        ((request.owner.type == Owner::ArtMesh && type == QLatin1String("artmesh")) ||
         (request.owner.type == Owner::Warp && type == QLatin1String("warp")) ||
         (request.owner.type == Owner::Rotation && type == QLatin1String("rotation")));
}

int remapBindingKeyform(const CKeyformGridSnapshot& before, const CKeyformGridSnapshot& after,
                       const CMocParameterStructureBuilder::EditBindingKeysRequest& request,
                       bool forward, int local, QMap<QString, float>& pose)
{
    if (local < 0 || local >= before.keyformCount || request.axisIndex < 0 ||
        request.axisIndex >= before.axes.size() || before.axes.size() != after.axes.size()) return -1;
    const int oldCount = before.axes[request.axisIndex].keys.size();
    const int newCount = after.axes[request.axisIndex].keys.size();
    if (oldCount < 1 || newCount < 1) return -1;
    qint64 stride = 1;
    for (int axis = 0; axis < request.axisIndex; ++axis) {
        const auto count = before.axes[axis].keys.size();
        if (count < 1 || stride > before.keyformCount / count) return -1;
        stride *= count;
    }
    const int digit = (local / stride) % oldCount;
    int next = -1;
    if (forward) {
        for (int key = 0; key < request.keys.size(); ++key)
            if (request.keys[key].oldKeyIndex == digit) { next = key; break; }
    } else if (digit < request.keys.size()) next = request.keys[digit].oldKeyIndex;
    if (next < 0 || next >= newCount) return -1;
    const qint64 mapped = local % stride + next * stride + (local / (stride * oldCount)) * stride * newCount;
    QMap<QString, float> corner;
    if (mapped < 0 || mapped >= after.keyformCount || !after.Pose(int(mapped), corner)) return -1;
    for (auto value = corner.cbegin(); value != corner.cend(); ++value) pose[value.key()] = value.value();
    return int(mapped);
}
}

void CKeyformEditController::SetDocument(CModelDocument* document)
{
    if (m_publishing) return;
    m_document = document;
    m_target.clear();
    m_type.clear();
    m_keyform = -1;
    m_pose.clear();
    m_source = document ? document->SourceDigest() : QByteArray();
    m_drag.reset();
    m_deformerDrag.reset();
}

bool CKeyformEditController::CaptureLogicalState(LogicalState& result, QString* error) const
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前关键形拖拽"));
    try {
        if (IsActive()) {
            QMap<QString, float> pose;
            if (!MakePose(m_target, m_type, m_keyform, m_pose, pose, error)) return false;
            if (pose != m_pose) return fail(error, QStringLiteral("当前关键形逻辑姿态已失效"));
        }
        LogicalState candidate{{m_target, m_type, m_keyform, m_pose}, m_source};
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法保存关键形逻辑上下文")); }
}

bool CKeyformEditController::PrepareLogicalState(const LogicalState& state, PreparedLogicalState& result, QString* error) const
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前关键形拖拽"));
    if (state.sourceDigest != m_source) return fail(error, QStringLiteral("关键形逻辑上下文属于不同的模型"));
    try {
        const auto& editor = state.editor;
        if (editor.target.isEmpty()) {
            if (!editor.type.isEmpty() || editor.keyform != -1 || !editor.pose.isEmpty())
                return fail(error, QStringLiteral("空关键形逻辑上下文含残留姿态"));
        } else {
            QMap<QString, float> pose;
            if (!MakePose(editor.target, editor.type, editor.keyform, editor.pose, pose, error)) return false;
            if (pose != editor.pose) return fail(error, QStringLiteral("关键形逻辑姿态缺少参数或不在指定角点"));
        }
        PreparedLogicalState candidate;
        candidate.m_owner = this; candidate.m_document = m_document; candidate.m_revision = m_document->Revision();
        candidate.m_requested = state;
        if (!CaptureLogicalState(candidate.m_expected, error)) return false;
        result = std::move(candidate); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备关键形逻辑上下文，原结果保持不变")); }
}

bool CKeyformEditController::ValidatePreparedLogicalState(const PreparedLogicalState& state, QString* error) const
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前关键形拖拽"));
    const auto& expected = state.m_expected.editor;
    if (state.m_owner != this || state.m_document != m_document || state.m_revision != m_document->Revision() ||
        state.m_expected.sourceDigest != m_source || expected.target != m_target || expected.type != m_type ||
        expected.keyform != m_keyform || expected.pose != m_pose)
        return fail(error, QStringLiteral("关键形逻辑上下文在准备后已改变"));
    return true;
}

void CKeyformEditController::AdoptLogicalState(PreparedLogicalState& state) noexcept
{
    Q_ASSERT(state.m_owner == this && state.m_document == m_document);
    auto& editor = state.m_requested.editor;
    m_target.swap(editor.target); m_type.swap(editor.type); std::swap(m_keyform, editor.keyform);
    m_pose.swap(editor.pose); m_source.swap(state.m_requested.sourceDigest);
    state.m_owner = nullptr; state.m_document = nullptr;
}

void CKeyformEditController::SetPublisher(Publisher publisher, PreviewEnder ender)
{
    if (m_publishing) return;
    m_publish = std::move(publisher);
    m_end = std::move(ender);
}

void CKeyformEditController::SetDefinitionPublisher(DefinitionPublisher publisher)
{
    if (!m_publishing) m_publishDefinitions = std::move(publisher);
}

void CKeyformEditController::SetStructurePublisher(StructurePublisher publisher, StructurePoseProvider poseProvider)
{
    if (!m_publishing) {
        m_publishStructure = std::move(publisher);
        m_structurePose = std::move(poseProvider);
    }
}

void CKeyformEditController::SetMeshPublisher(MeshPublisher publisher)
{
    if (!m_publishing) m_publishMesh = std::move(publisher);
}

void CKeyformEditController::SetDeformerPublisher(DeformerPublisher publisher)
{
    if (!m_publishing) m_publishDeformer = std::move(publisher);
}

bool CKeyformEditController::ReadDeformerEditorState(const CModelEdit& command, bool forward,
                                                   EditorState& state, QString* error)
{
    if (error) error->clear();
    if (command.field != CModelEdit::DeformerStructure || !command.runtime)
        return fail(error, QStringLiteral("变形器结构命令缺少编辑状态"));
    const auto& stored = forward ? command.runtime->afterEditor : command.runtime->beforeEditor;
    if (stored.metaType() != QMetaType::fromType<QVariantMap>())
        return fail(error, QStringLiteral("变形器历史的编辑状态格式无效"));
    const auto values = stored.toMap();
    if (values.size() != 4 || values.value("target").metaType() != QMetaType::fromType<QString>() ||
        values.value("type").metaType() != QMetaType::fromType<QString>() ||
        values.value("keyform").metaType() != QMetaType::fromType<int>() ||
        values.value("pose").metaType() != QMetaType::fromType<QMap<QString, float>>())
        return fail(error, QStringLiteral("变形器历史的编辑状态内容无效"));
    EditorState candidate{values.value("target").toString(), values.value("type").toString(),
                          values.value("keyform").toInt(), values.value("pose").value<QMap<QString, float>>()};
    if (candidate.target.isEmpty()) {
        if (!candidate.type.isEmpty() || candidate.keyform != -1 || !candidate.pose.isEmpty())
            return fail(error, QStringLiteral("变形器历史的空编辑状态无效"));
    } else if ((candidate.type != QLatin1String("artmesh") && candidate.type != QLatin1String("warp") &&
                candidate.type != QLatin1String("rotation")) || candidate.keyform < 0 ||
               !std::all_of(candidate.pose.cbegin(), candidate.pose.cend(), [](float value) { return std::isfinite(value); }))
        return fail(error, QStringLiteral("变形器历史的编辑对象或姿态无效"));
    state = std::move(candidate);
    return true;
}

bool CKeyformEditController::Ready(QString* error) const
{
    if (error) error->clear();
    if (!m_document || !m_document->IsLoaded() || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("编辑模型已改变，请重新选择关键形"));
    if (m_publishing) return fail(error, QStringLiteral("正在提交编辑，请稍后重试"));
    return true;
}

bool CKeyformEditController::Handles(const CModelEdit& edit)
{
    return materialField(edit.field) || edit.field == CModelEdit::GeometryKeyforms ||
           edit.field == CModelEdit::KeyformPositions || edit.field == CModelEdit::WarpKeyforms ||
           edit.field == CModelEdit::RotationKeyforms || edit.field == CModelEdit::DeformerParent ||
           edit.field == CModelEdit::MaskIndices || edit.field == CModelEdit::MaskInverted ||
           edit.field == CModelEdit::ParameterMetadata || edit.field == CModelEdit::ParameterStructure ||
           edit.field == CModelEdit::MeshStructure || edit.field == CModelEdit::DeformerStructure ||
           edit.field == CModelEdit::BlendShapeGeometry;
}

bool CKeyformEditController::CanEdit(const QString& id, int keyform, QString* error) const
{
    return CanEdit(id, QStringLiteral("artmesh"), keyform, error);
}

bool CKeyformEditController::CanEdit(const QString& id, const QString& type, int keyform, QString* error) const
{
    return Ready(error) && CanEditIn(*m_document, id, type, keyform, error);
}

bool CKeyformEditController::CanEditIn(const CModelDocument& document, const QString& id, const QString& type,
                                      int keyform, QString* error)
{
    int parent = -1, part = -1, count = -1;
    if (type == QLatin1String("part")) {
        if (keyform != -1) return fail(error, QStringLiteral("部件没有可编辑的几何关键形"));
        for (int index = 0; index < document.Parts().size(); ++index) if (document.Parts()[index].id == id) {
            if (count >= 0) return fail(error, QStringLiteral("部件 ID 重复：%1").arg(id));
            part = index;
            count = 0;
        }
    } else if (type == QLatin1String("artmesh")) {
        for (const auto& mesh : document.Drawables()) if (mesh.id == id) {
            if (count >= 0) return fail(error, QStringLiteral("网格 ID 重复：%1").arg(id));
            parent = mesh.parentDeformerIndex;
            part = mesh.parentPartIndex;
            count = mesh.keyforms.size();
        }
    } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
        for (const auto& deformer : document.Deformers()) if (deformer.id == id) {
            if (count >= 0) return fail(error, QStringLiteral("变形器 ID 重复：%1").arg(id));
            if ((type == QLatin1String("warp")) != (deformer.type == CModelDeformer::Warp))
                return fail(error, QStringLiteral("变形器类型不匹配：%1").arg(id));
            parent = document.DeformerParentIndex(id);
            part = deformer.parentPartIndex;
            count = deformer.type == CModelDeformer::Warp ? deformer.warpKeyforms.size() : deformer.rotationKeyforms.size();
        }
    }
    if (count < 0 || keyform < -1 || keyform >= count)
        return fail(error, QStringLiteral("对象或关键形不存在"));
    if (document.IsLocked(type, id))
        return fail(error, QStringLiteral("对象已锁定：%1").arg(id));
    const auto checkParts = [&](int index) {
        QSet<int> visited;
        while (index != -1) {
            if (index < 0 || index >= document.Parts().size() || visited.contains(index))
                return fail(error, QStringLiteral("部件父链无效"));
            visited.insert(index);
            const auto& part = document.Parts()[index];
            if (document.IsLocked(QStringLiteral("part"), part.id))
                return fail(error, QStringLiteral("所属部件已锁定：%1").arg(part.id));
            index = part.parentIndex;
        }
        return true;
    };
    if (!checkParts(part)) return false;
    QSet<int> visited;
    while (parent != -1) {
        if (parent < 0 || parent >= document.Deformers().size() || visited.contains(parent))
            return fail(error, QStringLiteral("变形器父链无效"));
        visited.insert(parent);
        const auto& deformer = document.Deformers()[parent];
        const auto parentType = deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        if (document.IsLocked(parentType, deformer.id))
            return fail(error, QStringLiteral("父变形器已锁定：%1").arg(deformer.id));
        if (!checkParts(deformer.parentPartIndex)) return false;
        parent = document.DeformerParentIndex(deformer.id);
    }
    return true;
}

bool CKeyformEditController::MakePose(const QString& id, const QString& type, int keyform, const QMap<QString, float>& base,
                                     QMap<QString, float>& pose, QString* error) const
{
    QMap<QString, float> result;
    for (const auto& parameter : m_document->Parameters())
        result.insert(parameter.id, base.value(parameter.id, m_document->ParameterValue(parameter.id)));
    QMap<QString, float> corner;
    if (!m_document->KeyformParameterValues(id, type, keyform, corner, error)) return false;
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) result.insert(it.key(), it.value());
    for (const auto& parameter : m_document->Parameters()) {
        const float value = result.value(parameter.id);
        const auto metadata = m_document->ParameterMetadata(parameter.id);
        if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum)
            return fail(error, QStringLiteral("编辑姿态超出参数范围：%1").arg(parameter.id));
    }
    pose = std::move(result);
    return true;
}

bool CKeyformEditController::Activate(const QString& id, int keyform,
                                    const QMap<QString, float>& currentValues, QString* error)
{
    return Activate(id, QStringLiteral("artmesh"), keyform, currentValues, error);
}

bool CKeyformEditController::Activate(const QString& id, const QString& type, int keyform,
                                    const QMap<QString, float>& currentValues, QString* error)
{
    return ActivateState(id, type, keyform, currentValues, true, error);
}

bool CKeyformEditController::Preview(const QString& id, const QString& type, int keyform,
                                   const QMap<QString, float>& currentValues, QString* error)
{
    return ActivateState(id, type, keyform, currentValues, false, error);
}

bool CKeyformEditController::ActivateState(const QString& id, const QString& type, int keyform,
                                         const QMap<QString, float>& currentValues, bool requireEditable, QString* error)
{
    if (!Ready(error) || (requireEditable && !CanEdit(id, type, keyform, error))) return false;
    if (keyform < 0) return fail(error, QStringLiteral("请选择要预览的关键形"));
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    QMap<QString, float> pose;
    if (!MakePose(id, type, keyform, currentValues, pose, error)) return false;
    if (!m_publish) return fail(error, QStringLiteral("尚未连接模型预览"));
    const quint64 revision = m_document->Revision();
    const auto* document = m_document;
    const QScopedValueRollback<bool> publishing(m_publishing, true);
    try {
        if (!m_publish(*m_document, pose, true, error)) return false;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("编辑姿态准备失败"));
    }
    if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("提交期间文档已改变，请重新选择关键形"));
    m_target = id;
    m_type = type;
    m_keyform = keyform;
    m_pose = std::move(pose);
    return true;
}

bool CKeyformEditController::Publish(CModelDocument candidate, const QMap<QString, float>& pose,
                                   bool editing, QString* error)
{
    if (!Ready(error)) return false;
    if (!m_publish || candidate.SourceDigest() != m_source)
        return fail(error, QStringLiteral("编辑候选与加载快照不一致"));
    const quint64 revision = m_document->Revision();
    const auto* document = m_document;
    const QScopedValueRollback<bool> publishing(m_publishing, true);
    try {
        if (!m_publish(candidate, pose, editing, error)) return false;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("模型预览提交失败，编辑保持不变"));
    }
    if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("提交期间文档已改变，候选未写入"));
    m_document->CommitCandidate(std::move(candidate));
    return true;
}

bool CKeyformEditController::Submit(const CModelEdit& edit, QString* error)
{
    return Submit(edit, {}, edit.index, error);
}

bool CKeyformEditController::Submit(const CModelEdit& edit, const QMap<QString, float>& currentValues,
                                   int previewKeyform, QString* error)
{
    if (!Handles(edit)) return fail(error, QStringLiteral("当前修改不属于模型预览编辑"));
    if (edit.field == CModelEdit::BlendShapeGeometry) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        if (edit.before.metaType() != QMetaType::fromType<CModelBlendShapeGeometryState>() ||
            edit.after.metaType() != QMetaType::fromType<CModelBlendShapeGeometryState>() ||
            edit.before.value<CModelBlendShapeGeometryState>() != m_document->BlendShapeGeometry())
            return fail(error, QStringLiteral("融合几何数据在操作期间已改变，请重新操作"));
        CModelDocument candidate = *m_document;
        if (!candidate.ExecuteEdit(edit)) return fail(error, QStringLiteral("融合几何修改无效或没有变化"));
        return Publish(std::move(candidate), m_pose, IsActive(), error);
    }
    if (edit.field == CModelEdit::GeometryKeyforms) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        try {
            const auto stateType = QMetaType::fromType<CModelGeometryState>();
            if (edit.before.metaType() != stateType || edit.after.metaType() != stateType)
                return fail(error, QStringLiteral("几何批量命令的数据类型无效"));
            CModelGeometryKeyform::Type type;
            if (edit.type == QLatin1String("artmesh")) type = CModelGeometryKeyform::ArtMesh;
            else if (edit.type == QLatin1String("warp")) type = CModelGeometryKeyform::Warp;
            else if (edit.type == QLatin1String("rotation")) type = CModelGeometryKeyform::Rotation;
            else return fail(error, QStringLiteral("几何批量预览对象类型无效"));
            CModelGeometryKeyform driver;
            CModelEdit prepared;
            if (!m_document->CaptureGeometryKeyform(type, edit.name, edit.index, driver, error) ||
                !m_document->MakeGeometryEdit(edit.before.value<CModelGeometryState>(), edit.after.value<CModelGeometryState>(),
                                             driver, prepared, error)) return false;
            QMap<QString, float> pose;
            const bool keepTarget = IsActive() && m_target == edit.name && m_type == edit.type && m_keyform == edit.index;
            if (!MakePose(edit.name, edit.type, edit.index, keepTarget ? m_pose : currentValues, pose, error)) return false;
            CModelDocument candidate = *m_document;
            if (!candidate.ExecuteEdit(edit)) return fail(error, QStringLiteral("几何批量修改已改变或格式无效"));
            QString nextTarget = edit.name, nextType = edit.type;
            if (!Publish(std::move(candidate), pose, true, error)) return false;
            m_target.swap(nextTarget); m_type.swap(nextType); m_keyform = edit.index; m_pose.swap(pose);
            return true;
        } catch (const std::exception&) {
            return fail(error, QStringLiteral("无法准备几何批量修改，原文档和预览保持不变"));
        }
    }
    if (edit.field == CModelEdit::DeformerStructure) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        try {
            CModelDocument candidate = *m_document;
            CModelEdit command = edit;
            command.runtime = std::make_shared<CModelEditRuntimeState>();
            command.coalesce = false;
            if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("变形器结构修改无效或数据已改变"));
            return PublishDeformer(std::move(candidate), command, true, error);
        } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备变形器结构，原文档和预览保持不变")); }
    }
    if (edit.field == CModelEdit::MeshStructure) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        try {
            CModelDocument candidate = *m_document;
            CModelEdit command = edit;
            command.runtime = std::make_shared<CModelEditRuntimeState>();
            command.coalesce = false;
            if (!candidate.ExecuteEdit(command) || !candidate.PeekUndoEdit(&command))
                return fail(error, QStringLiteral("网格结构修改无效或数据已改变"));
            return PublishMesh(std::move(candidate), command, true, error);
        } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备网格结构，原文档和预览保持不变")); }
    }
    if (materialField(edit.field)) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        try {
            bool current = false;
            if (edit.field == CModelEdit::VisibilityValues) {
                if (edit.type != QLatin1String("objects") || edit.before.metaType() != QMetaType::fromType<QMap<QString, bool>>() ||
                    edit.after.metaType() != QMetaType::fromType<QMap<QString, bool>>())
                    return fail(error, QStringLiteral("批量可见性数据类型无效"));
                const auto before = edit.before.value<QMap<QString, bool>>();
                const auto after = edit.after.value<QMap<QString, bool>>();
                if (before.isEmpty() || before.keys() != after.keys())
                    return fail(error, QStringLiteral("批量可见性目标不一致"));
                const auto visible = m_document->VisibilityState();
                const auto editable = editableObjects(*m_document);
                for (auto it = before.cbegin(); it != before.cend(); ++it) {
                    if (!visible.contains(it.key()) || visible.value(it.key()) != it.value())
                        return fail(error, QStringLiteral("批量可见性在操作期间已改变"));
                    if (!editable.value(it.key(), false))
                        return fail(error, QStringLiteral("对象或祖先已锁定、父链无效：%1").arg(it.key()));
                }
                current = true;
            } else {
                if (edit.field != CModelEdit::Visibility && edit.type != QLatin1String("artmesh"))
                    return fail(error, QStringLiteral("网格属性与对象类型不匹配"));
                if (!CanEditIn(*m_document, edit.name, edit.type, -1, error)) return false;
                if (edit.field == CModelEdit::Visibility) {
                    const auto visible = m_document->VisibilityState();
                    const auto found = visible.constFind(edit.type + QLatin1Char('|') + edit.name);
                    current = edit.before.metaType() == QMetaType::fromType<bool>() && edit.after.metaType() == QMetaType::fromType<bool>() &&
                        found != visible.cend() && edit.before.toBool() == found.value();
                } else if (edit.field == CModelEdit::MeshIndices) {
                    current = edit.before.metaType() == QMetaType::fromType<QVector<quint16>>() &&
                        edit.after.metaType() == QMetaType::fromType<QVector<quint16>>() &&
                        edit.before.value<QVector<quint16>>() == m_document->DrawableIndices(edit.name);
                } else for (const auto& mesh : m_document->Drawables()) if (mesh.id == edit.name) {
                    if (edit.field == CModelEdit::Opacity)
                        current = edit.before.metaType() == QMetaType::fromType<float>() && edit.after.metaType() == QMetaType::fromType<float>() &&
                            edit.before.toFloat() == m_document->DrawableOpacity(mesh);
                    else current = edit.before.metaType() == QMetaType::fromType<int>() && edit.after.metaType() == QMetaType::fromType<int>() &&
                        edit.before.toInt() == m_document->DrawableDrawOrder(mesh);
                    break;
                }
            }
            if (!current) return fail(error, QStringLiteral("属性数据类型无效或已改变，请重新操作"));
            CModelDocument candidate = *m_document;
            if (!candidate.ExecuteEdit(edit)) return fail(error, QStringLiteral("属性修改无效或没有变化"));
            return Publish(std::move(candidate), m_pose, IsActive(), error);
        } catch (const std::exception&) {
            return fail(error, QStringLiteral("无法准备属性修改，原文档和预览保持不变"));
        }
    }
    if (edit.field == CModelEdit::ParameterMetadata || edit.field == CModelEdit::ParameterStructure) {
        if (!Ready(error)) return false;
        if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
        try {
            CModelDocument candidate = *m_document;
            CModelEdit command = edit;
            if (edit.field == CModelEdit::ParameterStructure) {
                command.runtime = std::make_shared<CModelEditRuntimeState>();
                command.coalesce = false;
            }
            if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("参数定义已改变、无变化或数据无效"));
            if (edit.field == CModelEdit::ParameterStructure)
                return PublishStructure(std::move(candidate), command, true, false, error);
            return PublishDefinitions(std::move(candidate), false, error);
        } catch (const std::exception&) {
            return fail(error, QStringLiteral("无法准备参数定义，原文档和预览保持不变"));
        }
    }
    const bool parentEdit = edit.field == CModelEdit::DeformerParent;
    const bool clippingEdit = edit.field == CModelEdit::MaskIndices || edit.field == CModelEdit::MaskInverted;
    const bool poseEdit = !parentEdit && !clippingEdit;
    const bool meshEdit = edit.field == CModelEdit::KeyformPositions;
    const QString expectedType = meshEdit || clippingEdit ? QStringLiteral("artmesh") :
        edit.field == CModelEdit::WarpKeyforms ? QStringLiteral("warp") :
        edit.field == CModelEdit::RotationKeyforms ? QStringLiteral("rotation") : edit.type;
    if (edit.type != expectedType || (parentEdit && edit.type != QLatin1String("warp") && edit.type != QLatin1String("rotation")))
        return fail(error, QStringLiteral("编辑数据与对象类型不匹配"));
    if (poseEdit && previewKeyform < 0) return fail(error, QStringLiteral("请选择要编辑的关键形"));
    if (!CanEdit(edit.name, edit.type, poseEdit ? previewKeyform : -1, error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    bool current = false;
    if (meshEdit) {
        current = edit.before.canConvert<QVector<float>>() &&
            edit.before.value<QVector<float>>() == m_document->DrawableKeyformPositions(edit.name, edit.index);
    } else if (edit.field == CModelEdit::WarpKeyforms) {
        current = edit.before.canConvert<QVector<CModelWarpKeyform>>() &&
            edit.before.value<QVector<CModelWarpKeyform>>() == m_document->DeformerWarpKeyforms(edit.name);
    } else if (edit.field == CModelEdit::RotationKeyforms) {
        current = edit.before.canConvert<QVector<CModelRotationKeyform>>() &&
            edit.before.value<QVector<CModelRotationKeyform>>() == m_document->DeformerRotationKeyforms(edit.name);
    } else if (edit.field == CModelEdit::MaskIndices) {
        current = edit.before.canConvert<QVector<int>>() &&
            edit.before.value<QVector<int>>() == m_document->DrawableMaskIndices(edit.name);
    } else if (edit.field == CModelEdit::MaskInverted) {
        current = edit.before.metaType().id() == QMetaType::Bool && edit.after.metaType().id() == QMetaType::Bool &&
            edit.before.toBool() == m_document->DrawableMaskInverted(edit.name);
    } else {
        bool valid = false;
        const int before = edit.before.toInt(&valid);
        current = valid && before == m_document->DeformerParentIndex(edit.name);
    }
    if (!current) return fail(error, QStringLiteral("编辑数据在操作期间已改变，请重新操作"));
    auto pose = m_pose;
    const bool keepTarget = IsActive() && m_target == edit.name && m_type == edit.type &&
        (meshEdit || m_keyform == previewKeyform);
    if (poseEdit && !keepTarget && !MakePose(edit.name, edit.type, previewKeyform, currentValues, pose, error)) return false;
    CModelDocument candidate = *m_document;
    CModelEdit command = edit;
    command.coalesce = false;
    if (!PreparePairedEdit(*m_document, command, pose, previewKeyform, command, error)) return false;
    if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("编辑数据无效或没有变化"));
    if (parentEdit && !CanEditIn(candidate, edit.name, edit.type, -1, error)) return false;
    if (!Publish(std::move(candidate), pose, poseEdit || IsActive(), error)) return false;
    if (poseEdit) {
        if (!keepTarget) { m_target = edit.name; m_type = edit.type; m_keyform = previewKeyform; }
        m_pose = std::move(pose);
    }
    return true;
}

bool CKeyformEditController::History(bool undo, CModelEdit& edit, QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    CModelDocument candidate = *m_document;
    CModelEdit command;
    if (!(undo ? candidate.PeekUndoEdit(&command) : candidate.PeekRedoEdit(&command)) ||
        !Handles(command))
        return fail(error, QStringLiteral("当前历史不是模型几何编辑"));
    if (!(undo ? candidate.UndoEdit() : candidate.RedoEdit()))
        return fail(error, QStringLiteral("无法恢复关键形历史"));
    if (command.field == CModelEdit::DeformerStructure) {
        if (!PublishDeformer(std::move(candidate), command, !undo, error)) return false;
    } else if (command.field == CModelEdit::MeshStructure) {
        if (!PublishMesh(std::move(candidate), command, !undo, error)) return false;
    } else if (command.field == CModelEdit::ParameterStructure) {
        if (!PublishStructure(std::move(candidate), command, !undo, false, error)) return false;
    } else if (command.field == CModelEdit::ParameterMetadata) {
        if (!PublishDefinitions(std::move(candidate), false, error)) return false;
    } else if (!Publish(std::move(candidate), m_pose, IsActive(), error)) return false;
    edit = std::move(command);
    return true;
}

bool CKeyformEditController::Restore(CModelDocument candidate, QString* error, bool forceStructure)
{
    if (!Ready(error)) return false;
    if (forceStructure || candidate.ParameterStructure() != m_document->ParameterStructure() ||
        candidate.TextureSources() != m_document->TextureSources() || candidate.TextureWorkspace() != m_document->TextureWorkspace() ||
        candidate.Artwork() != m_document->Artwork() ||
        candidate.HasMeshStructure() != m_document->HasMeshStructure() ||
        candidate.HasMaterialStructure() != m_document->HasMaterialStructure() ||
        candidate.DrawableMaterialOverrides() != m_document->DrawableMaterialOverrides() ||
        candidate.DeformerStructureState() != m_document->DeformerStructureState()) {
        CModelEdit command;
        command.field = CModelEdit::ParameterStructure;
        command.runtime = std::make_shared<CModelEditRuntimeState>();
        return PublishStructure(std::move(candidate), command, true, true, error);
    }
    if (m_publishDefinitions) return PublishDefinitions(std::move(candidate), true, error);
    if (IsActive() || IsDragging()) return fail(error, QStringLiteral("请先退出关键形编辑再恢复会话"));
    return Publish(std::move(candidate), {}, false, error);
}

bool CKeyformEditController::PublishMesh(CModelDocument candidate, const CModelEdit& command, bool forward, QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!m_publishMesh || !command.runtime || candidate.SourceDigest() != m_source)
        return fail(error, QStringLiteral("网格结构预览尚未连接或源模型已改变"));
    const auto* document = m_document;
    const auto revision = m_document->Revision();
    const QScopedValueRollback<bool> publishing(m_publishing, true);
    QScopedValueRollback<CModelEditRuntimeState> runtime(*command.runtime);
    try {
        if (!m_publishMesh(candidate, command, forward, error)) return false;
    } catch (const std::exception&) { return fail(error, QStringLiteral("网格预览提交失败，原文档和历史保持不变")); }
    if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("提交网格结构期间文档已改变"));
    m_document->CommitCandidate(std::move(candidate));
    runtime.commit();
    return true;
}

bool CKeyformEditController::PublishDeformer(CModelDocument candidate, const CModelEdit& command,
                                            bool forward, QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!m_publishDeformer || !command.runtime || candidate.SourceDigest() != m_source)
        return fail(error, QStringLiteral("变形器结构预览尚未连接或源模型已改变"));
    try {
        const auto* document = m_document;
        const auto revision = m_document->Revision();
        const QScopedValueRollback<bool> publishing(m_publishing, true);
        QScopedValueRollback<CModelEditRuntimeState> runtime(*command.runtime);
        const EditorState previous{m_target, m_type, m_keyform, m_pose};
        EditorState next = previous;
        const auto& archived = forward ? command.runtime->afterEditor : command.runtime->beforeEditor;
        const bool restoring = next.target.isEmpty() && archived.isValid();
        if (restoring && !ReadDeformerEditorState(command, forward, next, error)) return false;
        if (!next.target.isEmpty()) {
            bool exists = false;
            if (next.type == QLatin1String("artmesh")) {
                for (const auto& mesh : candidate.Drawables()) exists |= mesh.id == next.target;
            } else for (const auto& deformer : candidate.Deformers())
                exists |= deformer.id == next.target &&
                    (deformer.type == CModelDeformer::Warp ? next.type == QLatin1String("warp")
                                                         : next.type == QLatin1String("rotation"));
            if (!exists) {
                if (restoring) return fail(error, QStringLiteral("变形器历史中的编辑对象已失效"));
                next = {};
            } else {
                if (next.pose.size() != candidate.Parameters().size())
                    return fail(error, QStringLiteral("变形器编辑姿态参数不完整"));
                for (const auto& parameter : candidate.Parameters()) {
                    const auto value = next.pose.constFind(parameter.id);
                    const auto metadata = candidate.ParameterMetadata(parameter.id);
                    if (value == next.pose.cend() || !std::isfinite(*value) || *value < metadata.minimum || *value > metadata.maximum)
                        return fail(error, QStringLiteral("变形器编辑姿态参数无效：%1").arg(parameter.id));
                }
                CKeyformGridSnapshot grid;
                if (!candidate.DescribeKeyformGrid(next.target, next.type, grid, error)) return false;
                const int keyform = keyformAtPose(grid, next.pose);
                if (keyform < 0 || (restoring && keyform != next.keyform))
                    return fail(error, QStringLiteral("变形器历史中的编辑姿态不对应有效关键形"));
                next.keyform = keyform;
                if (!CanEditIn(candidate, next.target, next.type, next.keyform, error)) return false;
            }
        }
        const auto encode = [](const EditorState& state) {
            return QVariantMap{{QStringLiteral("target"), state.target}, {QStringLiteral("type"), state.type},
                {QStringLiteral("keyform"), state.keyform}, {QStringLiteral("pose"), QVariant::fromValue(state.pose)}};
        };
        (forward ? command.runtime->beforeEditor : command.runtime->afterEditor) = encode(previous);
        (forward ? command.runtime->afterEditor : command.runtime->beforeEditor) = encode(next);
        if (!m_publishDeformer(candidate, command, forward, error)) return false;
        if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
            return fail(error, QStringLiteral("发布变形器结构期间文档已改变"));
        m_document->CommitCandidate(std::move(candidate));
        runtime.commit();
        m_target.swap(next.target);
        m_type.swap(next.type);
        m_keyform = next.keyform;
        m_pose.swap(next.pose);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器结构准备失败，原文档和历史保持不变"));
    }
}

bool CKeyformEditController::PublishDefinitions(CModelDocument candidate, bool restoreSession, QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!m_publishDefinitions || candidate.SourceDigest() != m_source)
        return fail(error, QStringLiteral("参数定义预览尚未连接或源模型已改变"));
    try {
        QMap<QString, float> values;
        for (const auto& parameter : candidate.Parameters()) {
            const float value = candidate.ParameterValue(parameter.id);
            if (restoreSession || value != m_document->ParameterValue(parameter.id)) values.insert(parameter.id, value);
        }
        bool keepEditor = IsActive() && !restoreSession;
        QMap<QString, float> pose;
        if (keepEditor) {
            QMap<QString, float> corner;
            if (!candidate.KeyformParameterValues(m_target, m_type, m_keyform, corner)) keepEditor = false;
            else {
                for (const auto& parameter : candidate.Parameters()) {
                    const auto found = m_pose.constFind(parameter.id);
                    const auto metadata = candidate.ParameterMetadata(parameter.id);
                    if (found == m_pose.cend() || !std::isfinite(*found))
                        return fail(error, QStringLiteral("当前编辑姿态的参数不完整"));
                    pose.insert(parameter.id, std::clamp(*found, metadata.minimum, metadata.maximum));
                }
                for (auto value = corner.cbegin(); value != corner.cend(); ++value)
                    if (pose.value(value.key()) != value.value())
                        return fail(error, QStringLiteral("参数定义与当前编辑角点不一致"));
            }
        }
        const auto* document = m_document;
        const auto revision = m_document->Revision();
        const QScopedValueRollback<bool> publishing(m_publishing, true);
        if (!m_publishDefinitions(candidate, values, keepEditor, error)) return false;
        if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
            return fail(error, QStringLiteral("发布参数定义期间文档已改变"));
        m_document->CommitCandidate(std::move(candidate));
        if (keepEditor) m_pose.swap(pose);
        else {
            m_target.clear();
            m_type.clear();
            m_keyform = -1;
            m_pose.clear();
        }
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("参数定义准备失败，原文档和历史保持不变"));
    }
}

bool CKeyformEditController::PublishStructure(CModelDocument candidate, const CModelEdit& command,
                                            bool forward, bool restoreSession, QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!m_publishStructure || candidate.SourceDigest() != m_source || !command.runtime)
        return fail(error, QStringLiteral("参数结构预览尚未连接或源模型已改变"));
    try {
        const auto* document = m_document;
        const auto revision = m_document->Revision();
        const QScopedValueRollback<bool> publishing(m_publishing, true);
        QScopedValueRollback<CModelEditRuntimeState> runtime(*command.runtime);
        QString nextTarget = restoreSession ? QString() : m_target;
        QString nextType = restoreSession ? QString() : m_type;
        QMap<QString, float> selectedPose = m_pose;
        bool restoringSelection = false;
        QVariant capturedEditor = QVariantMap{{QStringLiteral("target"), m_target}, {QStringLiteral("type"), m_type},
            {QStringLiteral("pose"), QVariant::fromValue(m_pose)}};
        const auto& archivedEditor = forward ? command.runtime->afterEditor : command.runtime->beforeEditor;
        if (!restoreSession && nextTarget.isEmpty() && archivedEditor.isValid()) {
            if (archivedEditor.metaType() != QMetaType::fromType<QVariantMap>())
                return fail(error, QStringLiteral("参数历史的编辑选区格式无效"));
            const auto state = archivedEditor.toMap();
            if (state.size() != 3 || state.value(QStringLiteral("target")).metaType() != QMetaType::fromType<QString>() ||
                state.value(QStringLiteral("type")).metaType() != QMetaType::fromType<QString>() ||
                state.value(QStringLiteral("pose")).metaType() != QMetaType::fromType<QMap<QString, float>>())
                return fail(error, QStringLiteral("参数历史的编辑选区内容无效"));
            nextTarget = state.value(QStringLiteral("target")).toString();
            nextType = state.value(QStringLiteral("type")).toString();
            selectedPose = state.value(QStringLiteral("pose")).value<QMap<QString, float>>();
            if (nextTarget.isEmpty() && (!nextType.isEmpty() || !selectedPose.isEmpty()))
                return fail(error, QStringLiteral("参数历史的空编辑选区无效"));
            restoringSelection = !nextTarget.isEmpty();
            if (restoringSelection && ((nextType != QLatin1String("artmesh") && nextType != QLatin1String("warp") &&
                nextType != QLatin1String("rotation")) || selectedPose.size() != candidate.Parameters().size()))
                return fail(error, QStringLiteral("参数历史的编辑选区或姿态不完整"));
        }
        QSet<QString> previousIds;
        for (const auto& parameter : m_document->Parameters()) previousIds.insert(parameter.id);
        QMap<QString, float> values, pose;
        for (const auto& parameter : candidate.Parameters()) {
            if (restoringSelection && !selectedPose.contains(parameter.id))
                return fail(error, QStringLiteral("参数历史的编辑姿态缺少参数：%1").arg(parameter.id));
            const float value = candidate.ParameterValue(parameter.id);
            if (restoreSession || (previousIds.contains(parameter.id) && value != m_document->ParameterValue(parameter.id)))
                values.insert(parameter.id, value);
            const auto metadata = candidate.ParameterMetadata(parameter.id);
            const float current = selectedPose.value(parameter.id, metadata.defaultValue);
            if (!std::isfinite(current)) return fail(error, QStringLiteral("当前编辑姿态无效"));
            pose.insert(parameter.id, std::clamp(current, metadata.minimum, metadata.maximum));
        }
        int nextKeyform = -1;
        if (!nextTarget.isEmpty() && !restoreSession) {
            if (m_structurePose && !m_structurePose(candidate, command, forward, pose, error)) return false;
            if (pose.size() != candidate.Parameters().size()) return fail(error, QStringLiteral("恢复的编辑姿态参数不完整"));
            for (const auto& parameter : candidate.Parameters()) {
                const auto found = pose.constFind(parameter.id);
                const auto metadata = candidate.ParameterMetadata(parameter.id);
                if (found == pose.cend() || !std::isfinite(*found) || *found < metadata.minimum || *found > metadata.maximum)
                    return fail(error, QStringLiteral("恢复的编辑姿态无效：%1").arg(parameter.id));
            }
            CKeyformGridSnapshot grid;
            if (candidate.DescribeKeyformGrid(nextTarget, nextType, grid)) {
                if (!restoringSelection && command.bindingKeysRequest &&
                    editsBindingOwner(*command.bindingKeysRequest, nextTarget, nextType)) {
                    CKeyformGridSnapshot previous;
                    if (m_document->DescribeKeyformGrid(nextTarget, nextType, previous))
                        nextKeyform = remapBindingKeyform(previous, grid, *command.bindingKeysRequest,
                                                        forward, m_keyform, pose);
                } else nextKeyform = keyformAtPose(grid, pose);
            }
            if (nextKeyform >= 0 && !CanEditIn(candidate, nextTarget, nextType, nextKeyform, error)) return false;
            if (restoringSelection && nextKeyform < 0)
                return fail(error, QStringLiteral("参数历史中的编辑姿态不对应有效关键形"));
        }
        const bool keepEditor = nextKeyform >= 0;
        if (!m_publishStructure(candidate, command, forward, restoreSession, values, keepEditor, pose, error)) return false;
        if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
            return fail(error, QStringLiteral("发布参数结构期间文档已改变"));
        m_document->CommitCandidate(std::move(candidate));
        (forward ? command.runtime->beforeEditor : command.runtime->afterEditor).swap(capturedEditor);
        runtime.commit();
        if (keepEditor) {
            m_target.swap(nextTarget);
            m_type.swap(nextType);
            m_keyform = nextKeyform;
            m_pose.swap(pose);
        } else {
            m_target.clear();
            m_type.clear();
            m_keyform = -1;
            m_pose.clear();
        }
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("参数结构准备失败，原文档和历史保持不变"));
    }
}

bool CKeyformEditController::BeginDrag(const QVector<int>& selected, const QVector<float>& runtimePositions,
                                     QString* error)
{
    if (!IsActive() || m_type != QLatin1String("artmesh") || !CanEdit(m_target, m_keyform, error)) return false;
    QVector<double> weights(m_document->DrawableKeyformPositions(m_target, m_keyform).size() / 2, 0);
    QSet<int> unique;
    for (int index : selected) {
        if (index < 0 || index >= weights.size() || unique.contains(index))
            return fail(error, QStringLiteral("顶点选区无效"));
        unique.insert(index);
        weights[index] = 1;
    }
    return BeginWeightedDrag(weights, runtimePositions, error);
}

bool CKeyformEditController::BeginWeightedDrag(const QVector<double>& weights,
                                             const QVector<float>& runtimePositions, QString* error)
{
    if (!IsActive() || m_type != QLatin1String("artmesh") || !CanEdit(m_target, m_keyform, error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    Drag drag;
    drag.start = *m_document;
    drag.local = m_document->DrawableKeyformPositions(m_target, m_keyform);
    drag.weights = QVector<double>(weights.cbegin(), weights.cend());
    if (!CVertexSelection::Validate({{}, drag.weights}, drag.local.size() / 2, error)) return false;
    for (int index = 0; index < drag.weights.size(); ++index)
        if (drag.weights[index] > 0) drag.selected.append(index);
    if (drag.selected.isEmpty()) return fail(error, QStringLiteral("没有可拖动的顶点"));
    if (!drag.mapping.Build(*m_document, m_target, m_keyform, m_pose, error) ||
        !drag.mapping.MatchesRuntime(runtimePositions, error) ||
        !drag.mapping.ToModel(drag.local, drag.model, error)) return false;
    if (!PreparePairSynchronization(drag.start, m_target, m_type, m_keyform, m_pose, drag.paired, error)) return false;
    drag.revision = m_document->Revision();
    m_drag = std::move(drag);
    return true;
}

bool CKeyformEditController::DragBy(const QPointF& delta, QString* error)
{
    if (!Ready(error) || !m_drag || !CanEdit(m_target, m_keyform, error)) return false;
    if (m_drag->revision != m_document->Revision())
        return fail(error, QStringLiteral("拖拽期间文档已改变，请结束本次操作"));
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()))
        return fail(error, QStringLiteral("拖拽坐标无效"));
    QVector<float> local = m_drag->local;
    if (!delta.isNull()) {
        QVector<float> model, inverse;
        if (!CVertexSelection::Translate(m_drag->model, m_drag->weights, delta, model, error) ||
            !m_drag->mapping.FromModel(model, inverse, error)) return false;
        for (int index : m_drag->selected) {
            local[index * 2] = inverse[index * 2];
            local[index * 2 + 1] = inverse[index * 2 + 1];
        }
    }
    if (local == m_document->DrawableKeyformPositions(m_target, m_keyform)) return true;
    CModelDocument candidate = m_drag->start;
    if (local != m_drag->local) {
        CModelEdit command{m_target, QStringLiteral("artmesh"), CModelEdit::KeyformPositions,
            QVariant::fromValue(m_drag->local), QVariant::fromValue(local), m_keyform, false};
        if (m_drag->paired && !PreparePairedDrag(m_drag->start, command, m_keyform, *m_drag->paired, command, error)) return false;
        if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("无法提交顶点拖拽"));
    }
    if (!Publish(std::move(candidate), m_pose, true, error)) return false;
    m_drag->revision = m_document->Revision();
    return true;
}

bool CKeyformEditController::DragHasChanges() const
{
    if (!m_document || !m_document->IsLoaded() || m_document->SourceDigest() != m_source) return false;
    if (m_deformerDrag) {
        return m_type == QLatin1String("warp")
            ? m_document->DeformerWarpKeyforms(m_target) != m_deformerDrag->warp
            : m_document->DeformerRotationKeyforms(m_target) != m_deformerDrag->rotation;
    }
    return m_drag && m_document->DrawableKeyformPositions(m_target, m_keyform) != m_drag->local;
}

bool CKeyformEditController::CancelDrag(QString* error)
{
    if (m_deformerDrag) {
        if (!Ready(error) || m_document->Revision() != m_deformerDrag->revision)
            return fail(error, QStringLiteral("文档已改变，不能覆盖后续编辑"));
        if (DragHasChanges() && !Publish(m_deformerDrag->start, m_pose, true, error)) return false;
        m_deformerDrag.reset();
        return true;
    }
    if (!m_drag) return true;
    if (!Ready(error) || m_document->Revision() != m_drag->revision)
        return fail(error, QStringLiteral("文档已改变，不能覆盖后续编辑"));
    if (DragHasChanges() && !Publish(m_drag->start, m_pose, true, error)) return false;
    m_drag.reset();
    return true;
}

bool CKeyformEditController::BeginDeformerDrag(DeformerHandle handle, const QVector<int>& selected, QString* error)
{
    return BeginDeformerDragState(handle, selected, nullptr, error);
}

bool CKeyformEditController::BeginWeightedWarpDrag(const QVector<double>& weights, QString* error)
{
    return BeginDeformerDragState(DeformerHandle::WarpPoints, {}, &weights, error);
}

bool CKeyformEditController::BeginDeformerDragState(DeformerHandle handle, const QVector<int>& selected,
                                                 const QVector<double>* weights, QString* error)
{
    if (!IsActive() || !CanEdit(m_target, m_type, m_keyform, error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    const bool warp = handle == DeformerHandle::WarpPoints;
    if ((warp && m_type != QLatin1String("warp")) ||
        (!warp && (m_type != QLatin1String("rotation") ||
                   (handle != DeformerHandle::RotationOrigin && handle != DeformerHandle::RotationAngle))))
        return fail(error, QStringLiteral("手柄与当前变形器类型不匹配"));
    if (!warp && !selected.isEmpty()) return fail(error, QStringLiteral("旋转手柄不接受控制点选区"));
    DeformerDrag drag;
    drag.start = *m_document;
    drag.handle = handle;
    if (!drag.mapping.Build(*m_document, m_target, m_type, m_keyform, m_pose, error)) return false;
    drag.model = drag.mapping.ModelPoints();
    if (warp) {
        if (weights) {
            drag.weights = QVector<double>(weights->cbegin(), weights->cend());
            if (!CVertexSelection::Validate({{}, drag.weights}, drag.model.size() / 2, error)) return false;
            for (int index = 0; index < drag.weights.size(); ++index)
                if (drag.weights[index] > 0) drag.selected.append(index);
        } else {
            drag.weights.fill(0, drag.model.size() / 2);
            QSet<int> unique;
            for (int index : selected) {
                if (index < 0 || index >= drag.weights.size() || unique.contains(index))
                    return fail(error, QStringLiteral("控制点选区无效"));
                unique.insert(index);
                drag.weights[index] = 1;
            }
            drag.selected = selected;
        }
        if (drag.selected.isEmpty()) return fail(error, QStringLiteral("请选择要移动的控制点"));
        drag.warp = m_document->DeformerWarpKeyforms(m_target);
    } else drag.rotation = m_document->DeformerRotationKeyforms(m_target);
    if (!PreparePairSynchronization(drag.start, m_target, m_type, m_keyform, m_pose, drag.paired, error)) return false;
    drag.revision = m_document->Revision();
    m_deformerDrag = std::move(drag);
    return true;
}

bool CKeyformEditController::MoveDeformerDrag(const QPointF& delta, double angleDelta, QString* error)
{
    if (!Ready(error) || !m_deformerDrag || !CanEdit(m_target, m_type, m_keyform, error)) return false;
    auto& drag = *m_deformerDrag;
    if (drag.revision != m_document->Revision())
        return fail(error, QStringLiteral("拖拽期间文档已改变，请结束本次操作"));
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()) || !std::isfinite(angleDelta))
        return fail(error, QStringLiteral("拖拽坐标或角度无效"));
    const bool angular = drag.handle == DeformerHandle::RotationAngle;
    if ((angular && !delta.isNull()) || (!angular && angleDelta != 0))
        return fail(error, QStringLiteral("拖拽量与手柄类型不匹配"));
    QVector<float> local;
    if (!angular && !delta.isNull()) {
        QVector<float> points;
        const auto append = [&](int index, double weight) {
            const double x = double(drag.model[index * 2]) + delta.x() * weight;
            const double y = double(drag.model[index * 2 + 1]) + delta.y() * weight;
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
                std::abs(y) > std::numeric_limits<float>::max()) return false;
            points.append(float(x)); points.append(float(y));
            return true;
        };
        if (drag.handle == DeformerHandle::WarpPoints) {
            for (int index : drag.selected)
                if (!append(index, drag.weights[index])) return fail(error, QStringLiteral("控制点坐标超出范围"));
        } else if (!append(0, 1)) return fail(error, QStringLiteral("旋转中心坐标超出范围"));
        if (!drag.mapping.FromModel(points, local, error)) return false;
    }
    CModelEdit command;
    command.name = m_target; command.type = m_type; command.index = m_keyform; command.coalesce = false;
    if (drag.handle == DeformerHandle::WarpPoints) {
        auto forms = drag.warp;
        if (!delta.isNull()) for (int index = 0; index < drag.selected.size(); ++index) {
            const int vertex = drag.selected[index];
            forms[m_keyform].positions[vertex * 2] = local[index * 2];
            forms[m_keyform].positions[vertex * 2 + 1] = local[index * 2 + 1];
        }
        if (forms == m_document->DeformerWarpKeyforms(m_target)) return true;
        command.field = CModelEdit::WarpKeyforms;
        command.before = QVariant::fromValue(drag.warp); command.after = QVariant::fromValue(forms);
    } else {
        auto forms = drag.rotation;
        if (angular) {
            const double angle = double(forms[m_keyform].angle) + angleDelta;
            if (!std::isfinite(angle) || std::abs(angle) > std::numeric_limits<float>::max())
                return fail(error, QStringLiteral("旋转角度超出范围"));
            forms[m_keyform].angle = float(angle);
        } else if (!delta.isNull()) {
            forms[m_keyform].originX = local[0]; forms[m_keyform].originY = local[1];
        }
        if (forms == m_document->DeformerRotationKeyforms(m_target)) return true;
        command.field = CModelEdit::RotationKeyforms;
        command.before = QVariant::fromValue(drag.rotation); command.after = QVariant::fromValue(forms);
    }
    CModelDocument candidate = drag.start;
    if (command.before != command.after) {
        if (drag.paired && !PreparePairedDrag(drag.start, command, m_keyform, *drag.paired, command, error)) return false;
        if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("无法提交变形器拖拽"));
    }
    if (!Publish(std::move(candidate), m_pose, true, error)) return false;
    drag.revision = m_document->Revision();
    return true;
}

void CKeyformEditController::FinishDrag()
{
    if (!m_publishing) { m_drag.reset(); m_deformerDrag.reset(); }
}

bool CKeyformEditController::End(QString* error)
{
    if (!Ready(error)) return false;
    if (IsDragging()) return fail(error, QStringLiteral("请先结束当前画布拖拽"));
    if (!IsActive()) return true;
    const auto* document = m_document;
    const auto revision = m_document->Revision();
    const QScopedValueRollback<bool> publishing(m_publishing, true);
    try {
        if (m_end && !m_end(error)) return false;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("恢复编辑前姿态失败"));
    }
    if (m_document != document || m_document->Revision() != revision || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("恢复期间文档已改变，请重新选择关键形"));
    m_target.clear();
    m_type.clear();
    m_keyform = -1;
    m_pose.clear();
    return true;
}
}
