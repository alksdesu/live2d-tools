/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismShader_OpenGLES2.hpp"
#include "CubismRenderer_OpenGLES2.hpp"
#include "framework/Type/csmRectF.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "live2d/LAppDelegate.hpp"
#include <QOpenGLVersionFunctionsFactory>

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {
namespace {
CubismShader_OpenGLES2* s_instance = nullptr;
using Variant = CCubismShaderPrograms::Variant;
using BlendMode = CCubismShaderPrograms::BlendMode;

QOpenGLFunctions_3_3_Core* functions(const QOpenGLContext* context)
{
    if (!context || QOpenGLContext::currentContext() != context) return nullptr;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(const_cast<QOpenGLContext*>(context));
    return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
}

CubismRenderer::CubismTextureColor* channel(CubismClippingContext_OpenGLES2* clipping)
{
    if (!clipping || !clipping->GetClippingManager() || clipping->_layoutChannelIndex < 0 ||
        clipping->_layoutChannelIndex >= 4) return nullptr;
    return clipping->GetClippingManager()->GetChannelFlagAsColor(clipping->_layoutChannelIndex);
}

void colorUniform(QOpenGLFunctions_3_3_Core& gl, GLint location, const CubismRenderer::CubismTextureColor& color)
{
    gl.glUniform4f(location, color.R, color.G, color.B, color.A);
}
}

CubismShader_OpenGLES2* CubismShader_OpenGLES2::GetInstance()
{
    if (!s_instance) s_instance = CSM_NEW CubismShader_OpenGLES2();
    return s_instance;
}

bool CubismShader_OpenGLES2::DeleteInstance()
{
    if (!s_instance) return true;
    if (!s_instance->m_programs.Release()) return false;
    CSM_DELETE_SELF(CubismShader_OpenGLES2, s_instance);
    s_instance = nullptr;
    return true;
}

bool CubismShader_OpenGLES2::Initialize(const QOpenGLContext* context)
{
    if (m_programs.Initialize(context)) return true;
    CubismLogError("Shader initialization failed: %s", m_programs.Error().toUtf8().constData());
    return false;
}

#ifdef CSM_TARGET_ANDROID_ES2
void CubismShader_OpenGLES2::SetExtShaderMode(csmBool extMode, csmBool extPAMode)
{
    if (extMode || extPAMode) CubismLogWarning("The Qt desktop renderer requires OpenGL 3.3 Core shaders.");
}
#endif

bool CubismShader_OpenGLES2::SetupShaderProgramForDraw(CubismRenderer_OpenGLES2* renderer,
    const CubismModel& model, csmInt32 index, bool isAll, csmFloat32 opacityOverride)
{
    if (!renderer || index < 0 || index >= model.GetDrawableCount()) return false;
    const auto* context = LAppDelegate::GetInstance()->getGlOwningContext();
    if (!Initialize(context)) return false;
    auto* gl = functions(context);
    if (!gl) return false;
    auto* clipping = renderer->GetClippingContextBufferForDraw();
    const bool masked = clipping != nullptr;
    const bool inverted = masked && model.GetDrawableInvertedMask(index);
    const bool premultiplied = renderer->IsPremultipliedAlpha();
    const Variant variant = premultiplied
        ? (masked ? (inverted ? Variant::PremultipliedInverted : Variant::PremultipliedMasked) : Variant::Premultiplied)
        : (masked ? (inverted ? Variant::StraightInverted : Variant::StraightMasked) : Variant::Straight);
    BlendMode blend = BlendMode::Normal;
    if (model.GetDrawableBlendMode(index) == CubismRenderer::CubismBlendMode_Additive) blend = BlendMode::Additive;
    else if (model.GetDrawableBlendMode(index) == CubismRenderer::CubismBlendMode_Multiplicative) blend = BlendMode::Multiplicative;
    const auto* program = m_programs.Get(variant);
    const auto* flag = masked ? channel(clipping) : nullptr;
    auto* mask = masked ? renderer->GetMaskBuffer(clipping->_bufferIndex) : nullptr;
    if (!program || (masked && (!flag || !mask || !mask->IsValid())) || !m_programs.Bind(variant, blend)) return false;

    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, renderer->GetBindedTextureId(model.GetDrawableTextureIndex(index)));
    gl->glUniform1i(program->sampler0, 0);
    if (masked) {
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, mask->GetColorBuffer());
        gl->glUniform1i(program->sampler1, 1);
        gl->glUniformMatrix4fv(program->clipMatrix, 1, GL_FALSE, clipping->_matrixForDraw.GetArray());
        colorUniform(*gl, program->channelFlag, *flag);
    }
    gl->glUniformMatrix4fv(program->matrix, 1, GL_FALSE, renderer->GetMvpMatrix().GetArray());
    const csmFloat32 opacity = opacityOverride >= 0.0f ? opacityOverride : model.GetDrawableOpacityAll(index, isAll);
    colorUniform(*gl, program->baseColor, renderer->GetModelColorWithOpacity(opacity));
    colorUniform(*gl, program->multiplyColor, model.GetMultiplyColor(index));
    colorUniform(*gl, program->screenColor, model.GetScreenColor(index));
    return gl->glGetError() == GL_NO_ERROR;
}

bool CubismShader_OpenGLES2::SetupShaderProgramForMask(CubismRenderer_OpenGLES2* renderer,
    const CubismModel& model, csmInt32 index)
{
    if (!renderer || index < 0 || index >= model.GetDrawableCount()) return false;
    const auto* context = LAppDelegate::GetInstance()->getGlOwningContext();
    if (!Initialize(context)) return false;
    auto* gl = functions(context);
    auto* clipping = renderer->GetClippingContextBufferForMask();
    const auto* flag = channel(clipping);
    if (!gl || !clipping || !clipping->_layoutBounds || !flag) return false;
    const auto* program = m_programs.Get(Variant::SetupMask);
    if (!program || !m_programs.Bind(Variant::SetupMask)) return false;
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, renderer->GetBindedTextureId(model.GetDrawableTextureIndex(index)));
    gl->glUniform1i(program->sampler0, 0);
    gl->glUniformMatrix4fv(program->clipMatrix, 1, GL_FALSE, clipping->_matrixForMask.GetArray());
    colorUniform(*gl, program->channelFlag, *flag);
    const csmRectF& rect = *clipping->_layoutBounds;
    const CubismRenderer::CubismTextureColor bounds{
        rect.X * 2.0f - 1.0f, rect.Y * 2.0f - 1.0f, rect.GetRight() * 2.0f - 1.0f, rect.GetBottom() * 2.0f - 1.0f};
    colorUniform(*gl, program->baseColor, bounds);
    return gl->glGetError() == GL_NO_ERROR;
}
}}}}
