#include "creadopengl.h"
#include "cmeshbounds.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <cmath>
#include <vector>

bool CReadOpengl::setGlOwningContext(const QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context) return false;
    auto* candidate = const_cast<QOpenGLContext*>(context);
    auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(candidate);
    if (!functions || !functions->initializeOpenGLFunctions()) return false;
    m_context = candidate;
    m_gl = functions;
    return true;
}

bool CReadOpengl::CanUseContext() const
{
    return m_context && m_context->isValid() && m_gl && QOpenGLContext::currentContext() == m_context;
}

bool CReadOpengl::ClearTarget(bool alphaOnly)
{
    if (!CanUseContext() || m_gl->glGetError() != GL_NO_ERROR) return false;
    GLint framebuffer = 0, count = 0;
    m_gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (!framebuffer) return false;
    m_gl->glGetIntegerv(GL_MAX_DRAW_BUFFERS, &count);
    if (count <= 0) return false;
    std::vector<GLenum> buffers(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        GLint buffer;
        m_gl->glGetIntegerv(GL_DRAW_BUFFER0 + index, &buffer);
        buffers[static_cast<size_t>(index)] = static_cast<GLenum>(buffer);
    }
    GLboolean colorMask[4];
    m_gl->glGetBooleani_v(GL_COLOR_WRITEMASK, 0, colorMask);
    const bool scissor = m_gl->glIsEnabled(GL_SCISSOR_TEST);
    const auto restore = qScopeGuard([&] {
        m_gl->glDrawBuffers(count, buffers.data());
        m_gl->glColorMaski(0, colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        if (scissor) m_gl->glEnable(GL_SCISSOR_TEST);
    });
    m_gl->glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (m_gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    m_gl->glDisable(GL_SCISSOR_TEST);
    m_gl->glColorMaski(0, !alphaOnly, !alphaOnly, !alphaOnly, GL_TRUE);
    const GLfloat color[]{1.0f, 1.0f, 1.0f, 0.0f};
    m_gl->glClearBufferfv(GL_COLOR, 0, color);
    return m_gl->glGetError() == GL_NO_ERROR;
}

bool CReadOpengl::PreDraw()
{
    return ClearTarget(false);
}

bool CReadOpengl::UpdateFrame()
{
    return ClearTarget(true);
}

bool CReadOpengl::ReadImage(const QRect& rect, const QSize& canvasSize, QImage& image)
{
    if (!CanUseContext() || m_gl->glGetError() != GL_NO_ERROR || rect.isEmpty() ||
        !QRect(QPoint(), canvasSize).contains(rect)) return false;
    GLint drawFramebuffer = 0, readFramebuffer = 0;
    m_gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    m_gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    if (!drawFramebuffer) return false;
    m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFramebuffer);
    GLint readBuffer = 0;
    m_gl->glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    const auto restoreFramebuffer = qScopeGuard([&] {
        m_gl->glReadBuffer(readBuffer);
        m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    });
    m_gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (m_gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    GLint objectType = 0, object = 0, componentType = 0;
    m_gl->glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
    m_gl->glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    if (objectType != GL_TEXTURE || !object) return false;
    m_gl->glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
    if (componentType == GL_INT || componentType == GL_UNSIGNED_INT) return false;
    GLint textureBinding = 0, level = 0, textureWidth = 0, textureHeight = 0;
    m_gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    const auto restoreTexture = qScopeGuard([&] { m_gl->glBindTexture(GL_TEXTURE_2D, textureBinding); });
    m_gl->glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
    m_gl->glBindTexture(GL_TEXTURE_2D, object);
    if (m_gl->glGetError() != GL_NO_ERROR) return false;
    m_gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &textureWidth);
    m_gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &textureHeight);
    if (QSize(textureWidth, textureHeight) != canvasSize) return false;

    QImage candidate(rect.size(), QImage::Format_RGBA8888);
    if (candidate.isNull()) return false;
    GLint packBuffer = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    m_gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    m_gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    m_gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    m_gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    m_gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    const auto restorePack = qScopeGuard([&] {
        m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
        m_gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        m_gl->glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        m_gl->glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        m_gl->glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    });
    m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    m_gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    m_gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    m_gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    m_gl->glReadPixels(rect.x(), rect.y(), rect.width(), rect.height(), GL_RGBA, GL_UNSIGNED_BYTE, candidate.bits());
    if (m_gl->glGetError() != GL_NO_ERROR) return false;
    candidate = candidate.mirrored(false, true);
    if (candidate.isNull()) return false;
    image = std::move(candidate);
    return true;
}

bool CReadOpengl::GetScreenNode(const float* vertices, int vertexCount, const float* mvp,
                               int width, int height, QImage& image, QRect& psdRect)
{
    if (!CanUseContext() || !mvp || width <= 0 || height <= 0 || vertexCount < 0 || vertexCount > 1000000 ||
        (vertexCount > 0 && !vertices)) return false;
    for (int index = 0; index < 16; ++index) if (!std::isfinite(mvp[index])) return false;
    bool valid = true;
    const QRect rect = MeshBounds::Viewport(vertices, vertexCount, [&](float x, float y) {
        const double px = double(mvp[0]) * x + double(mvp[4]) * y + mvp[12];
        const double py = double(mvp[1]) * x + double(mvp[5]) * y + mvp[13];
        const double pw = double(mvp[3]) * x + double(mvp[7]) * y + mvp[15];
        const double nx = px / pw, ny = py / pw;
        if (!std::isfinite(nx) || !std::isfinite(ny)) valid = false;
        return QPointF(nx, ny);
    }, QSize(width, height));
    if (!valid) return false;
    QImage candidate;
    if (!rect.isEmpty() && !ReadImage(rect, QSize(width, height), candidate)) return false;
    image = std::move(candidate);
    psdRect = rect.isEmpty() ? QRect() : QRect(rect.x(), height - rect.y() - rect.height(), rect.width(), rect.height());
    return true;
}
