#include "ckeyformeditcontroller.h"
#include <QScopedValueRollback>
#include <cmath>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
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

bool CKeyformEditController::Ready(QString* error) const
{
    if (error) error->clear();
    if (!m_document || !m_document->IsLoaded() || m_document->SourceDigest() != m_source)
        return fail(error, QStringLiteral("编辑模型已改变，请重新选择关键形"));
    if (m_publishing) return fail(error, QStringLiteral("正在提交编辑，请稍后重试"));
    return true;
}

bool CKeyformEditController::CanEdit(const QString& id, int keyform, QString* error) const
{
    if (!Ready(error)) return false;
    const CModelDrawable* target = nullptr;
    for (const auto& mesh : m_document->Drawables()) if (mesh.id == id) { target = &mesh; break; }
    if (!target || keyform < 0 || keyform >= target->keyforms.size())
        return fail(error, QStringLiteral("网格或关键形不存在"));
    if (m_document->IsLocked(QStringLiteral("artmesh"), id))
        return fail(error, QStringLiteral("网格已锁定：%1").arg(id));
    const auto checkParts = [&](int index) {
        QSet<int> visited;
        while (index != -1) {
            if (index < 0 || index >= m_document->Parts().size() || visited.contains(index))
                return fail(error, QStringLiteral("部件父链无效"));
            visited.insert(index);
            const auto& part = m_document->Parts()[index];
            if (m_document->IsLocked(QStringLiteral("part"), part.id))
                return fail(error, QStringLiteral("所属部件已锁定：%1").arg(part.id));
            index = part.parentIndex;
        }
        return true;
    };
    if (!checkParts(target->parentPartIndex)) return false;
    QSet<int> visited;
    int parent = target->parentDeformerIndex;
    while (parent != -1) {
        if (parent < 0 || parent >= m_document->Deformers().size() || visited.contains(parent))
            return fail(error, QStringLiteral("变形器父链无效"));
        visited.insert(parent);
        const auto& deformer = m_document->Deformers()[parent];
        const auto type = deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        if (m_document->IsLocked(type, deformer.id))
            return fail(error, QStringLiteral("父变形器已锁定：%1").arg(deformer.id));
        if (!checkParts(deformer.parentPartIndex)) return false;
        parent = m_document->DeformerParentIndex(deformer.id);
    }
    return true;
}

bool CKeyformEditController::MakePose(const QString& id, int keyform, const QMap<QString, float>& base,
                                     QMap<QString, float>& pose, QString* error) const
{
    QMap<QString, float> result;
    for (const auto& parameter : m_document->Parameters())
        result.insert(parameter.id, base.value(parameter.id, m_document->ParameterValue(parameter.id)));
    QMap<QString, float> corner;
    if (!m_document->KeyformParameterValues(id, QStringLiteral("artmesh"), keyform, corner, error)) return false;
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) result.insert(it.key(), it.value());
    for (const auto& parameter : m_document->Parameters()) {
        const float value = result.value(parameter.id);
        const auto metadata = m_document->ParameterMetadata(parameter.id);
        if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum ||
            value < parameter.minimum || value > parameter.maximum)
            return fail(error, QStringLiteral("编辑姿态超出参数范围：%1").arg(parameter.id));
    }
    pose = std::move(result);
    return true;
}

bool CKeyformEditController::Activate(const QString& id, int keyform,
                                    const QMap<QString, float>& currentValues, QString* error)
{
    if (!CanEdit(id, keyform, error)) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
    QMap<QString, float> pose;
    if (!MakePose(id, keyform, currentValues, pose, error)) return false;
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
    if (edit.field != CModelEdit::KeyformPositions || edit.type != QStringLiteral("artmesh") ||
        !CanEdit(edit.name, edit.index, error)) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
    const auto current = m_document->DrawableKeyformPositions(edit.name, edit.index);
    if (!edit.before.canConvert<QVector<float>>() || edit.before.value<QVector<float>>() != current)
        return fail(error, QStringLiteral("关键形在编辑期间已改变，请重新操作"));
    auto pose = m_pose;
    const bool keepTarget = IsActive() && m_target == edit.name;
    if (!keepTarget && (!CanEdit(edit.name, previewKeyform, error) ||
        !MakePose(edit.name, previewKeyform, currentValues, pose, error))) return false;
    CModelDocument candidate = *m_document;
    CModelEdit command = edit;
    command.coalesce = false;
    if (!candidate.ExecuteEdit(command)) return fail(error, QStringLiteral("关键形编辑数据无效或没有变化"));
    if (!Publish(std::move(candidate), pose, true, error)) return false;
    if (!keepTarget) { m_target = edit.name; m_keyform = previewKeyform; }
    m_pose = std::move(pose);
    return true;
}

bool CKeyformEditController::History(bool undo, CModelEdit& edit, QString* error)
{
    if (!Ready(error)) return false;
    if (m_drag) return fail(error, QStringLiteral("请先结束当前顶点拖拽"));
    CModelDocument candidate = *m_document;
    CModelEdit command;
    if (!(undo ? candidate.PeekUndoEdit(&command) : candidate.PeekRedoEdit(&command)) ||
        command.field != CModelEdit::KeyformPositions)
        return fail(error, QStringLiteral("当前历史不是关键形编辑"));
    if (!(undo ? candidate.UndoEdit() : candidate.RedoEdit()))
        return fail(error, QStringLiteral("无法恢复关键形历史"));
    if (!Publish(std::move(candidate), m_pose, IsActive(), error)) return false;
    edit = std::move(command);
    return true;
}

bool CKeyformEditController::Restore(CModelDocument candidate, QString* error)
{
    if (IsActive() || m_drag) return fail(error, QStringLiteral("请先退出关键形编辑再恢复会话"));
    return Publish(std::move(candidate), {}, false, error);
}

bool CKeyformEditController::BeginDrag(const QVector<int>& selected, const QVector<float>& runtimePositions,
                                     QString* error)
{
    if (!IsActive() || !CanEdit(m_target, m_keyform, error)) return false;
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
    m_keyform = -1;
    m_pose.clear();
    return true;
}
}
