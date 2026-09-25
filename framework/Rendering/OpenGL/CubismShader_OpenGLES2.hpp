#pragma once

#include "framework/CubismFramework.hpp"
#include "opengl/ccubismshaderprograms.h"

namespace Live2D { namespace Cubism { namespace Framework {
class CubismModel;
namespace Rendering {
class CubismRenderer_OpenGLES2;

class CubismShader_OpenGLES2
{
public:
    static CubismShader_OpenGLES2* GetInstance();
    static bool DeleteInstance();
    bool Initialize(const QOpenGLContext* context);
    bool SetupShaderProgramForDraw(CubismRenderer_OpenGLES2* renderer, const CubismModel& model,
                                   csmInt32 index, bool isAll, csmFloat32 opacityOverride = -1.0f);
    bool SetupShaderProgramForMask(CubismRenderer_OpenGLES2* renderer, const CubismModel& model, csmInt32 index);
#ifdef CSM_TARGET_ANDROID_ES2
    static void SetExtShaderMode(csmBool extMode, csmBool extPAMode);
#endif

private:
    CubismShader_OpenGLES2() = default;
    ~CubismShader_OpenGLES2() = default;
    CCubismShaderPrograms m_programs;
};
}}}}
