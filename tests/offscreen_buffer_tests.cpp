#include "opengl/coffscreenbuffer.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QScopeGuard>
#include <QSize>
#include <QSurfaceFormat>
#include <limits>
#include <memory>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<COffscreenBuffer>);
static_assert(!std::is_copy_assignable_v<COffscreenBuffer>);

namespace {
struct GlState
{
    GLint read = 0, draw = 0, active = 0, texture = 0, unpack = 0;
    GLint alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;

    bool operator==(const GlState& other) const
    {
        return read == other.read && draw == other.draw && active == other.active
            && texture == other.texture && unpack == other.unpack && alignment == other.alignment
            && rowLength == other.rowLength && skipPixels == other.skipPixels && skipRows == other.skipRows;
    }
};
}

class OffscreenBufferTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    QOpenGLFunctions_3_3_Core m_gl;

    GlState state()
    {
        GlState result;
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &result.read);
        m_gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &result.draw);
        m_gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &result.active);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &result.texture);
        m_gl.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &result.unpack);
        m_gl.glGetIntegerv(GL_UNPACK_ALIGNMENT, &result.alignment);
        m_gl.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &result.rowLength);
        m_gl.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &result.skipPixels);
        m_gl.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &result.skipRows);
        return result;
    }

    GLuint texture(int width, int height, bool depth = false)
    {
        GLuint id = 0;
        GLint binding = 0, unpack = 0;
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        m_gl.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        m_gl.glGenTextures(1, &id);
        m_gl.glBindTexture(GL_TEXTURE_2D, id);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, depth ? GL_DEPTH_COMPONENT24 : GL_RGBA8,
                         width, height, 0, depth ? GL_DEPTH_COMPONENT : GL_RGBA,
                         depth ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE, nullptr);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_gl.glBindTexture(GL_TEXTURE_2D, binding);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
        return id;
    }

    QByteArray pixels(GLuint texture, int width, int height)
    {
        GLint binding = 0, pack = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        m_gl.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        m_gl.glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        m_gl.glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        m_gl.glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        m_gl.glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        const auto restore = qScopeGuard([&] {
            m_gl.glBindTexture(GL_TEXTURE_2D, binding);
            m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
            m_gl.glPixelStorei(GL_PACK_ALIGNMENT, alignment);
            m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
            m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
            m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        });
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        QByteArray data(width * height * 4, '\0');
        m_gl.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        return data;
    }

