#include "live2d/LAppLive2DManager.hpp"
#include "framework/Model/CubismUserModel.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Id/CubismId.hpp"
#include <QtGlobal>

namespace {
[[noreturn]] void unexpectedRuntimeAccess()
{
    qFatal("Parameter panel tests must not access Cubism runtime");
}
}

LAppLive2DManager* LAppLive2DManager::GetInstance() { unexpectedRuntimeAccess(); }
LAppModel* LAppLive2DManager::GetModel(Csm::csmUint32) const { unexpectedRuntimeAccess(); }
Csm::csmUint32 LAppLive2DManager::GetModelNum() const { unexpectedRuntimeAccess(); }

namespace Live2D { namespace Cubism { namespace Framework {
CubismModel* CubismUserModel::GetModel() const { unexpectedRuntimeAccess(); }
CubismIdHandle CubismModel::GetParameterId(csmUint32) { unexpectedRuntimeAccess(); }
csmFloat32 CubismModel::GetParameterValue(csmInt32) { unexpectedRuntimeAccess(); }
csmInt32 CubismModel::GetParameterCount() const { unexpectedRuntimeAccess(); }
Core::csmParameterType CubismModel::GetParameterType(csmUint32) const { unexpectedRuntimeAccess(); }
csmFloat32 CubismModel::GetParameterDefaultValue(csmUint32) const { unexpectedRuntimeAccess(); }
csmFloat32 CubismModel::GetParameterMaximumValue(csmUint32) const { unexpectedRuntimeAccess(); }
csmFloat32 CubismModel::GetParameterMinimumValue(csmUint32) const { unexpectedRuntimeAccess(); }
const csmString& CubismId::GetString() const { unexpectedRuntimeAccess(); }
const csmChar* csmString::GetRawString() const { unexpectedRuntimeAccess(); }
}}}
