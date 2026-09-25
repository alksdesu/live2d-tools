#include "live2dstruct/ccanvasgeometry.h"
#include "opengl/coffscreenbuffer.h"
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
    QOpenGLFunctions_3_3_Core m_gl;
    CReadOpengl m_reader;
    std::array<float, 16> m_matrix{};
    GLuint m_vertexArray = 0, m_vertexBuffer = 0;

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
        QCOMPARE(image.pixel(3, 3), qRgba(255, 255, 255, 0));
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

    void alphaResetPreservesRgbAndSeparatesSequentialPsdLayers()
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
            QCOMPARE(cleared.mid(index * 4, 4), QByteArray::fromHex("ffffff00"));

        QVERIFY(draw(upperTriangle, qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 255)));
        QImage first;
        QRect firstRect;
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, first, firstRect));
        const QByteArray beforePixels = pixels(target.GetRenderTexture());
        hostileClearState();
        const auto beforeAlpha = state();
        QVERIFY(m_reader.UpdateFrame());
        QVERIFY(state() == beforeAlpha);
        const QByteArray afterPixels = pixels(target.GetRenderTexture());
        for (int index = 0; index < 16 * 16; ++index) {
            QCOMPARE(afterPixels.mid(index * 4, 3), beforePixels.mid(index * 4, 3));
            QCOMPARE(quint8(afterPixels[index * 4 + 3]), quint8(0));
        }
        QVERIFY(draw(lowerTriangle, qRgba(0, 0, 255, 128), qRgba(0, 0, 255, 128)));
        QImage second;
        QRect secondRect;
        QVERIFY(m_reader.GetScreenNode(lowerTriangle.data(), 3, m_matrix.data(), 16, 16, second, secondRect));
        QCOMPARE(secondRect, QRect(9, 8, 4, 4));
        QCOMPARE(second.pixel(0, 0), qRgba(0, 0, 255, 128));
        const QByteArray combined = pixels(target.GetRenderTexture());
        QCOMPARE(framebufferPixel(combined, 1, 2), qRgba(255, 0, 0, 0));
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
        QCOMPARE(image.pixel(0, 0), qRgba(255, 0, 0, 0));
        QVERIFY(m_reader.PreDraw());
        QVERIFY(m_reader.GetScreenNode(upperTriangle.data(), 3, m_matrix.data(), 16, 16, image, rect));
        QCOMPARE(image.pixel(0, 0), qRgba(255, 255, 255, 0));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    RenderedPsdTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "rendered_psd_tests.moc"
