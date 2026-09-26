#include "LAppModel.hpp"
#include <QSet>
#include <cmath>
#include <exception>
#include <limits>

using namespace Live2D::Cubism::Framework;

namespace {
enum class PreviewContext { EditorPose, EditorBase, TimelineBase };

bool validSeed(const ParameterStructureSeed& seed)
{
    if (seed.mode == ParameterStructureSeed::Mode::Defaults) return true;
    if (seed.mode != ParameterStructureSeed::Mode::Values) return false;
    const auto validValue = [](const ParameterRuntimeValue& value) {
        return std::isfinite(value.current) && (!value.saved || std::isfinite(*value.saved)) &&
            std::isfinite(value.minimum) && std::isfinite(value.maximum) && value.minimum <= value.maximum;
    };
    return validValue(seed.runtime) && (!seed.editorEntry || validValue(*seed.editorEntry)) &&
        (!seed.editorPose || std::isfinite(*seed.editorPose)) &&
        (!seed.editorBase || std::isfinite(*seed.editorBase)) &&
        (!seed.timelineBase || std::isfinite(*seed.timelineBase));
}
}

bool LAppModel::CaptureParameterStructureSeeds(const QStringList& ids, ParameterStructureSeeds& result, QString* error) const
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || HasExportPreview()) {
        if (error) *error = QStringLiteral("当前模型状态不能保存参数恢复值");
        return false;
    }
    try {
        CubismModel::RuntimeValueSnapshot values;
        if (!_model->CaptureRuntimeValues(values, error)) return false;
        CubismModel::RuntimeValueSnapshot entry;
        if (HasEditorPreview() && !_model->RebaseRuntimeValues(*m_editorEntryState, entry, error)) return false;
        QHash<QString, int> indices;
        for (int i = 0; i < values.parameterIds.size(); ++i) indices.insert(values.parameterIds[i], i);
        ParameterStructureSeeds candidate;
        for (const auto& id : ids) {
            const auto index = indices.constFind(id);
            if (index == indices.cend() || candidate.contains(id)) {
                if (error) *error = QStringLiteral("恢复参数重复或不存在：%1").arg(id);
                return false;
            }
            ParameterStructureSeed seed;
            seed.mode = ParameterStructureSeed::Mode::Values;
            seed.runtime = values.Parameter(*index);
            if (HasEditorPreview()) {
                if (m_editorEntryState->parameterIds != values.parameterIds ||
                    !m_editorPose.contains(id) || !m_editorBaseParameters.contains(id)) {
                    if (error) *error = QStringLiteral("编辑参数基线不完整：%1").arg(id);
                    return false;
                }
                seed.editorEntry = entry.Parameter(*index);
                seed.editorPose = m_editorPose[id];
                seed.editorBase = m_editorBaseParameters[id];
            }
            if (_timelinePreviewActive) {
                if (!_timelineBaseParameters.contains(id)) {
                    if (error) *error = QStringLiteral("时间轴参数基线不完整：%1").arg(id);
                    return false;
                }
                seed.timelineBase = _timelineBaseParameters[id];
            }
            if (!validSeed(seed)) {
                if (error) *error = QStringLiteral("参数恢复值含非有限数据：%1").arg(id);
                return false;
            }
            candidate.insert(id, seed);
        }
        result.swap(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存参数结构恢复值");
        return false;
    }
}

