#ifndef CGLRENDERSTATE_H
#define CGLRENDERSTATE_H

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>
#include <array>
#include <vector>

class CGlRenderState
{
public:
    bool Save(const QOpenGLContext* context);
    bool Restore();
    const std::array<GLint, 4>& Viewport() const { return m_viewport; }

private:
    QPointer<QOpenGLContext> m_context;
    bool m_saved = false;
    GLint m_vertexArray = 0, m_arrayBuffer = 0, m_program = 0;
    GLint m_activeTexture = 0;
    std::array<GLint, 2> m_textures{};
    std::array<GLint, 2> m_samplers{};
    std::array<GLboolean, 4> m_enabled{};
    std::vector<GLboolean> m_blendEnabled;
    std::vector<std::array<GLboolean, 4>> m_colorMasks;
    GLint m_frontFace = 0, m_cullFace = 0;
    std::array<GLint, 4> m_blendFactors{};
    std::array<GLint, 2> m_blendEquations{};
    GLint m_readFramebuffer = 0, m_drawFramebuffer = 0, m_readBuffer = 0;
    std::array<GLint, 4> m_viewport{};
    std::array<GLfloat, 4> m_clearColor{};
};

#endif
