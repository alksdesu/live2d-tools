#include "cmodeldocument.h"
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool sameEntry(const CModelBlendShapeConstraintEntry& a, const CModelBlendShapeConstraintEntry& b)
{
    return a == b;
}

CModelBlendShapeConstraintState freeze(const CModelBlendShapeConstraintState& state)
{
    CModelBlendShapeConstraintState result;
    result.reserve(state.size());
    for (const auto& entry : state) {
        auto copy = entry;
        copy.sourceDigest = QByteArray(entry.sourceDigest.constData(), entry.sourceDigest.size());
        copy.parameterId = QString(entry.parameterId.constData(), entry.parameterId.size());
        copy.values = QVector<CMocBlendShapeConstraintBuilder::Value>(entry.values.cbegin(), entry.values.cend());
        result.append(std::move(copy));
    }
    return result;
}

bool sameKey(const CModelBlendShapeConstraintEntry& entry, int index)
{
    return entry.constraintIndex == index;
}
}

bool CModelDocument::CaptureBlendShapeConstraint(int constraintIndex, CModelBlendShapeConstraintEntry& result,
                                                 QString* error) const
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型快照"));
    CMocBlendShapeConstraintBuilder::Frame frame;
    if (!CMocBlendShapeConstraintBuilder::Capture(m_workingBaseBytes, {constraintIndex}, frame, error)) return false;
    for (const auto& entry : m_blendShapeConstraints)
        if (sameKey(entry, constraintIndex)) {
            if (entry.sourceDigest != frame.sourceDigest || entry.parameterIndex != frame.parameterIndex ||
                entry.parameterId != frame.parameterId ||
                entry.parameterMinimum != frame.parameterMinimum || entry.parameterMaximum != frame.parameterMaximum)
                return fail(error, QStringLiteral("融合限制曲线状态来源已失效"));
            result = entry;
            return true;
        }
    result = {constraintIndex, frame.sourceDigest, frame.parameterIndex, frame.parameterId,
              frame.parameterMinimum, frame.parameterMaximum, frame.values};
    return true;
}

bool CModelDocument::SetBlendShapeConstraints(const CModelBlendShapeConstraintState& state, QString* error)
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型快照"));
    if (state.size() > 100000) return fail(error, QStringLiteral("融合限制曲线条目数量超出限制"));
    const QByteArray sourceDigest = QCryptographicHash::hash(m_workingBaseBytes, QCryptographicHash::Sha256);
    QSet<int> indices;
    CModelBlendShapeConstraintState normalized;
    normalized.reserve(state.size());
    for (const auto& entry : state) {
        if (entry.constraintIndex < 0 || indices.contains(entry.constraintIndex) || entry.sourceDigest != sourceDigest ||
            entry.parameterIndex < 0 || entry.parameterId.isEmpty() || entry.parameterId.contains(QChar::Null) ||
            !std::isfinite(entry.parameterMinimum) || !std::isfinite(entry.parameterMaximum))
            return fail(error, QStringLiteral("融合限制曲线状态索引、来源或参数信息无效"));
        CMocBlendShapeConstraintBuilder::Frame frame;
        if (!CMocBlendShapeConstraintBuilder::Capture(m_workingBaseBytes, {entry.constraintIndex}, frame, error) ||
            frame.parameterIndex != entry.parameterIndex || frame.parameterId != entry.parameterId ||
            frame.parameterMinimum != entry.parameterMinimum || frame.parameterMaximum != entry.parameterMaximum) return fail(error, QStringLiteral("融合限制曲线状态与模型不匹配"));
        if (entry.values == frame.values) return fail(error, QStringLiteral("融合限制曲线状态未应用任何变化"));
        CMocBlendShapeConstraintBuilder::Request request{frame, entry.values};
        CMocBlendShapeConstraintBuilder::Result built;
        if (!CMocBlendShapeConstraintBuilder::Build(m_workingBaseBytes, request, built, error)) return false;
        indices.insert(entry.constraintIndex);
        normalized.append(entry);
    }
    CModelDocument candidate = *this;
    candidate.m_blendShapeConstraints = freeze(normalized);
    QByteArray preview;
    if (!candidate.BuildPreviewMoc(preview, error)) return false;
    if (candidate.m_blendShapeConstraints == m_blendShapeConstraints) return true;
    m_blendShapeConstraints = std::move(candidate.m_blendShapeConstraints);
    ++m_revision;
    return true;
}

bool CModelDocument::MakeBlendShapeConstraintEdit(const CModelBlendShapeConstraintEntry& before,
                                                  const QVector<CMocBlendShapeConstraintBuilder::Value>& values,
                                                  CModelEdit& edit, QString* error) const
{
    if (error) error->clear();
    CModelBlendShapeConstraintEntry current;
    if (!CaptureBlendShapeConstraint(before.constraintIndex, current, error) || !sameEntry(current, before) ||
        values == before.values) return fail(error, QStringLiteral("融合限制曲线来源已改变或没有变化"));
    CModelDocument candidate = *this;
    auto next = m_blendShapeConstraints;
    auto found = std::find_if(next.begin(), next.end(), [&](const auto& entry) { return sameKey(entry, before.constraintIndex); });
    CMocBlendShapeConstraintBuilder::Frame frame;
    if (!CMocBlendShapeConstraintBuilder::Capture(m_workingBaseBytes, {before.constraintIndex}, frame, error)) return false;
    if (values == frame.values) {
        if (found != next.end()) next.erase(found);
    } else {
        CModelBlendShapeConstraintEntry updated = before;
        updated.values = values;
        if (found == next.end()) next.append(updated); else *found = updated;
    }
    if (!candidate.SetBlendShapeConstraints(next, error)) return false;
    CModelEdit command{"blendShapeConstraints", "blendshape", CModelEdit::BlendShapeConstraints,
        QVariant::fromValue(m_blendShapeConstraints), QVariant::fromValue(candidate.m_blendShapeConstraints), -1, false};
    edit = std::move(command);
    return true;
}

bool CModelDocument::ApplyBlendShapeConstraintEdit(const CModelEdit& edit, bool forward, QString* error)
{
    if (edit.type != QLatin1String("blendshape") || edit.name != QLatin1String("blendShapeConstraints") || edit.index != -1 ||
        edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None || edit.freezeValue ||
        edit.meshRequest || !edit.meshRemaps.isEmpty() || edit.bindingKeysRequest || edit.bindingKeysBatchRequest || edit.deformerRequest ||
        edit.before.metaType() != QMetaType::fromType<CModelBlendShapeConstraintState>() ||
        edit.after.metaType() != QMetaType::fromType<CModelBlendShapeConstraintState>())
        return fail(error, QStringLiteral("融合限制曲线命令格式无效"));
    const auto expected = forward ? edit.before.value<CModelBlendShapeConstraintState>() : edit.after.value<CModelBlendShapeConstraintState>();
    const auto next = forward ? edit.after.value<CModelBlendShapeConstraintState>() : edit.before.value<CModelBlendShapeConstraintState>();
    if (m_blendShapeConstraints != expected) return fail(error, QStringLiteral("融合限制曲线操作期间已改变"));
    return SetBlendShapeConstraints(next, error);
}
}
