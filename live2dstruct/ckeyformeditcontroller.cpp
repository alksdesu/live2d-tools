#include "ckeyformeditcontroller.h"
#include "cparentgraph.h"
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
    return materialField(edit.field) || edit.field == CModelEdit::KeyformPositions || edit.field == CModelEdit::WarpKeyforms ||
           edit.field == CModelEdit::RotationKeyforms || edit.field == CModelEdit::DeformerParent ||
           edit.field == CModelEdit::MaskIndices || edit.field == CModelEdit::MaskInverted ||
           edit.field == CModelEdit::ParameterMetadata || edit.field == CModelEdit::ParameterStructure;
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
    if (!CanEdit(id, type, keyform, error) || keyform < 0) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
    if (materialField(edit.field)) {
        if (!Ready(error)) return false;
        if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
        if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
    CModelDocument candidate = *m_document;
    CModelEdit command;
    if (!(undo ? candidate.PeekUndoEdit(&command) : candidate.PeekRedoEdit(&command)) ||
        !Handles(command))
        return fail(error, QStringLiteral("当前历史不是模型几何编辑"));
    if (!(undo ? candidate.UndoEdit() : candidate.RedoEdit()))
        return fail(error, QStringLiteral("无法恢复关键形历史"));
    if (command.field == CModelEdit::ParameterStructure) {
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
    if (forceStructure || candidate.ParameterStructure() != m_document->ParameterStructure()) {
        CModelEdit command;
        command.field = CModelEdit::ParameterStructure;
        command.runtime = std::make_shared<CModelEditRuntimeState>();
        return PublishStructure(std::move(candidate), command, true, true, error);
    }
    if (m_publishDefinitions) return PublishDefinitions(std::move(candidate), true, error);
    if (IsActive() || m_drag) return fail(error, QStringLiteral("请先退出关键形编辑再恢复会话"));
    return Publish(std::move(candidate), {}, false, error);
}

bool CKeyformEditController::PublishDefinitions(CModelDocument candidate, bool restoreSession, QString* error)
{
    if (!Ready(error)) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
    if (!m_publishStructure || candidate.SourceDigest() != m_source || !command.runtime)
        return fail(error, QStringLiteral("参数结构预览尚未连接或源模型已改变"));
    try {
        const auto* document = m_document;
        const auto revision = m_document->Revision();
        const QScopedValueRollback<bool> publishing(m_publishing, true);
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
            int count = 0;
            if (nextType == QLatin1String("artmesh")) {
                for (const auto& mesh : candidate.Drawables())
                    if (mesh.id == nextTarget) count = mesh.keyforms.size();
            } else {
                for (const auto& deformer : candidate.Deformers()) if (deformer.id == nextTarget)
                    count = deformer.type == CModelDeformer::Warp ? deformer.warpKeyforms.size() : deformer.rotationKeyforms.size();
            }
            for (int index = 0; index < count; ++index) {
                QMap<QString, float> corner;
                if (!candidate.KeyformParameterValues(nextTarget, nextType, index, corner)) break;
                bool matches = true;
                for (auto value = corner.cbegin(); value != corner.cend(); ++value)
                    if (!pose.contains(value.key()) || pose.value(value.key()) != value.value()) { matches = false; break; }
                if (matches) { nextKeyform = index; break; }
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
    if (m_drag || selected.isEmpty()) return fail(error, QStringLiteral("没有可拖动的顶点或拖拽尚未结束"));
    Drag drag;
    drag.start = *m_document;
    drag.local = m_document->DrawableKeyformPositions(m_target, m_keyform);
    QSet<int> unique;
    for (int index : selected) {
        if (index < 0 || index >= drag.local.size() / 2 || unique.contains(index))
            return fail(error, QStringLiteral("顶点选区无效"));
        unique.insert(index);
    }
    drag.selected = selected;
    if (!drag.mapping.Build(*m_document, m_target, m_keyform, m_pose, error) ||
        !drag.mapping.MatchesRuntime(runtimePositions, error) ||
        !drag.mapping.ToModel(drag.local, drag.model, error)) return false;
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
    QVector<float> model = m_drag->model;
    for (int index : m_drag->selected) {
        const double x = double(model[index * 2]) + delta.x(), y = double(model[index * 2 + 1]) + delta.y();
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() ||
            std::abs(y) > std::numeric_limits<float>::max()) return fail(error, QStringLiteral("拖拽坐标超出范围"));
        model[index * 2] = float(x);
        model[index * 2 + 1] = float(y);
    }
    QVector<float> inverse;
    if (!m_drag->mapping.FromModel(model, inverse, error)) return false;
    QVector<float> local = m_drag->local;
    for (int index : m_drag->selected) {
        local[index * 2] = inverse[index * 2];
        local[index * 2 + 1] = inverse[index * 2 + 1];
    }
    if (delta.isNull()) local = m_drag->local;
    if (local == m_document->DrawableKeyformPositions(m_target, m_keyform)) return true;
    CModelDocument candidate = m_drag->start;
    if (local != m_drag->local && !candidate.ExecuteEdit({m_target, QStringLiteral("artmesh"),
        CModelEdit::KeyformPositions, QVariant::fromValue(m_drag->local), QVariant::fromValue(local), m_keyform, false}))
        return fail(error, QStringLiteral("无法提交顶点拖拽"));
    if (!Publish(std::move(candidate), m_pose, true, error)) return false;
    m_drag->revision = m_document->Revision();
    return true;
}

bool CKeyformEditController::CancelDrag(QString* error)
{
    if (!m_drag) return true;
    if (!Ready(error) || m_document->Revision() != m_drag->revision)
        return fail(error, QStringLiteral("文档已改变，不能覆盖后续编辑"));
    if (m_document->DrawableKeyformPositions(m_target, m_keyform) != m_drag->local &&
        !Publish(m_drag->start, m_pose, true, error)) return false;
    m_drag.reset();
    return true;
}

void CKeyformEditController::FinishDrag() { if (!m_publishing) m_drag.reset(); }

bool CKeyformEditController::End(QString* error)
{
    if (!Ready(error)) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
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
