#include "LAppModel.hpp"
#include <cmath>

bool LAppModel::CaptureOnionSkinBase(live2dstruct::COnionSkin::Base& result, QString* error) const
{
    if (error) error->clear();
    if (!CanSampleOnionSkin()) {
        if (error) *error = QStringLiteral("当前模型编辑期间暂停洋葱皮");
        return false;
    }
    Csm::CubismModel::RuntimeValueSnapshot values;
    if (!_model->CaptureRuntimeValues(values, error)) return false;
    live2dstruct::COnionSkin::Base candidate;
    for (int index = 0; index < values.parameterIds.size(); ++index) {
        const auto& id = values.parameterIds[index];
        const float value = _timelinePreviewActive && _timelineBaseParameters.contains(id) ? _timelineBaseParameters[id]
            : values.HasSavedParameter(index) ? values.savedParameters[index] : values.parameterValues[index];
        if (!std::isfinite(value)) { if (error) *error = QStringLiteral("洋葱皮基础参数无效"); return false; }
        candidate.parameters.insert(id, value);
    }
    for (int index = 0; index < values.partIds.size(); ++index) {
        const auto& id = values.partIds[index];
        candidate.parts.insert(id, _timelinePreviewActive ? _timelineBaseParts.value(id, values.partOpacities[index]) : values.partOpacities[index]);
    }
    candidate.modelOpacity = _timelinePreviewActive ? _timelineBaseOpacity : values.modelOpacity;
    candidate.hiddenParts = _hiddenPartIds;
    GetTimelineEffectIds(candidate.eyeBlinkIds, candidate.lipSyncIds);
    result = std::move(candidate);
    return true;
}
