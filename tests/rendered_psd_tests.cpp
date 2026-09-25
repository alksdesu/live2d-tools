#include "live2dstruct/ccanvasgeometry.h"
#include "opengl/coffscreenbuffer.h"
#include "opengl/ccubismshaderprograms.h"
#include "opengl/creadopengl.h"
#include "psd/cpsdlist.h"
#include "psd_binary_reader.h"

#include <QtTest>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLShaderProgram>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <array>
#include <limits>
#include <memory>
#include <vector>

using live2dstruct::CCanvasGeometry;

namespace {
constexpr std::array<float, 6> upperTriangle{-0.5f, 0.75f, 0.5f, 0.75f, -0.5f, -0.25f};
constexpr std::array<float, 6> lowerTriangle{1.5f, -0.75f, 2.5f, -0.75f, 1.5f, -1.75f};
constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr std::array<float, 16> textureMatrix{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};
constexpr std::array<float, 12> fullQuad{-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
using Variant = CCubismShaderPrograms::Variant;
using Color = std::array<float, 4>;

struct GlState
{
    GLint read = 0, draw = 0, readBuffer = 0, activeTexture = 0, texture = 0;
    GLint packBuffer = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    GLboolean scissor = GL_FALSE;
    std::array<GLint, 4> viewport{}, scissorBox{};
    std::array<GLboolean, 4> colorMask{};
    std::array<GLfloat, 4> clearColor{};
    std::vector<GLint> drawBuffers;

    bool operator==(const GlState& other) const
    {
        return read == other.read && draw == other.draw && readBuffer == other.readBuffer &&
            activeTexture == other.activeTexture && texture == other.texture && packBuffer == other.packBuffer &&
            alignment == other.alignment && rowLength == other.rowLength && skipPixels == other.skipPixels &&
            skipRows == other.skipRows && scissor == other.scissor && viewport == other.viewport &&
            scissorBox == other.scissorBox && colorMask == other.colorMask && clearColor == other.clearColor &&
            drawBuffers == other.drawBuffers;
    }
};

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QRgb framebufferPixel(const QByteArray& bytes, int x, int y)
{
    const auto* rgba = reinterpret_cast<const uchar*>(bytes.constData()) + ((15 - y) * 16 + x) * 4;
    return qRgba(rgba[0], rgba[1], rgba[2], rgba[3]);
}

QRgb layerPixel(const PsdBinary::Layer& layer, int x, int y)
{
    const int index = y * layer.bounds.width() + x;
    return qRgba(quint8(layer.Plane(0).at(index)), quint8(layer.Plane(1).at(index)),
                 quint8(layer.Plane(2).at(index)), quint8(layer.Plane(-1).at(index)));
}
}

class RenderedPsdTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<CCubismShaderPrograms> m_cubismPrograms;
    QOpenGLFunctions_3_3_Core m_gl;
    CReadOpengl m_reader;
    std::array<float, 16> m_matrix{};
    GLuint m_vertexArray = 0, m_vertexBuffer = 0;
    GLuint m_layerVertexArray = 0, m_layerVertexBuffer = 0;

