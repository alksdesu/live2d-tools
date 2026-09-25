#include "coffscreenbuffer.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <cmath>
#include <limits>

namespace {
struct Bindings
{
    explicit Bindings(QOpenGLFunctions_3_3_Core& gl) : gl(gl)
    {
        gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        gl.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    }

    ~Bindings()
    {
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        gl.glBindTexture(GL_TEXTURE_2D, texture);
        gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    }

    QOpenGLFunctions_3_3_Core& gl;
    GLint read = 0, draw = 0, texture = 0, unpack = 0;
};
}

COffscreenBuffer::~COffscreenBuffer()
{
    DestroyOffscreenSurface();
}

QOpenGLFunctions_3_3_Core* COffscreenBuffer::functions() const
{
    if (!m_context || !m_context->isValid() || QOpenGLContext::currentContext() != m_context)
        return nullptr;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_context);
    return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
}

bool COffscreenBuffer::setGlOwningContext(const QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context)
        return false;
    if (m_context && m_context != context && (m_framebuffer || m_texture || m_drawing))
        return false;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(const_cast<QOpenGLContext*>(context));
    if (!gl || !gl->initializeOpenGLFunctions()) return false;
    if (!m_context) reset();
    m_context = const_cast<QOpenGLContext*>(context);
    return true;
}

bool COffscreenBuffer::CreateOffscreenSurface(int width, int height, GLuint borrowed)
{
    auto* gl = functions();
    if (!gl || m_drawing || width <= 0 || height <= 0
        || width > std::numeric_limits<int>::max() / 4 / height
        || (borrowed && borrowed == m_texture && m_ownsTexture))
        return false;
    GLint maximum = 0;
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum <= 0 || width > maximum || height > maximum) return false;
    if (borrowed && !gl->glIsTexture(borrowed)) return false;

    GLuint texture = borrowed, framebuffer = 0;
    bool committed = false;
    Bindings bindings(*gl);
    const auto discard = qScopeGuard([&] {
        if (committed) return;
        if (framebuffer) gl->glDeleteFramebuffers(1, &framebuffer);
        if (texture && !borrowed) gl->glDeleteTextures(1, &texture);
    });
    if (!borrowed) gl->glGenTextures(1, &texture);
    if (!texture) return false;
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    GLint textureBinding = 0;
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    if (static_cast<GLuint>(textureBinding) != texture) return false;
    if (!borrowed) {
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    GLint actualWidth = 0, actualHeight = 0;
    gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
    gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
    if (actualWidth != width || actualHeight != height) return false;

    gl->glGenFramebuffers(1, &framebuffer);
    if (!framebuffer) return false;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    const GLuint previousFramebuffer = m_framebuffer, previousTexture = m_texture;
    const bool ownedPreviousTexture = m_ownsTexture;
    m_framebuffer = framebuffer;
    m_texture = texture;
    m_width = width;
    m_height = height;
    m_ownsTexture = !borrowed;
    committed = true;
    if (static_cast<GLuint>(bindings.read) == previousFramebuffer) bindings.read = 0;
    if (static_cast<GLuint>(bindings.draw) == previousFramebuffer) bindings.draw = 0;
    if (ownedPreviousTexture && static_cast<GLuint>(bindings.texture) == previousTexture) bindings.texture = 0;
    if (previousFramebuffer) gl->glDeleteFramebuffers(1, &previousFramebuffer);
    if (ownedPreviousTexture && previousTexture) gl->glDeleteTextures(1, &previousTexture);
    return true;
}

bool COffscreenBuffer::BeginDraw(GLint restoreFBO)
{
    auto* gl = functions();
    if (!gl || !IsValid() || m_drawing) return false;
    if (!gl->glIsFramebuffer(m_framebuffer) || !gl->glIsTexture(m_texture)) return false;
    if (restoreFBO > 0 && !gl->glIsFramebuffer(static_cast<GLuint>(restoreFBO))) return false;
    GLint previousRead = 0, previousDraw = 0;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    if (restoreFBO < 0) {
        m_restoreRead = previousRead;
        m_restoreDraw = previousDraw;
    } else {
        m_restoreRead = restoreFBO;
        m_restoreDraw = restoreFBO;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
        return false;
    }
    m_drawing = true;
    return true;
}

bool COffscreenBuffer::EndDraw()
{
    auto* gl = functions();
    if (!gl || !m_drawing) return false;
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_restoreRead);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_restoreDraw);
    m_drawing = false;
    return true;
}

bool COffscreenBuffer::Clear(float red, float green, float blue, float alpha)
{
    auto* gl = functions();
    if (!gl || !m_drawing || !std::isfinite(red) || !std::isfinite(green)
        || !std::isfinite(blue) || !std::isfinite(alpha))
        return false;
    GLint framebuffer = 0;
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (static_cast<GLuint>(framebuffer) != m_framebuffer) return false;
    const GLfloat color[] = {red, green, blue, alpha};
    gl->glClearBufferfv(GL_COLOR, 0, color);
    return true;
}

bool COffscreenBuffer::DestroyOffscreenSurface()
{
    if (!m_context) {
        reset();
        return true;
    }
    auto* gl = functions();
    if (!gl) return false;
    if (m_drawing && !EndDraw()) return false;
    if (m_framebuffer) gl->glDeleteFramebuffers(1, &m_framebuffer);
    if (m_ownsTexture && m_texture) gl->glDeleteTextures(1, &m_texture);
    reset();
    return true;
}

bool COffscreenBuffer::IsValid() const
{
    return m_context && m_context->isValid() && m_framebuffer && m_texture && m_width > 0 && m_height > 0;
}

void COffscreenBuffer::reset()
{
    m_framebuffer = 0;
    m_texture = 0;
    m_width = 0;
    m_height = 0;
    m_restoreRead = 0;
    m_restoreDraw = 0;
    m_ownsTexture = false;
    m_drawing = false;
}
