#include "LAppModel.hpp"
#include <QSet>
#include <algorithm>
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

bool LAppModel::CaptureEditorPreviewContext(EditorPreviewContext& result, QString* error) const
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || HasExportPreview()) {
        if (error) *error = QStringLiteral("当前模型状态不能保存编辑上下文");
        return false;
    }
    try {
        EditorPreviewContext candidate;
        candidate.sourceDigest = m_sourceMocDigest;
        candidate.active = HasEditorPreview();
        if (!_model->CaptureRuntimeValues(candidate.current, error)) return false;
        if (candidate.active) {
            candidate.entry = *m_editorEntryState;
            candidate.pose = m_editorPose;
            candidate.base = m_editorBaseParameters;
            CubismModel::RuntimeValueSnapshot checked;
            if (!_model->RebaseRuntimeValues(candidate.entry, checked, error)) return false;
        }
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存编辑上下文");
        return false;
    }
}

bool LAppModel::ApplyParameterStructurePreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                                               const ParameterStructureSeeds& addedSeeds,
                                               const QMap<QString, float>& baseValues, bool keepEditorPreview,
                                               QString* error, const ParameterDefinitionPreparation& preparePreview,
                                               const ParameterReferenceState* references, const EditorPreviewContext* restoreEditor,
                                               const TimelinePreviewPreparation& prepareTimeline, const PhysicsDraft* physicsDraft,
                                               const CMeshGeometryMigration* geometry,
                                               const QMap<QString, float>* requestedEditorPose, const TextureState* textureState,
                                               const QSet<QString>* materialDrawableIds, const AtlasState* atlasState)
{
    if (error) error->clear();
    if (HasExportPreview() || !_initialized || _updating || !_model || sourceDigest.isEmpty() ||
        sourceDigest != m_sourceMocDigest || bytes.isEmpty() || bytes.size() > std::numeric_limits<int>::max() ||
        (textureState && atlasState) ||
        ((textureState || atlasState || materialDrawableIds) && (m_physicsDragActive || m_userParameterSnapshotActive))) {
        if (error) *error = QStringLiteral("当前模型状态不允许修改参数结构");
        return false;
    }
    if (geometry && (m_physicsDragActive || geometry->sourcePreviewDigest.size() != 32 ||
                     geometry->sourcePreviewDigest != m_previewMocDigest)) {
        if (error) *error = QStringLiteral("会话顶点迁移与当前预览版本不一致，或画布仍在交互中");
        return false;
    }
    try {
        const auto requestedPose = requestedEditorPose ? std::make_unique<QMap<QString, float>>(*requestedEditorPose) : nullptr;
        const auto restoredContext = restoreEditor ? std::make_unique<EditorPreviewContext>(*restoreEditor) : nullptr;
        if (restoredContext && (restoredContext->sourceDigest != sourceDigest || restoredContext->active != keepEditorPreview)) {
            if (error) *error = QStringLiteral("恢复的编辑上下文来源或模式不匹配");
            return false;
        }
        if (requestedPose && (!keepEditorPreview || (!HasEditorPreview() && !(restoredContext && restoredContext->active)))) {
            if (error) *error = QStringLiteral("指定编辑姿态需要保留现有或恢复的编辑上下文");
            return false;
        }
        const auto referenceCandidate = references ? std::make_unique<ParameterReferenceState>(*references) : nullptr;
        std::function<void()> commitReferences;
        if (referenceCandidate && !PrepareParameterReferences(*referenceCandidate, commitReferences, error)) return false;
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
        PreparedTimelinePreview nextTimeline{_timelinePreview, m_timelineExplicitParameterIds, _timelinePreviewActive};
        QMap<QString, float> nextTimelineParameters = _timelineParameters;
        QMap<QString, float> nextTimelineParts = _timelineBaseParts;
        float nextTimelineOpacity = _timelineBaseOpacity;
        std::unique_ptr<CubismModel::RuntimeValueSnapshot> nextEntry;
        const bool restoreActive = restoredContext && restoredContext->active;
        const bool editing = restoreActive || HasEditorPreview();
        const auto* editorEntry = restoreActive ? &restoredContext->entry : m_editorEntryState.get();
        const auto& editorPose = restoreActive ? restoredContext->pose : m_editorPose;
        const auto& editorBase = restoreActive ? restoredContext->base : m_editorBaseParameters;
        QHash<QString, int> editorIndices;
        if (restoreActive) for (int index = 0; index < editorEntry->parameterIds.size(); ++index)
            editorIndices.insert(editorEntry->parameterIds[index], index);
        const auto prepare = [&](CubismModel* candidate, QString* detail) {
            const auto reject = [&](const QString& message) { if (detail) *detail = message; return false; };
            CubismModel::RuntimeValueSnapshot values;
            if (!candidate->CaptureRuntimeValues(values, detail)) return false;
            if (restoreActive) {
                auto currentSeeds = addedSeeds;
                for (const auto& id : restoredContext->current.parameterIds) currentSeeds.remove(id);
                CubismModel::RuntimeValueSnapshot restoredValues;
                if (!candidate->MigrateRuntimeValues(restoredContext->current, restoredValues, currentSeeds, detail)) return false;
                values = std::move(restoredValues);
            }
            for (int i = 0; i < values.parameterIds.size(); ++i) nextIndices.insert(values.parameterIds[i], i);
            const auto bound = [&](int index, float value) {
                if (value < values.minimums[index]) return values.minimums[index];
                if (value > values.maximums[index]) return values.maximums[index];
                return value;
            };
            if (editing) {
                ParameterStructureSeeds entrySeeds = addedSeeds;
                for (const auto& id : editorEntry->parameterIds) entrySeeds.remove(id);
                for (auto it = entrySeeds.begin(); it != entrySeeds.end(); ++it)
                    if (it->mode == ParameterStructureSeed::Mode::Values && it->editorEntry) it->runtime = *it->editorEntry;
                nextEntry = std::make_unique<CubismModel::RuntimeValueSnapshot>();
                if (!candidate->MigrateRuntimeValues(*editorEntry, *nextEntry, entrySeeds, detail)) return false;
            }
            const auto migrateMap = [&](const QMap<QString, float>& source, QMap<QString, float>& target,
                                        PreviewContext context) {
                const bool archived = restoreActive && context != PreviewContext::TimelineBase;
                const auto& originIndices = archived ? editorIndices : sourceIndices;
                const auto& origin = archived ? *editorEntry : sourceState;
                for (auto it = source.cbegin(); it != source.cend(); ++it)
                    if (!originIndices.contains(it.key()) || !std::isfinite(it.value())) return false;
                for (int i = 0; i < values.parameterIds.size(); ++i) {
                    const QString& id = values.parameterIds[i];
                    float value = context == PreviewContext::EditorBase ? nextEntry->parameterValues[i] :
                        context == PreviewContext::TimelineBase && values.HasSavedParameter(i) ? values.savedParameters[i] : values.parameterValues[i];
                    bool rangeChanged = false;
                    const auto old = originIndices.constFind(id);
                    if (old != originIndices.cend()) {
                        if (!source.contains(id)) return false;
                        value = source[id];
                        rangeChanged = origin.minimums[*old] != values.minimums[i] || origin.maximums[*old] != values.maximums[i];
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
            if ((editing && (!migrateMap(editorPose, nextEditorPose, PreviewContext::EditorPose) ||
                             !migrateMap(editorBase, nextEditorBase, PreviewContext::EditorBase))) ||
                (_timelinePreviewActive && !migrateMap(_timelineBaseParameters, nextTimelineBase, PreviewContext::TimelineBase)))
                return reject(QStringLiteral("参数编辑或时间轴基线无法迁移"));
            if (requestedPose) {
                if (requestedPose->size() != values.parameterIds.size())
                    return reject(QStringLiteral("指定编辑姿态的参数集合与候选模型不一致"));
                for (int index = 0; index < values.parameterIds.size(); ++index) {
                    const auto value = requestedPose->constFind(values.parameterIds[index]);
                    if (value == requestedPose->cend() || !std::isfinite(*value) ||
                        *value < values.minimums[index] || *value > values.maximums[index])
                        return reject(QStringLiteral("指定编辑姿态缺少参数或超出候选范围：%1").arg(values.parameterIds[index]));
                }
                nextEditorPose = *requestedPose;
            }
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
            QSet<QString> eyeIds, lipIds, originalGroupIds;
            const auto& referenceState = referenceCandidate ? *referenceCandidate : m_parameterReferences;
            const auto collectGroup = [&](const auto& ids, const QSet<QString>& suppressed, QSet<QString>& effective) {
                for (int index = 0; index < ids.GetSize(); ++index) {
                    const auto id = QString::fromUtf8(ids[index]->GetString().GetRawString());
                    originalGroupIds.insert(id);
                    if (nextIndices.contains(id) && !suppressed.contains(id)) effective.insert(id);
                }
            };
            collectGroup(_eyeBlinkIds, referenceState.eyeBlink, eyeIds);
            collectGroup(_lipSyncIds, referenceState.lipSync, lipIds);
            QSet<QString> removedDerived;
            for (auto it = nextTimeline.frame.parameters.begin(); it != nextTimeline.frame.parameters.end();) {
                const bool derivedRemoved = !nextTimeline.explicitParameterIds.contains(it.key()) &&
                    originalGroupIds.contains(it.key()) && !eyeIds.contains(it.key()) && !lipIds.contains(it.key());
                if (!nextIndices.contains(it.key()) || derivedRemoved) {
                    if (derivedRemoved) removedDerived.insert(it.key());
                    it = nextTimeline.frame.parameters.erase(it);
                } else ++it;
            }
            for (auto it = nextTimeline.explicitParameterIds.begin(); it != nextTimeline.explicitParameterIds.end();) {
                if (!nextTimeline.frame.parameters.contains(*it)) it = nextTimeline.explicitParameterIds.erase(it);
                else ++it;
            }
            if (prepareTimeline && !_timelinePreviewActive) {
                const auto& baseline = editing ? *nextEntry : values;
                for (int index = 0; index < values.parameterIds.size(); ++index) {
                    const auto& id = values.parameterIds[index];
                    float value = baseline.HasSavedParameter(index)
                        ? baseline.savedParameters[index] : baseline.parameterValues[index];
                    const auto seed = addedSeeds.constFind(id);
                    if (!sourceIndices.contains(id) && !baseValues.contains(id) && seed != addedSeeds.cend() &&
                        seed->mode == ParameterStructureSeed::Mode::Values && seed->timelineBase) {
                        value = *seed->timelineBase;
                        if (seed->runtime.minimum != values.minimums[index] || seed->runtime.maximum != values.maximums[index])
                            value = bound(index, value);
                    }
                    nextTimelineBase.insert(id, value);
                }
                for (int index = 0; index < values.partIds.size(); ++index)
                    nextTimelineParts.insert(values.partIds[index], baseline.partOpacities[index]);
                nextTimelineOpacity = baseline.modelOpacity;
            }
            if (prepareTimeline && !prepareTimeline(nextTimelineBase, eyeIds, lipIds, nextTimeline, detail)) return false;
            if (!nextTimeline.frame.IsFinite()) return reject(QStringLiteral("恢复的时间轴帧含非有限值或无效 ID"));
            for (const auto& id : nextTimeline.explicitParameterIds)
                if (!nextTimeline.frame.parameters.contains(id)) return reject(QStringLiteral("时间轴显式轨道与帧参数不匹配"));
            for (auto it = nextTimeline.frame.parameters.cbegin(); it != nextTimeline.frame.parameters.cend(); ++it)
                if (!nextIndices.contains(it.key())) return reject(QStringLiteral("时间轴引用不存在的参数：%1").arg(it.key()));
            for (auto it = nextTimeline.frame.parts.cbegin(); it != nextTimeline.frame.parts.cend(); ++it)
                if (!values.partIds.contains(it.key())) return reject(QStringLiteral("时间轴引用不存在的 Part：%1").arg(it.key()));
            nextTimelineParameters = nextTimeline.active ? nextTimeline.frame.ResolveParameters(eyeIds, lipIds) : QMap<QString, float>{};
            for (auto it = nextTimelineParameters.begin(); it != nextTimelineParameters.end();) {
                if (!nextIndices.contains(it.key())) it = nextTimelineParameters.erase(it);
                else ++it;
            }
            for (auto it = _timelineParameters.cbegin(); it != _timelineParameters.cend(); ++it)
                if (!m_timelineExplicitParameterIds.contains(it.key()) && originalGroupIds.contains(it.key()) &&
                    !eyeIds.contains(it.key()) && !lipIds.contains(it.key())) removedDerived.insert(it.key());
            if (!(editing && keepEditorPreview)) {
                if (prepareTimeline && (_timelinePreviewActive || nextTimeline.active)) {
                    for (int index = 0; index < values.parameterIds.size(); ++index) {
                        const auto id = values.parameterIds[index];
                        if (!nextTimelineBase.contains(id)) return reject(QStringLiteral("时间轴参数基线不完整"));
                        values.parameterValues[index] = nextTimelineBase[id];
                    }
                    for (int index = 0; index < values.partIds.size(); ++index) {
                        const auto id = values.partIds[index];
                        if ((!nextTimeline.active || (_timelinePreview.parts.contains(id) && !nextTimeline.frame.parts.contains(id))) && nextTimelineParts.contains(id))
                            values.partOpacities[index] = nextTimelineParts[id];
                    }
                    if (!nextTimeline.active || _timelinePreview.model.contains(QStringLiteral("Opacity"))) values.modelOpacity = nextTimelineOpacity;
                    if (nextTimeline.active) {
                        for (auto it = nextTimelineParameters.cbegin(); it != nextTimelineParameters.cend(); ++it)
                            values.parameterValues[nextIndices[it.key()]] = bound(nextIndices[it.key()], it.value());
                        for (auto it = nextTimeline.frame.parts.cbegin(); it != nextTimeline.frame.parts.cend(); ++it)
                            values.partOpacities[values.partIds.indexOf(it.key())] = std::clamp(it.value(), 0.0f, 1.0f);
                        if (nextTimeline.frame.model.contains(QStringLiteral("Opacity")))
                            values.modelOpacity = std::clamp(nextTimeline.frame.model[QStringLiteral("Opacity")], 0.0f, 1.0f);
                    }
                } else for (const auto& id : removedDerived)
                    if (nextIndices.contains(id) && nextTimelineBase.contains(id)) values.parameterValues[nextIndices[id]] = nextTimelineBase[id];
            }
            if (!nextTimeline.active) {
                nextTimeline.frame = {};
                nextTimeline.explicitParameterIds.clear();
                nextTimelineBase.clear();
                nextTimelineParts.clear();
            }
            if (!candidate->RestoreRuntimeValues(values, detail)) return false;
            if (editing && keepEditorPreview && !candidate->ApplyParameterPose(nextEditorPose, detail)) return false;
            return !preparePreview || preparePreview(*candidate, detail);
        };
        auto policy = CubismModelUpdatePolicy::AllowClippingEdits | CubismModelUpdatePolicy::AllowParameterDefinitionEdits |
            CubismModelUpdatePolicy::AllowParameterStructureEdits;
        if (geometry) policy = policy | CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        if (materialDrawableIds) policy = policy | CubismModelUpdatePolicy::AllowDrawableMaterialEdits;
        if (!ApplyPreviewTransaction(ownedBytes, sourceDigest, prepare, error, policy, addedSeeds,
                                     referenceCandidate ? &referenceCandidate->physics : nullptr, physicsDraft, geometry, textureState, materialDrawableIds, atlasState)) return false;
        if (commitReferences) commitReferences();
        m_mapParaToIndex.swap(nextIndices);
        _timelinePreview.parameters.swap(nextTimeline.frame.parameters);
        _timelinePreview.parts.swap(nextTimeline.frame.parts);
        _timelinePreview.model.swap(nextTimeline.frame.model);
        _timelineParameters.swap(nextTimelineParameters);
        m_timelineExplicitParameterIds.swap(nextTimeline.explicitParameterIds);
        _timelinePreviewActive = nextTimeline.active;
        _timelineBaseParameters.swap(nextTimelineBase);
        _timelineBaseParts.swap(nextTimelineParts);
        _timelineBaseOpacity = nextTimelineOpacity;
        _opacity = _model->GetModelOpacity();
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
