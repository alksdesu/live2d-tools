#pragma once

#include "framework/CubismFramework.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "opengl/ccubismshaderprograms.h"

namespace Live2D { namespace Cubism { namespace Framework {
class CubismModel;
namespace Rendering {
class CubismRenderer_OpenGLES2;

class CubismShader_OpenGLES2
{
public:
    enum class DrawMode { Preview, IsolatedPsdLayer };
    static CubismShader_OpenGLES2* GetInstance();
    static bool DeleteInstance();
    bool Initialize(const QOpenGLContext* context);
    bool SetupShaderProgramForDraw(CubismRenderer_OpenGLES2* renderer, const CubismModel& model,
                                   csmInt32 index, bool isAll, csmFloat32 opacityOverride = -1.0f,
                                   DrawMode mode = DrawMode::Preview);
    bool SetupShaderProgramForMask(CubismRenderer_OpenGLES2* renderer, const CubismModel& model, csmInt32 index);
    bool SetupShaderProgramForOffscreenMask(CubismRenderer_OpenGLES2* renderer, const CubismModel& model,
                                            csmInt32 index);
    bool SetupShaderProgramForComposite(CubismRenderer_OpenGLES2* renderer, GLuint texture,
                                        CubismRenderer::CubismBlendMode blend, csmFloat32 opacity,
                                        const CubismRenderer::CubismTextureColor& multiply,
                                        const CubismRenderer::CubismTextureColor& screen,
                                        GLuint maskTexture = 0, csmBool maskInvert = false,
                                        const CubismRenderer::CubismTextureColor& maskRect = CubismRenderer::CubismTextureColor(0, 0, 1, 1),
                                        const CubismRenderer::CubismTextureColor& maskChannel = CubismRenderer::CubismTextureColor(1, 0, 0, 0));
    bool SetupShaderProgramForBackdropComposite(CubismRenderer_OpenGLES2* renderer, GLuint sourceTexture,
                                                GLuint backdropTexture, csmInt32 colorMode, csmInt32 alphaMode,
                                                csmFloat32 opacity,
                                                const CubismRenderer::CubismTextureColor& multiply,
                                                const CubismRenderer::CubismTextureColor& screen,
                                                GLuint maskTexture = 0, csmBool maskInvert = false,
                                                const CubismRenderer::CubismTextureColor& maskRect = CubismRenderer::CubismTextureColor(0, 0, 1, 1),
                                                const CubismRenderer::CubismTextureColor& maskChannel = CubismRenderer::CubismTextureColor(1, 0, 0, 0));
#ifdef CSM_TARGET_ANDROID_ES2
    static void SetExtShaderMode(csmBool extMode, csmBool extPAMode);
#endif

private:
    CubismShader_OpenGLES2() = default;
    ~CubismShader_OpenGLES2() = default;
    CCubismShaderPrograms m_programs;
};
}}}}