    GLuint texture(const Color& color)
    {
        GLuint texture = 0;
        m_gl.glGenTextures(1, &texture);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, color.data());
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return texture;
    }

    bool drawLayer(Variant variant, GLuint texture, GLuint mask = 0)
    {
        m_gl.glDrawBuffer(GL_COLOR_ATTACHMENT0);
        m_gl.glViewport(0, 0, 16, 16);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (GLenum capability : {GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                                  GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DITHER}) m_gl.glDisable(capability);
        m_gl.glEnable(GL_BLEND);
        if (!m_cubismPrograms->Bind(variant)) return false;
        const auto* program = m_cubismPrograms->Get(variant);
        if (!program) return false;
        const bool setup = variant == Variant::SetupMask;
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        m_gl.glUniform1i(program->sampler0, 0);
        m_gl.glActiveTexture(GL_TEXTURE1);
        m_gl.glBindTexture(GL_TEXTURE_2D, mask);
        m_gl.glUniform1i(program->sampler1, 1);
        m_gl.glUniformMatrix4fv(program->matrix, 1, GL_FALSE, identity.data());
        m_gl.glUniformMatrix4fv(program->clipMatrix, 1, GL_FALSE, setup ? identity.data() : textureMatrix.data());
        m_gl.glUniform4f(program->baseColor, setup ? -1 : 1, setup ? -1 : 1, 1, 1);
        m_gl.glUniform4f(program->multiplyColor, 1, 1, 1, 1);
        m_gl.glUniform4f(program->screenColor, 0, 0, 0, 0);
        m_gl.glUniform4f(program->channelFlag, 1, 0, 0, 0);
        m_gl.glBindVertexArray(m_layerVertexArray);
        m_gl.glDrawArrays(GL_TRIANGLES, 0, 6);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    bool readLayer(QImage& image, QRect& rect)
    {
        return m_reader.GetScreenNode(fullQuad.data(), 6, identity.data(), 16, 16, image, rect);
    }

    bool create(COffscreenBuffer& target)
    {
        return target.setGlOwningContext(m_context.get()) && target.CreateOffscreenSurface(16, 16);
    }

    GlState state()
    {
        GlState value;
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &value.read);
        m_gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value.draw);
        m_gl.glGetIntegerv(GL_READ_BUFFER, &value.readBuffer);
        m_gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &value.activeTexture);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &value.texture);
        m_gl.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value.packBuffer);
        m_gl.glGetIntegerv(GL_PACK_ALIGNMENT, &value.alignment);
        m_gl.glGetIntegerv(GL_PACK_ROW_LENGTH, &value.rowLength);
        m_gl.glGetIntegerv(GL_PACK_SKIP_PIXELS, &value.skipPixels);
        m_gl.glGetIntegerv(GL_PACK_SKIP_ROWS, &value.skipRows);
        m_gl.glGetIntegerv(GL_VIEWPORT, value.viewport.data());
        m_gl.glGetIntegerv(GL_SCISSOR_BOX, value.scissorBox.data());
        m_gl.glGetBooleani_v(GL_COLOR_WRITEMASK, 0, value.colorMask.data());
        m_gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, value.clearColor.data());
        value.scissor = m_gl.glIsEnabled(GL_SCISSOR_TEST);
        GLint count = 0;
        m_gl.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &count);
        value.drawBuffers.resize(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
            m_gl.glGetIntegerv(GL_DRAW_BUFFER0 + index, &value.drawBuffers[static_cast<std::size_t>(index)]);
        return value;
    }

    QByteArray pixels(GLuint framebuffer)
    {
        const GlState previous = state();
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        GLint targetReadBuffer = 0;
        m_gl.glGetIntegerv(GL_READ_BUFFER, &targetReadBuffer);
        const auto restore = qScopeGuard([&] {
            m_gl.glReadBuffer(targetReadBuffer);
            m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, previous.read);
            m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, previous.packBuffer);
            m_gl.glPixelStorei(GL_PACK_ALIGNMENT, previous.alignment);
            m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, previous.rowLength);
            m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, previous.skipPixels);
            m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, previous.skipRows);
        });
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        m_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        QByteArray result(16 * 16 * 4, '\0');
        m_gl.glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        return result;
    }

    bool draw(const std::array<float, 6>& vertices, QRgb top, QRgb bottom)
    {
        m_gl.glDrawBuffer(GL_COLOR_ATTACHMENT0);
        m_gl.glViewport(0, 0, 16, 16);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (GLenum capability : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                                  GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DITHER}) m_gl.glDisable(capability);
        if (!m_program->bind()) return false;
        m_gl.glUniformMatrix4fv(m_program->uniformLocation("matrix"), 1, GL_FALSE, m_matrix.data());
        const auto color = [&](const char* name, QRgb pixel) {
            m_gl.glUniform4f(m_program->uniformLocation(name), qRed(pixel) / 255.0f, qGreen(pixel) / 255.0f,
                            qBlue(pixel) / 255.0f, qAlpha(pixel) / 255.0f);
        };
        color("topColor", top);
        color("bottomColor", bottom);
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
        m_gl.glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STREAM_DRAW);
        m_gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    void hostileClearState()
    {
        m_gl.glViewport(2, 3, 7, 9);
        m_gl.glScissor(8, 8, 1, 1);
        m_gl.glEnable(GL_SCISSOR_TEST);
        m_gl.glColorMaski(0, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        m_gl.glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        m_gl.glDrawBuffer(GL_NONE);
    }

    bool rejected(const float* vertices, int count, const float* matrix, int width, int height)
    {
        QImage original(2, 3, QImage::Format_RGBA8888);
        original.fill(qRgba(17, 29, 41, 53));
        QImage image = original;
        const QRect originalRect(-7, 11, 2, 3);
        QRect rect = originalRect;
        return !m_reader.GetScreenNode(vertices, count, matrix, width, height, image, rect) &&
            image == original && rect == originalRect;
    }

private slots:
    void initTestCase()
    {
        QSurfaceFormat requested;
        requested.setRenderableType(QSurfaceFormat::OpenGL);
        requested.setVersion(3, 3);
        requested.setProfile(QSurfaceFormat::CoreProfile);
        requested.setSamples(0);
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(requested);
        if (!m_context->create() || !m_context->isValid()) QSKIP("Unable to create a real OpenGL 3.3 Core context.");
        const auto actual = m_context->format();
        if (actual.renderableType() != QSurfaceFormat::OpenGL || actual.profile() != QSurfaceFormat::CoreProfile ||
            actual.majorVersion() < 3 || (actual.majorVersion() == 3 && actual.minorVersion() < 3))
            QSKIP("The returned context does not provide OpenGL 3.3 Core.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get()))
            QSKIP("Unable to make the offscreen surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable.");
        QVERIFY(m_reader.setGlOwningContext(m_context.get()));
        QVERIFY(CCanvasGeometry::ModelToNdc(16, 16, 3, 5, 4, QSize(16, 16), m_matrix));
        m_program = std::make_unique<QOpenGLShaderProgram>();
        QVERIFY2(m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 330 core\nlayout(location=0) in vec2 position; uniform mat4 matrix;"
            "void main(){ gl_Position=matrix*vec4(position,0,1); }"), qPrintable(m_program->log()));
        QVERIFY2(m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 330 core\nuniform vec4 topColor; uniform vec4 bottomColor; out vec4 color;"
            "void main(){ color=gl_FragCoord.y>12.0?topColor:bottomColor; }"), qPrintable(m_program->log()));
        QVERIFY2(m_program->link(), qPrintable(m_program->log()));
        m_gl.glGenVertexArrays(1, &m_vertexArray);
        m_gl.glGenBuffers(1, &m_vertexBuffer);
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
        m_gl.glEnableVertexAttribArray(0);
        m_gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        m_cubismPrograms = std::make_unique<CCubismShaderPrograms>();
        QVERIFY2(m_cubismPrograms->Initialize(m_context.get()), qPrintable(m_cubismPrograms->Error()));
        constexpr std::array<float, 24> vertices{
            -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1,
            -1, 1, 0, 1, 1, -1, 1, 0, 1, 1, 1, 1};
        m_gl.glGenVertexArrays(1, &m_layerVertexArray);
        m_gl.glGenBuffers(1, &m_layerVertexBuffer);
        m_gl.glBindVertexArray(m_layerVertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_layerVertexBuffer);
        m_gl.glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
        m_gl.glEnableVertexAttribArray(0);
        m_gl.glEnableVertexAttribArray(1);
        m_gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        m_gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        qInfo() << "Rendered PSD GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        m_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, 0);
        m_gl.glDisable(GL_SCISSOR_TEST);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanup()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        if (!m_context || !m_surface || !m_context->makeCurrent(m_surface.get())) return;
        m_program.reset();
        m_cubismPrograms.reset();
        if (m_layerVertexBuffer) m_gl.glDeleteBuffers(1, &m_layerVertexBuffer);
        if (m_layerVertexArray) m_gl.glDeleteVertexArrays(1, &m_layerVertexArray);
        if (m_vertexBuffer) m_gl.glDeleteBuffers(1, &m_vertexBuffer);
        if (m_vertexArray) m_gl.glDeleteVertexArrays(1, &m_vertexArray);
        m_context->doneCurrent();
    }

    void asymmetricTriangleHasCorrectReadbackAndPsdOrientation()
    {
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(draw(upperTriangle, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255)));
        QImage image;
        QRect rect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(rect, QRect(1, 2, 4, 4));
        QCOMPARE(image.size(), QSize(4, 4));
        QCOMPARE(image.pixel(0, 0), qRgba(255, 0, 0, 255));
        QCOMPARE(image.pixel(0, 2), qRgba(0, 255, 0, 255));
        QCOMPARE(image.pixel(3, 3), qRgba(0, 0, 0, 0));
        const QByteArray full = pixels(target.GetRenderTexture());
        QCOMPARE(framebufferPixel(full, 1, 2), image.pixel(0, 0));
        QCOMPARE(framebufferPixel(full, 1, 4), image.pixel(0, 2));
        QCOMPARE(qAlpha(framebufferPixel(full, 1, 12)), 0);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("orientation.psd");
        CPsdList writer;
        QVERIFY(writer.SetPsd(path, 16, 16));
        writer.AddLayer(QStringLiteral("upper triangle"), image, rect);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(readFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.canvas, QSize(16, 16));
        QCOMPARE(parsed.layers.size(), 1);
        QCOMPARE(parsed.layers[0].bounds, rect);
        QCOMPARE(layerPixel(parsed.layers[0], 0, 0), qRgba(255, 0, 0, 255));
        QCOMPARE(layerPixel(parsed.layers[0], 0, 2), qRgba(0, 255, 0, 255));
        QCOMPARE(layerPixel(parsed.layers[0], 3, 3), qRgba(0, 0, 0, 0));
    }

    void fullResetSeparatesSequentialPsdLayersAndPreservesGlState()
    {
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        const GLfloat initial[]{0.0f, 0.0f, 0.0f, 1.0f};
        m_gl.glClearBufferfv(GL_COLOR, 0, initial);
        m_gl.glReadBuffer(GL_NONE);
        hostileClearState();
        const auto beforeClear = state();
        QVERIFY(m_reader.PreDraw());
        QVERIFY(state() == beforeClear);
        const QByteArray cleared = pixels(target.GetRenderTexture());
        for (int index = 0; index < 16 * 16; ++index)
            QCOMPARE(cleared.mid(index * 4, 4), QByteArray::fromHex("00000000"));

        QVERIFY(draw(upperTriangle, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255)));
        QImage first;
        QRect firstRect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, first, firstRect));
        hostileClearState();
        const auto beforeReset = state();
        QVERIFY(m_reader.UpdateFrame());
        QVERIFY(state() == beforeReset);
        const QByteArray afterPixels = pixels(target.GetRenderTexture());
        for (int index = 0; index < 16 * 16; ++index) {
            QCOMPARE(afterPixels.mid(index * 4, 4), QByteArray::fromHex("00000000"));
        }
        QVERIFY(draw(lowerTriangle, qRgba(0, 0, 255, 128), qRgba(0, 0, 255, 128)));
        QImage second;
        QRect secondRect;
        QVERIFY(m_reader.GetScreenNode(lowerTriangle.data(), 3, m_matrix.data(), 16, 16, second, secondRect));
        QCOMPARE(secondRect, QRect(9, 8, 4, 4));
        QCOMPARE(second.pixel(0, 0), qRgba(0, 0, 255, 128));
        const QByteArray combined = pixels(target.GetRenderTexture());
        QCOMPARE(framebufferPixel(combined, 1, 2), qRgba(0, 0, 0, 0));
        QCOMPARE(framebufferPixel(combined, 9, 8), qRgba(0, 0, 255, 128));
        QCOMPARE(first.pixel(0, 0), qRgba(255, 0, 0, 255));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        CPsdList writer;
        const QString path = directory.filePath("layers.psd");
        QVERIFY(writer.SetPsd(path, 16, 16));
        writer.AddLayer("first", first, firstRect);
        writer.AddLayer("second", second, secondRect);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(readFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 2);
        QCOMPARE(parsed.layers[0].name, QString("first"));
        QCOMPARE(parsed.layers[1].name, QString("second"));
        QCOMPARE(parsed.layers[0].bounds, QRect(1, 2, 4, 4));
        QCOMPARE(parsed.layers[1].bounds, QRect(9, 8, 4, 4));
        QCOMPARE(layerPixel(parsed.layers[0], 0, 0), qRgba(255, 0, 0, 255));
        QCOMPARE(layerPixel(parsed.layers[1], 0, 0), qRgba(0, 0, 255, 128));
        QCOMPARE(layerPixel(parsed.layers[1], 3, 3), qRgba(0, 0, 0, 0));
    }

    void halfTransparentBlackIsIndependentOfPreviousRedLayer()
    {
        const std::array<GLuint, 2> textures{texture({0, 0, 0, 0.5f}), texture({1, 0, 0, 1})};
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(drawLayer(Variant::Straight, textures[0]));
        QImage isolated, afterRed;
        QRect isolatedRect, afterRedRect;
        QVERIFY(readLayer(isolated, isolatedRect));
        QCOMPARE(isolatedRect, QRect(0, 0, 16, 16));
        QCOMPARE(isolated.pixel(8, 8), qRgba(0, 0, 0, 128));
        QVERIFY(m_reader.UpdateFrame());
        QVERIFY(drawLayer(Variant::Straight, textures[1]));
        QImage red;
        QRect redRect;
        QVERIFY(readLayer(red, redRect));
        QCOMPARE(red.pixel(8, 8), qRgba(255, 0, 0, 255));
        hostileClearState();
        const auto before = state();
        QVERIFY(m_reader.UpdateFrame());
        QVERIFY(state() == before);
        QVERIFY(drawLayer(Variant::Straight, textures[0]));
        QVERIFY(readLayer(afterRed, afterRedRect));
        QCOMPARE(afterRedRect, isolatedRect);
        QCOMPARE(afterRed, isolated);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("independent-layers.psd");
        CPsdList writer;
        QVERIFY(writer.SetPsd(path, 16, 16));
        writer.AddLayer("black-first", isolated, isolatedRect, 255, 0x6e6f726d, false, true);
        writer.AddLayer("red", red, redRect, 255, 0x6e6f726d, false, true);
        writer.AddLayer("black-after-red", afterRed, afterRedRect, 255, 0x6e6f726d, false, true);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(readFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 3);
        for (int channel : {0, 1, 2, -1}) QCOMPARE(parsed.layers[0].Plane(channel), parsed.layers[2].Plane(channel));
        QCOMPARE(layerPixel(parsed.layers[0], 8, 8), qRgba(0, 0, 0, 128));
        QCOMPARE(layerPixel(parsed.layers[1], 8, 8), qRgba(255, 0, 0, 255));
    }

    void productionShaderPixelsKeepBlendAndOpacityInMetadata_data()
    {
        QTest::addColumn<QByteArray>("blend");
        QTest::addColumn<quint32>("blendKey");
        QTest::addColumn<float>("textureAlpha");
        QTest::addColumn<bool>("premultipliedTexture");
        const std::array<QByteArray, 3> names{"norm", "lddg", "mul "};
        const std::array<quint32, 3> keys{0x6e6f726d, 0x6c646467, 0x6d756c20};
        for (int blend = 0; blend < 3; ++blend)
            for (float alpha : {0.5f, 1.0f})
                for (bool premultiplied : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1-alpha-%2-premultiplied-%3")
                        .arg(QString::fromLatin1(names[blend])).arg(alpha).arg(premultiplied)))
                        << names[blend] << keys[blend] << alpha << premultiplied;
    }

    void productionShaderPixelsKeepBlendAndOpacityInMetadata()
    {
        QFETCH(QByteArray, blend);
        QFETCH(quint32, blendKey);
        QFETCH(float, textureAlpha);
        QFETCH(bool, premultipliedTexture);
        Color texel{0.5f, 0.25f, 0.125f, textureAlpha};
        if (premultipliedTexture) for (int channel = 0; channel < 3; ++channel) texel[channel] *= textureAlpha;
        const GLuint source = texture(texel);
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(1, &source); });
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(drawLayer(premultipliedTexture ? Variant::Premultiplied : Variant::Straight, source));
        QImage image;
        QRect rect;
        QVERIFY(readLayer(image, rect));
        const int alpha = qRound(textureAlpha * 255);
        const QRgb readback = image.pixel(8, 8);
        QCOMPARE(qAlpha(readback), alpha);
        QVERIFY(qAbs(qRed(readback) - qRound(0.5 * textureAlpha * 255)) <= 1);
        QVERIFY(qAbs(qGreen(readback) - qRound(0.25 * textureAlpha * 255)) <= 1);
        QVERIFY(qAbs(qBlue(readback) - qRound(0.125 * textureAlpha * 255)) <= 1);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("metadata.psd");
        CPsdList writer;
        QVERIFY(writer.SetPsd(path, 16, 16));
        constexpr quint8 opacity = 127;
        writer.AddLayer("material", image, rect, opacity, blendKey, false, true);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(readFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 1);
        const auto& layer = parsed.layers[0];
        QCOMPARE(layer.opacity, opacity);
        QCOMPARE(layer.blend, blend);
        QCOMPARE(layer.clipping, quint8(0));
        QCOMPARE(layer.bounds, QRect(0, 0, 16, 16));
        const QRgb straight = layerPixel(layer, 8, 8);
        QCOMPARE(qAlpha(straight), alpha);
        QVERIFY(qAbs(qRed(straight) - 128) <= 1);
        QVERIFY(qAbs(qGreen(straight) - 64) <= 1);
        QVERIFY(qAbs(qBlue(straight) - 32) <= 1);
        const double effectiveAlpha = double(qAlpha(straight)) * layer.opacity / 255;
        QVERIFY(qAbs(effectiveAlpha - double(textureAlpha) * opacity) <= 1);
    }

    void combinedAndInvertedMasksAreBakedOnceIntoPsdAlpha_data()
    {
        QTest::addColumn<bool>("inverted");
        QTest::addColumn<bool>("premultipliedTexture");
        for (bool inverted : {false, true})
            for (bool premultiplied : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("inverse-%1-premultiplied-%2").arg(inverted).arg(premultiplied)))
                    << inverted << premultiplied;
    }

    void combinedAndInvertedMasksAreBakedOnceIntoPsdAlpha()
    {
        QFETCH(bool, inverted);
        QFETCH(bool, premultipliedTexture);
        const std::array<GLuint, 3> textures{texture({1, 1, 1, 0.25f}), texture({1, 1, 1, 0.5f}),
                                             texture({0.25f, 0.5f, 0.75f, 1})};
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        COffscreenBuffer mask, target;
        QVERIFY(create(mask));
        QVERIFY(create(target));
        QVERIFY(mask.BeginDraw());
        QVERIFY(mask.Clear(1, 1, 1, 1));
        QVERIFY(drawLayer(Variant::SetupMask, textures[0]));
        QVERIFY(drawLayer(Variant::SetupMask, textures[1]));
        QVERIFY(mask.EndDraw());
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        const auto variant = premultipliedTexture
            ? (inverted ? Variant::PremultipliedInverted : Variant::PremultipliedMasked)
            : (inverted ? Variant::StraightInverted : Variant::StraightMasked);
        QVERIFY(drawLayer(variant, textures[2], mask.GetColorBuffer()));
        QImage image;
        QRect rect;
        QVERIFY(readLayer(image, rect));
        const double unionAlpha = 1 - (1 - 0.25) * (1 - 0.5);
        const double coverage = inverted ? 1 - unionAlpha : unionAlpha;
        const int expectedAlpha = qRound(coverage * 255);
        QVERIFY(qAbs(qAlpha(image.pixel(8, 8)) - expectedAlpha) <= 1);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("baked-mask.psd");
        CPsdList writer;
        QVERIFY(writer.SetPsd(path, 16, 16));
        constexpr quint8 opacity = 153;
        writer.AddLayer("masked", image, rect, opacity, 0x6e6f726d, false, true);
        QVERIFY(writer.CreatePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(readFile(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.size(), 1);
        const auto& layer = parsed.layers[0];
        QCOMPARE(layer.opacity, opacity);
        QCOMPARE(layer.blend, QByteArray("norm"));
        QCOMPARE(layer.clipping, quint8(0));
        const QRgb pixel = layerPixel(layer, 8, 8);
        QVERIFY(qAbs(qAlpha(pixel) - expectedAlpha) <= 1);
        QVERIFY(qAbs(qRed(pixel) - 64) <= 2);
        QVERIFY(qAbs(qGreen(pixel) - 128) <= 2);
        QVERIFY(qAbs(qBlue(pixel) - 191) <= 2);
        QVERIFY(qAbs(double(qAlpha(pixel)) * layer.opacity / 255 - coverage * opacity) <= 1);
    }

    void readbackUsesDrawFramebufferAndPreservesHostilePackState()
    {
        COffscreenBuffer target, readTarget;
        QVERIFY(create(target));
        QVERIFY(create(readTarget));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(draw(upperTriangle, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255)));
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, readTarget.GetRenderTexture());
        const GLfloat sentinelColor[]{0.0f, 0.0f, 1.0f, 1.0f};
        m_gl.glClearBufferfv(GL_COLOR, 0, sentinelColor);
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.GetRenderTexture());
        m_gl.glReadBuffer(GL_NONE);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readTarget.GetRenderTexture());
        m_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        GLuint pack = 0;
        m_gl.glGenBuffers(1, &pack);
        const auto release = qScopeGuard([&] {
            m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_gl.glDeleteBuffers(1, &pack);
            m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
            m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            m_gl.glActiveTexture(GL_TEXTURE0);
        });
        const QByteArray contents(4096, char(0x5a));
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
        m_gl.glBufferData(GL_PIXEL_PACK_BUFFER, contents.size(), contents.constData(), GL_STATIC_DRAW);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 8);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 13);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 5);
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, readTarget.GetColorBuffer());
        hostileClearState();
        const auto before = state();
        QImage image;
        QRect rect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QVERIFY(state() == before);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, target.GetRenderTexture());
        GLint targetReadBuffer = 0;
        m_gl.glGetIntegerv(GL_READ_BUFFER, &targetReadBuffer);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readTarget.GetRenderTexture());
        QCOMPARE(targetReadBuffer, GLint(GL_NONE));
        QVERIFY(state() == before);
        QCOMPARE(rect, QRect(1, 2, 4, 4));
        QCOMPARE(image.pixel(0, 0), qRgba(255, 0, 0, 255));
        QByteArray after(contents.size(), '\0');
        m_gl.glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, after.size(), after.data());
        QCOMPARE(after, contents);
        const QByteArray otherPixels = pixels(readTarget.GetRenderTexture());
        QCOMPARE(framebufferPixel(otherPixels, 1, 2), qRgba(0, 0, 255, 255));
        QVERIFY(state() == before);
    }

    void clippingAndEmptyGeometryHaveDefinedOutputs()
    {
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        const std::array<float, 6> clipped{-1.25f, 0.75f, 0.75f, 0.75f, -1.25f, -1.25f};
        QVERIFY(draw(clipped, qRgba(31, 79, 151, 255), qRgba(31, 79, 151, 255)));
        QImage image;
        QRect rect;
        QVERIFY(m_reader.GetScreenNode(clipped.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(rect, QRect(0, 2, 6, 8));
        QCOMPARE(image.size(), QSize(6, 8));
        QCOMPARE(image.pixel(0, 0), qRgba(31, 79, 151, 255));
        QCOMPARE(qAlpha(image.pixel(5, 7)), 0);
        const std::array<float, 6> outside{10, 10, 11, 10, 10, 11};
        QVERIFY(m_reader.GetScreenNode(outside.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QVERIFY(image.isNull());
        QVERIFY(rect.isEmpty());
        image = QImage(2, 2, QImage::Format_RGBA8888);
        rect = QRect(1, 2, 3, 4);
        QVERIFY(m_reader.GetScreenNode(nullptr, 0, m_matrix.data(), 16, 16, image, rect));
        QVERIFY(image.isNull());
        QVERIFY(rect.isEmpty());
    }

    void invalidInputsPreserveImageAndRectangle()
    {
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(rejected(nullptr, 3, m_matrix.data(), 16, 16));
        QVERIFY(rejected(upperTriangle.data(), -1, m_matrix.data(), 16, 16));
        QVERIFY(rejected(upperTriangle.data(), 1000001, m_matrix.data(), 16, 16));
        QVERIFY(rejected(upperTriangle.data(), 3, nullptr, 16, 16));
        for (const QSize size : {QSize(0, 16), QSize(16, -1), QSize(17, 16), QSize(16, 15)})
            QVERIFY(rejected(upperTriangle.data(), 3, m_matrix.data(), size.width(), size.height()));
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
            for (int index : {0, 5, 10, 12, 15}) {
                auto matrix = m_matrix;
                matrix[index] = invalid;
                QVERIFY(rejected(upperTriangle.data(), 3, matrix.data(), 16, 16));
            }
            for (int index : {0, 3, 5}) {
                auto vertices = upperTriangle;
                vertices[index] = invalid;
                QVERIFY(rejected(vertices.data(), 3, m_matrix.data(), 16, 16));
            }
        }
        auto noPerspectiveDivide = m_matrix;
        noPerspectiveDivide[15] = 0;
        QVERIFY(rejected(upperTriangle.data(), 3, noPerspectiveDivide.data(), 16, 16));
        m_gl.glEnable(std::numeric_limits<GLenum>::max());
        QVERIFY(rejected(upperTriangle.data(), 3, m_matrix.data(), 16, 16));
        while (m_gl.glGetError() != GL_NO_ERROR) {}
        QImage image;
        QRect rect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
    }

    void unsupportedFramebufferAttachmentsFailAtomically_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("default") << 0;
        QTest::newRow("incomplete") << 1;
        QTest::newRow("integer-texture") << 2;
        QTest::newRow("multisample-texture") << 3;
        QTest::newRow("renderbuffer") << 4;
    }

    void unsupportedFramebufferAttachmentsFailAtomically()
    {
        QFETCH(int, type);
        GLuint framebuffer = 0, texture = 0, renderbuffer = 0;
        const auto release = qScopeGuard([&] {
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (framebuffer) m_gl.glDeleteFramebuffers(1, &framebuffer);
            if (texture) m_gl.glDeleteTextures(1, &texture);
            if (renderbuffer) m_gl.glDeleteRenderbuffers(1, &renderbuffer);
        });
        if (type != 0) {
            m_gl.glGenFramebuffers(1, &framebuffer);
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        }
        if (type == 2 || type == 3) {
            const GLenum target = type == 2 ? GL_TEXTURE_2D : GL_TEXTURE_2D_MULTISAMPLE;
            m_gl.glGenTextures(1, &texture);
            m_gl.glBindTexture(target, texture);
            if (type == 2) {
                m_gl.glTexImage2D(target, 0, GL_RGBA8UI, 16, 16, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
            } else {
                GLint samples = 0;
                m_gl.glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &samples);
                if (samples < 2) QSKIP("Multisample color textures are unavailable.");
                m_gl.glTexImage2DMultisample(target, 2, GL_RGBA8, 16, 16, GL_TRUE);
            }
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture, 0);
        } else if (type == 4) {
            m_gl.glGenRenderbuffers(1, &renderbuffer);
            m_gl.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            m_gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
            m_gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        }
        if (type >= 2) QCOMPARE(m_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(rejected(upperTriangle.data(), 3, m_matrix.data(), 16, 16));
        while (m_gl.glGetError() != GL_NO_ERROR) {}
    }

    void wrongContextFailsWithoutChangingOutputsAndOwnerCanResume()
    {
        COffscreenBuffer target;
        QVERIFY(create(target));
        QVERIFY(target.BeginDraw());
        QVERIFY(m_reader.PreDraw());
        QVERIFY(draw(upperTriangle, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255)));
        QOpenGLContext other;
        other.setFormat(m_context->format());
        QVERIFY(other.create());
        QOffscreenSurface otherSurface;
        otherSurface.setFormat(other.format());
        otherSurface.create();
        QVERIFY(otherSurface.isValid());
        const auto restore = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(!m_reader.setGlOwningContext(&other));
        m_context->doneCurrent();
        QVERIFY(!m_reader.setGlOwningContext(m_context.get()));
        QVERIFY(!m_reader.PreDraw());
        QVERIFY(!m_reader.UpdateFrame());
        QVERIFY(rejected(upperTriangle.data(), 3, m_matrix.data(), 16, 16));
        QVERIFY(other.makeCurrent(&otherSurface));
        QVERIFY(!m_reader.setGlOwningContext(m_context.get()));
        QVERIFY(!m_reader.PreDraw());
        QVERIFY(!m_reader.UpdateFrame());
        QVERIFY(rejected(upperTriangle.data(), 3, m_matrix.data(), 16, 16));
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QImage image;
        QRect rect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(image.pixel(0, 0), qRgba(255, 0, 0, 255));
        QCOMPARE(rect, QRect(1, 2, 4, 4));
        QVERIFY(m_reader.UpdateFrame());
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(image.pixel(0, 0), qRgba(0, 0, 0, 0));
        QVERIFY(m_reader.PreDraw());
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(image.pixel(0, 0), qRgba(0, 0, 0, 0));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    RenderedPsdTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "rendered_psd_tests.moc"