private slots:
    void initTestCase()
    {
        QSurfaceFormat requested;
        requested.setRenderableType(QSurfaceFormat::OpenGL);
        requested.setVersion(3, 3);
        requested.setProfile(QSurfaceFormat::CoreProfile);
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(requested);
        if (!m_context->create() || !m_context->isValid()) QSKIP("Unable to create a real OpenGL 3.3 Core context.");
        const auto actual = m_context->format();
        if (actual.renderableType() != QSurfaceFormat::OpenGL || actual.profile() != QSurfaceFormat::CoreProfile
            || actual.majorVersion() < 3 || (actual.majorVersion() == 3 && actual.minorVersion() < 3))
            QSKIP("The returned context does not provide OpenGL 3.3 Core.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get()))
            QSKIP("Unable to make the offscreen surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable.");
        qInfo() << "Offscreen buffer GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        m_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, 0);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        m_gl.glDisable(GL_SCISSOR_TEST);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    void cleanup()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void ownedSurfaceClearsPixelsAndPreservesIndependentBindings()
    {
        COffscreenBuffer read, draw, target;
        QVERIFY(read.setGlOwningContext(m_context.get()));
        QVERIFY(draw.setGlOwningContext(m_context.get()));
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(read.CreateOffscreenSurface(1, 1));
        QVERIFY(draw.CreateOffscreenSurface(1, 1));
        GLuint sentinelTexture = texture(1, 1), sentinelUnpack = 0;
        m_gl.glGenBuffers(1, &sentinelUnpack);
        const auto release = qScopeGuard([&] {
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_gl.glDeleteBuffers(1, &sentinelUnpack);
            m_gl.glDeleteTextures(1, &sentinelTexture);
            m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            m_gl.glActiveTexture(GL_TEXTURE0);
        });
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, sentinelTexture);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sentinelUnpack);
        m_gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, 1, nullptr, GL_STATIC_DRAW);
        m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 13);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, read.GetRenderTexture());
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.GetRenderTexture());
        const GlState before = state();
        QVERIFY(target.CreateOffscreenSurface(3, 2));
        QVERIFY(target.IsValid());
        QCOMPARE(target.GetBufferWidth(), 3);
        QCOMPARE(target.GetBufferHeight(), 2);
        QVERIFY(state() == before);
        QVERIFY(!target.Clear(1, 0, 0, 1));
        QVERIFY(target.BeginDraw());
        QCOMPARE(state().read, GLint(target.GetRenderTexture()));
        QCOMPARE(state().draw, GLint(target.GetRenderTexture()));
        QVERIFY(!target.BeginDraw());
        QVERIFY(!target.CreateOffscreenSurface(7, 9));
        m_gl.glClearColor(0.125f, 0.25f, 0.5f, 0.75f);
        QVERIFY(target.Clear(1, 0, 0, 1));
        GLfloat clearColor[4] = {};
        m_gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        QCOMPARE(clearColor[0], 0.125f);
        QCOMPARE(clearColor[1], 0.25f);
        QCOMPARE(clearColor[2], 0.5f);
        QCOMPARE(clearColor[3], 0.75f);
        QVERIFY(!target.Clear(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1));
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.GetRenderTexture());
        QVERIFY(!target.Clear(0, 1, 0, 1));
        QCOMPARE(pixels(target.GetColorBuffer(), 3, 2), QByteArray::fromHex("ff0000ff").repeated(6));
        QVERIFY(target.EndDraw());
        QVERIFY(!target.EndDraw());
        QVERIFY(state() == before);
        QVERIFY(target.BeginDraw(GLint(draw.GetRenderTexture())));
        QVERIFY(target.EndDraw());
        QCOMPARE(state().read, GLint(draw.GetRenderTexture()));
        QCOMPARE(state().draw, GLint(draw.GetRenderTexture()));
        GLuint invalid = 0;
        m_gl.glGenFramebuffers(1, &invalid);
        m_gl.glDeleteFramebuffers(1, &invalid);
        const GlState explicitState = state();
        QVERIFY(!target.BeginDraw(GLint(invalid)));
        QVERIFY(state() == explicitState);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void failedReplacementPreservesPixelsResourcesAndState()
    {
        COffscreenBuffer target;
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(target.CreateOffscreenSurface(4, 3));
        QVERIFY(target.BeginDraw());
        QVERIFY(target.Clear(0, 1, 0, 1));
        QVERIFY(target.EndDraw());
        const GLuint framebuffer = target.GetRenderTexture(), color = target.GetColorBuffer();
        const GLuint wrongSize = texture(2, 2), replacement = texture(4, 3);
        GLuint depthColor = texture(4, 3, true), empty = 0, invalid = 0;
        m_gl.glGenTextures(1, &empty);
        m_gl.glBindTexture(GL_TEXTURE_2D, empty);
        m_gl.glBindTexture(GL_TEXTURE_2D, 0);
        m_gl.glGenTextures(1, &invalid);
        m_gl.glDeleteTextures(1, &invalid);
        const auto release = qScopeGuard([&] {
            const GLuint textures[] = {wrongSize, replacement, depthColor, empty};
            m_gl.glDeleteTextures(4, textures);
        });
        const GlState before = state();
        for (const auto& size : {QSize(0, 3), QSize(4, 0), QSize(-1, 3), QSize(4, -1),
                                 QSize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max())}) {
            QVERIFY(!target.CreateOffscreenSurface(size.width(), size.height()));
            QVERIFY(state() == before);
        }
        QVERIFY(!target.CreateOffscreenSurface(4, 3, invalid));
        QVERIFY(!target.CreateOffscreenSurface(4, 3, empty));
        QVERIFY(!target.CreateOffscreenSurface(4, 3, wrongSize));
        QVERIFY(!target.CreateOffscreenSurface(4, 3, depthColor));
        QVERIFY(!target.CreateOffscreenSurface(4, 3, color));
        QVERIFY(state() == before);
        QCOMPARE(target.GetRenderTexture(), framebuffer);
        QCOMPARE(target.GetColorBuffer(), color);
        QCOMPARE(target.GetBufferWidth(), 4);
        QCOMPARE(target.GetBufferHeight(), 3);
        QVERIFY(m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(m_gl.glIsTexture(color));
        QVERIFY(m_gl.glIsTexture(wrongSize));
        QVERIFY(m_gl.glIsTexture(depthColor));
        QCOMPARE(pixels(color, 4, 3), QByteArray::fromHex("00ff00ff").repeated(12));
        QVERIFY(target.CreateOffscreenSurface(4, 3, replacement));
        QVERIFY(!m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl.glIsTexture(color));
        QCOMPARE(target.GetColorBuffer(), replacement);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void borrowedTextureSurvivesRecreationDestructionAndOwnerDestruction()
    {
        GLuint borrowed = texture(3, 2);
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(1, &borrowed); });
        GLuint firstFramebuffer = 0, lastFramebuffer = 0;
        {
            COffscreenBuffer target;
            QVERIFY(target.setGlOwningContext(m_context.get()));
            QVERIFY(target.CreateOffscreenSurface(3, 2, borrowed));
            firstFramebuffer = target.GetRenderTexture();
            QVERIFY(target.BeginDraw());
            QVERIFY(target.Clear(0, 0, 1, 1));
            QVERIFY(target.EndDraw());
            QCOMPARE(pixels(borrowed, 3, 2), QByteArray::fromHex("0000ffff").repeated(6));
            QVERIFY(target.CreateOffscreenSurface(3, 2, borrowed));
            QVERIFY(!m_gl.glIsFramebuffer(firstFramebuffer));
            QVERIFY(m_gl.glIsTexture(borrowed));
            QVERIFY(target.DestroyOffscreenSurface());
            QVERIFY(!target.IsValid());
            QCOMPARE(target.GetRenderTexture(), GLuint(0));
            QCOMPARE(target.GetColorBuffer(), GLuint(0));
            QCOMPARE(target.GetBufferWidth(), 0);
            QCOMPARE(target.GetBufferHeight(), 0);
            QVERIFY(m_gl.glIsTexture(borrowed));
            QVERIFY(target.CreateOffscreenSurface(3, 2, borrowed));
            lastFramebuffer = target.GetRenderTexture();
        }
        QVERIFY(!m_gl.glIsFramebuffer(lastFramebuffer));
        QVERIFY(m_gl.glIsTexture(borrowed));
        QCOMPARE(pixels(borrowed, 3, 2), QByteArray::fromHex("0000ffff").repeated(6));
    }

    void ownedDestructionRestoresDrawingAndDeletesBothGlObjects()
    {
        COffscreenBuffer read, draw;
        QVERIFY(read.setGlOwningContext(m_context.get()));
        QVERIFY(draw.setGlOwningContext(m_context.get()));
        QVERIFY(read.CreateOffscreenSurface(1, 1));
        QVERIFY(draw.CreateOffscreenSurface(1, 1));
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, read.GetRenderTexture());
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.GetRenderTexture());
        const GlState before = state();
        GLuint framebuffer = 0, color = 0;
        {
            COffscreenBuffer target;
            QVERIFY(target.setGlOwningContext(m_context.get()));
            QVERIFY(target.CreateOffscreenSurface(4, 4));
            framebuffer = target.GetRenderTexture();
            color = target.GetColorBuffer();
            QVERIFY(target.BeginDraw());
        }
        QVERIFY(state() == before);
        QVERIFY(!m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl.glIsTexture(color));
        {
            COffscreenBuffer target;
            QVERIFY(target.setGlOwningContext(m_context.get()));
            QVERIFY(target.CreateOffscreenSurface(4, 4));
            framebuffer = target.GetRenderTexture();
            color = target.GetColorBuffer();
            QVERIFY(target.BeginDraw());
            QVERIFY(target.DestroyOffscreenSurface());
            QVERIFY(!target.EndDraw());
            QVERIFY(target.DestroyOffscreenSurface());
        }
        QVERIFY(state() == before);
        QVERIFY(!m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl.glIsTexture(color));
    }

    void replacementUnbindsDeletedOwnedObjectsWithoutTouchingOtherBindings()
    {
        COffscreenBuffer target, other;
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(other.setGlOwningContext(m_context.get()));
        QVERIFY(target.CreateOffscreenSurface(2, 2));
        QVERIFY(other.CreateOffscreenSurface(1, 1));
        const GLuint oldFramebuffer = target.GetRenderTexture(), oldTexture = target.GetColorBuffer();
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, oldFramebuffer);
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, other.GetRenderTexture());
        m_gl.glBindTexture(GL_TEXTURE_2D, oldTexture);
        QVERIFY(target.CreateOffscreenSurface(3, 3));
        QCOMPARE(state().read, GLint(0));
        QCOMPARE(state().draw, GLint(other.GetRenderTexture()));
        QCOMPARE(state().texture, GLint(0));
        QVERIFY(!m_gl.glIsFramebuffer(oldFramebuffer));
        QVERIFY(!m_gl.glIsTexture(oldTexture));
        QVERIFY(target.IsValid());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void absentAndForeignContextsCannotChangeLiveResources()
    {
        COffscreenBuffer target;
        QVERIFY(!target.CreateOffscreenSurface(1, 1));
        QVERIFY(!target.setGlOwningContext(nullptr));
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(target.CreateOffscreenSurface(3, 2));
        const GLuint framebuffer = target.GetRenderTexture(), color = target.GetColorBuffer();
        QVERIFY(target.BeginDraw());
        m_context->doneCurrent();
        const auto restore = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        QVERIFY(!target.setGlOwningContext(m_context.get()));
        QVERIFY(!target.CreateOffscreenSurface(4, 4));
        QVERIFY(!target.BeginDraw());
        QVERIFY(!target.EndDraw());
        QVERIFY(!target.Clear(1, 0, 0, 1));
        QVERIFY(!target.DestroyOffscreenSurface());
        QCOMPARE(target.GetRenderTexture(), framebuffer);
        QCOMPARE(target.GetColorBuffer(), color);
        auto foreign = std::make_unique<QOpenGLContext>();
        foreign->setFormat(m_context->format());
        if (!foreign->create() || !foreign->makeCurrent(m_surface.get()))
            QSKIP("Unable to create a second real context for ownership checks.");
        QVERIFY(!target.setGlOwningContext(foreign.get()));
        QVERIFY(!target.CreateOffscreenSurface(4, 4));
        QVERIFY(!target.BeginDraw());
        QVERIFY(!target.EndDraw());
        QVERIFY(!target.Clear(1, 0, 0, 1));
        QVERIFY(!target.DestroyOffscreenSurface());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(target.EndDraw());
        QVERIFY(target.IsValid());
        QVERIFY(m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(m_gl.glIsTexture(color));
        QVERIFY(target.DestroyOffscreenSurface());
        QVERIFY(!m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl.glIsTexture(color));
    }

    void destructorWithoutCurrentOwnerLeavesGlCleanupToTheOwner()
    {
        auto target = std::make_unique<COffscreenBuffer>();
        QVERIFY(target->setGlOwningContext(m_context.get()));
        QVERIFY(target->CreateOffscreenSurface(2, 2));
        const GLuint framebuffer = target->GetRenderTexture(), color = target->GetColorBuffer();
        m_context->doneCurrent();
        target.reset();
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(m_gl.glIsTexture(color));
        m_gl.glDeleteFramebuffers(1, &framebuffer);
        m_gl.glDeleteTextures(1, &color);
        QVERIFY(!m_gl.glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl.glIsTexture(color));
    }

    void destroyedOwnerAllowsCpuResetAndNewContextRegistration()
    {
        COffscreenBuffer target;
        auto owner = std::make_unique<QOpenGLContext>();
        owner->setFormat(m_context->format());
        const auto restore = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        if (!owner->create() || !owner->makeCurrent(m_surface.get()))
            QSKIP("Unable to create a second real context for lifetime checks.");
        QVERIFY(target.setGlOwningContext(owner.get()));
        QVERIFY(target.CreateOffscreenSurface(2, 2));
        owner->doneCurrent();
        owner.reset();
        QVERIFY(!target.IsValid());
        QVERIFY(target.DestroyOffscreenSurface());
        QCOMPARE(target.GetRenderTexture(), GLuint(0));
        QCOMPARE(target.GetColorBuffer(), GLuint(0));
        QCOMPARE(target.GetBufferWidth(), 0);
        QCOMPARE(target.GetBufferHeight(), 0);
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(target.CreateOffscreenSurface(3, 3));
        QVERIFY(target.IsValid());
    }

    void gpuLimitsAndSignedByteSizeAreCheckedBeforeAllocation()
    {
        COffscreenBuffer target;
        QVERIFY(target.setGlOwningContext(m_context.get()));
        QVERIFY(target.CreateOffscreenSurface(1, 1));
        const GLuint framebuffer = target.GetRenderTexture(), color = target.GetColorBuffer();
        const GlState before = state();
        GLint maximum = 0;
        m_gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        QVERIFY(maximum > 0);
        if (maximum < std::numeric_limits<GLint>::max()) {
            QVERIFY(!target.CreateOffscreenSurface(maximum + 1, 1));
            QVERIFY(!target.CreateOffscreenSurface(1, maximum + 1));
        }
        QVERIFY(!target.CreateOffscreenSurface(std::numeric_limits<int>::max(), 1));
        QVERIFY(!target.CreateOffscreenSurface(1, std::numeric_limits<int>::max()));
        QVERIFY(!target.CreateOffscreenSurface(32768, 32768));
        QCOMPARE(target.GetRenderTexture(), framebuffer);
        QCOMPARE(target.GetColorBuffer(), color);
        QVERIFY(state() == before);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    OffscreenBufferTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "offscreen_buffer_tests.moc"
