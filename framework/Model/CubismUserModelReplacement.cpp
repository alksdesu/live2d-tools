/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "CubismUserModel.hpp"
#include <QScopedValueRollback>

namespace Live2D { namespace Cubism { namespace Framework {
bool CubismUserModel::ReplaceModel(const csmByte* bytes, csmSizeInt size,
                                 const CubismModelReplacement::RendererPreparation& prepareRenderer,
                                 QString* error, const CubismModelReplacement::ModelPreparation& prepareModel)
{
    return ReplaceModel(bytes, size, prepareRenderer, CubismModelUpdatePolicy::Strict, error, prepareModel);
}

bool CubismUserModel::ReplaceModel(const csmByte* bytes, csmSizeInt size,
                                 const CubismModelReplacement::RendererPreparation& prepareRenderer,
                                 CubismModelUpdatePolicy policy, QString* error,
                                 const CubismModelReplacement::ModelPreparation& prepareModel)
{
    return ReplaceModel(bytes, size, prepareRenderer, policy, {}, error, prepareModel);
}

bool CubismUserModel::ReplaceModel(const csmByte* bytes, csmSizeInt size,
                                 const CubismModelReplacement::RendererPreparation& prepareRenderer,
                                 CubismModelUpdatePolicy policy, const ParameterStructureSeeds& seeds, QString* error,
                                 const CubismModelReplacement::ModelPreparation& prepareModel)
{
    if (error) error->clear();
    if (_updating || !_moc || !_model || !_renderer || !_modelMatrix ||
        (_pose && !_pose->CanRebindModel(_model))) {
        if (error) *error = QStringLiteral("当前模型状态不允许替换预览");
        return false;
    }
    const QScopedValueRollback<csmBool> updating(_updating, true);
    auto* sourcePhysics = _physics;
    auto* sourcePose = _pose;
    const bool structure = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowParameterStructureEdits);
    CubismModelReplacement candidate;
    if (!candidate.Prepare(bytes, size, *_moc, *_model, *_renderer, prepareRenderer, policy, seeds, error, prepareModel)) return false;
    if (_physics != sourcePhysics || _pose != sourcePose) {
        if (error) *error = QStringLiteral("模型效果在准备期间发生变化");
        return false;
    }
    CubismModel* previous = _model;
    CubismModel* replacement = candidate.GetModel();
    CubismPhysics::ParameterDefinitionState physicsState;
    const bool migratePhysics = _physics && (structure || AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowParameterDefinitionEdits));
    if (migratePhysics && !(structure ? _physics->PrepareParameterStructureChange(*previous, *replacement, physicsState, error)
                                     : _physics->PrepareParameterDefinitionChange(*previous, *replacement, physicsState, error))) return false;
    CubismPose::RebindState poseState;
    if (_pose && !(structure ? _pose->PrepareRebindModel(previous, replacement, poseState, error)
                            : _pose->RebindModel(previous, replacement))) {
        if (error) *error = QStringLiteral("Pose 无法保持当前状态");
        return false;
    }
    if (!candidate.Commit(_moc, _model, _renderer)) {
        if (_pose && !structure) _pose->RebindModel(replacement, previous);
        if (error) *error = QStringLiteral("预览源模型在准备期间发生变化");
        return false;
    }
    if (migratePhysics) _physics->CommitParameterDefinitionChange(physicsState);
    if (_pose && structure) _pose->CommitRebindModel(poseState);
    return true;
}

}}}