bool LAppModel::ApplyParameterStructurePreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                               const ParameterStructureSeeds& addedSeeds,
                                               const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                               QString* error, const ParameterDefinitionPreparation& preparePreview)
{
    if (error) error->clear();
    if (HasExportPreview() || !_initialized || _updating || !_model || sourceDigest.isEmpty() ||
        sourceDigest != m_sourceMocDigest || bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("当前模型状态不允许修改参数结构");
        return false;
    }
    try {
        for (auto it = addedSeeds.cbegin(); it != addedSeeds.cend(); ++it) {
            if (!validSeed(it.value())) {
                if (error) *error = QStringLiteral("参数恢复值无效：%1").arg(it.key());
                return false;
            }
        }
        const QByteArray ownedBytes(bytes.constData(), bytes.size());
        CubismModel::RuntimeValueSnapshot sourceState;
        if (!_model->CaptureRuntimeValues(sourceState, error)) return false;
        QHash<QString, int> sourceIndices;
        for (int i = 0; i < sourceState.parameterIds.size(); ++i) sourceIndices.insert(sourceState.parameterIds[i], i);
        QMap<QString, int> nextIndices;
        QMap<QString, float> nextEditorPose, nextEditorBase, nextTimelineBase;
        std::unique_ptr<CubismModel::RuntimeValueSnapshot> nextEntry;
        const bool editing = HasEditorPreview();
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            const auto reject = [&](const QString& message) { if (detail) *detail = message; return false; };
            CubismModel::RuntimeValueSnapshot values;
            if (!candidate->CaptureRuntimeValues(values, detail)) return false;
            for (int i = 0; i < values.parameterIds.size(); ++i) nextIndices.insert(values.parameterIds[i], i);
            const auto bound = [&](int index, float value) {
                if (value < values.minimums[index]) return values.minimums[index];
                if (value > values.maximums[index]) return values.maximums[index];
                return value;
            };
            if (editing) {
                ParameterStructureSeeds entrySeeds = addedSeeds;
                for (auto it = entrySeeds.begin(); it != entrySeeds.end(); ++it)
                    if (it->mode == ParameterStructureSeed::Mode::Values && it->editorEntry) it->runtime = *it->editorEntry;
                nextEntry = std::make_unique<CubismModel::RuntimeValueSnapshot>();
                if (!candidate->MigrateRuntimeValues(*m_editorEntryState, *nextEntry, entrySeeds, detail)) return false;
            }
            const auto migrateMap = [&](const QMap<QString, float>& source, QMap<QString, float>& target,
                                        PreviewContext context) {
                for (auto it = source.cbegin(); it != source.cend(); ++it)
                    if (!sourceIndices.contains(it.key()) || !std::isfinite(it.value())) return false;
                for (int i = 0; i < values.parameterIds.size(); ++i) {
                    const QString& id = values.parameterIds[i];
                    float value = context == PreviewContext::EditorBase ? nextEntry->parameterValues[i] :
                        context == PreviewContext::TimelineBase && values.HasSavedParameter(i) ? values.savedParameters[i] : values.parameterValues[i];
                    bool rangeChanged = false;
                    const auto old = sourceIndices.constFind(id);
                    if (old != sourceIndices.cend()) {
                        if (!source.contains(id)) return false;
                        value = source[id];
                        rangeChanged = sourceState.minimums[*old] != values.minimums[i] || sourceState.maximums[*old] != values.maximums[i];
                    } else {
                        const auto seed = addedSeeds.constFind(id);
                        if (seed != addedSeeds.cend() && seed->mode == ParameterStructureSeed::Mode::Values) {
                            const auto& selected = context == PreviewContext::EditorPose ? seed->editorPose :
                                context == PreviewContext::EditorBase ? seed->editorBase : seed->timelineBase;
                            if (selected) value = *selected;
                            rangeChanged = seed->runtime.minimum != values.minimums[i] || seed->runtime.maximum != values.maximums[i];
                        }
                    }
                    if (!std::isfinite(value)) return false;
                    target.insert(id, rangeChanged ? bound(i, value) : value);
                }
                return true;
            };
            if ((editing && (!migrateMap(m_editorPose, nextEditorPose, PreviewContext::EditorPose) ||
                             !migrateMap(m_editorBaseParameters, nextEditorBase, PreviewContext::EditorBase))) ||
                (_timelinePreviewActive && !migrateMap(_timelineBaseParameters, nextTimelineBase, PreviewContext::TimelineBase)))
                return reject(QStringLiteral("参数编辑或时间轴基线无法迁移"));
            for (auto it = baseValues.cbegin(); it != baseValues.cend(); ++it) {
                const auto index = nextIndices.constFind(it.key());
                if (index == nextIndices.cend() || !std::isfinite(it.value()) ||
                    it.value() < values.minimums[*index] || it.value() > values.maximums[*index])
                    return reject(QStringLiteral("参数基线超出候选定义：%1").arg(it.key()));
                if (!values.SetSavedParameter(*index, it.value())) return false;
                if (!editing && !_timelinePreviewActive) values.parameterValues[*index] = it.value();
                if (_timelinePreviewActive) nextTimelineBase[it.key()] = it.value();
                if (editing) {
                    if (!nextEntry->SetSavedParameter(*index, it.value())) return false;
                    if (!_timelinePreviewActive) {
                        nextEntry->parameterValues[*index] = it.value();
                        nextEditorBase[it.key()] = it.value();
                    }
                }
            }
            if (editing && !keepEditorPreview) values = *nextEntry;
            if (!candidate->RestoreRuntimeValues(values, detail)) return false;
            if (editing && keepEditorPreview && !candidate->ApplyParameterPose(nextEditorPose, detail)) return false;
            return !preparePreview || preparePreview(*candidate, detail);
        };
        const auto policy = CubismModelUpdatePolicy::AllowClippingEdits | CubismModelUpdatePolicy::AllowParameterDefinitionEdits |
            CubismModelUpdatePolicy::AllowParameterStructureEdits;
        if (!ApplyPreviewTransaction(ownedBytes, sourceDigest, prepare, error, policy, addedSeeds)) return false;
        m_mapParaToIndex.swap(nextIndices);
        if (_timelinePreviewActive) _timelineBaseParameters.swap(nextTimelineBase);
        if (editing && keepEditorPreview) {
            m_editorEntryState.swap(nextEntry);
            m_editorPose.swap(nextEditorPose);
            m_editorBaseParameters.swap(nextEditorBase);
        } else if (editing) {
            m_editorEntryState.reset();
            m_editorPose.clear();
            m_editorBaseParameters.clear();
        }
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("参数结构修改失败，原模型与预览基线保持不变");
        return false;
    }
}
