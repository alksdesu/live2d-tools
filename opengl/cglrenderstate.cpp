#include "cglrenderstate.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>

namespace {
constexpr std::array<GLenum, 4> capabilities{GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE};

QOpenGLFunctions_3_3_Core* functions(QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context) return nullptr;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
    return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
}
}

bool CGlRenderState::Save(const QOpenGLContext* context)
{
    if (m_saved) return false;
    auto* owner = const_cast<QOpenGLContext*>(context);
    auto* gl = functions(owner);
    if (!gl || gl->glGetError() != GL_NO_ERROR) return false;
    GLint drawBuffers = 0;
    gl->glGetIntegerv(GL_MAX_DRAW_BUFFERS, &drawBuffers);
    if (drawBuffers <= 0) return false;
    m_colorMasks.resize(static_cast<size_t>(drawBuffers));
    m_blendEnabled.resize(static_cast<size_t>(drawBuffers));
    gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vertexArray);
    gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
    gl->glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
    const auto restoreActiveTexture = qScopeGuard([&] { gl->glActiveTexture(m_activeTexture); });
    for (size_t index = 0; index < m_textures.size(); ++index) {
        gl->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_textures[index]);
    }
    for (size_t index = 0; index < capabilities.size(); ++index)
        m_enabled[index] = gl->glIsEnabled(capabilities[index]);
    for (GLint index = 0; index < drawBuffers; ++index) {
        m_blendEnabled[static_cast<size_t>(index)] = gl->glIsEnabledi(GL_BLEND, index);
        gl->glGetBooleani_v(GL_COLOR_WRITEMASK, index, m_colorMasks[static_cast<size_t>(index)].data());
    }
    gl->glGetIntegerv(GL_FRONT_FACE, &m_frontFace);
    gl->glGetIntegerv(GL_CULL_FACE_MODE, &m_cullFace);
    gl->glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendFactors[0]);
    gl->glGetIntegerv(GL_BLEND_DST_RGB, &m_blendFactors[1]);
    gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendFactors[2]);
    gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendFactors[3]);
    gl->glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEquations[0]);
    gl->glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEquations[1]);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_drawFramebuffer);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFramebuffer);
    gl->glGetIntegerv(GL_READ_BUFFER, &m_readBuffer);
    gl->glGetIntegerv(GL_VIEWPORT, m_viewport.data());
    gl->glGetFloatv(GL_COLOR_CLEAR_VALUE, m_clearColor.data());
    if (gl->glGetError() != GL_NO_ERROR) return false;
    m_context = owner;
    m_saved = true;
    return true;
}

bool CGlRenderState::Restore()
{
    auto* gl = functions(m_context);
    if (!m_saved || !gl) return false;
    gl->glUseProgram(m_program);
    gl->glBindVertexArray(m_vertexArray);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_arrayBuffer);
    for (size_t index = 0; index < m_textures.size(); ++index) {
        gl->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
        gl->glBindTexture(GL_TEXTURE_2D, m_textures[index]);
    }
    gl->glActiveTexture(m_activeTexture);
    for (size_t index = 0; index < capabilities.size(); ++index) {
        if (m_enabled[index]) gl->glEnable(capabilities[index]);
        else gl->glDisable(capabilities[index]);
    }
    for (size_t index = 0; index < m_colorMasks.size(); ++index) {
        const auto& mask = m_colorMasks[index];
        gl->glColorMaski(static_cast<GLuint>(index), mask[0], mask[1], mask[2], mask[3]);
        if (m_blendEnabled[index]) gl->glEnablei(GL_BLEND, static_cast<GLuint>(index));
        else gl->glDisablei(GL_BLEND, static_cast<GLuint>(index));
    }
    gl->glFrontFace(m_frontFace);
    gl->glCullFace(m_cullFace);
    gl->glBlendFuncSeparate(m_blendFactors[0], m_blendFactors[1], m_blendFactors[2], m_blendFactors[3]);
    gl->glBlendEquationSeparate(m_blendEquations[0], m_blendEquations[1]);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFramebuffer);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFramebuffer);
    gl->glReadBuffer(m_readBuffer);
    gl->glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    gl->glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    m_saved = false;
    return true;
}
