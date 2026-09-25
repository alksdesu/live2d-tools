#include "edit/cdrawwarp.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>

using namespace live2dstruct;

namespace {
struct GlState
{
    GLint vertexArray = 0;
    GLint vertexBuffer = 0;
    GLint indexBuffer = 0;
    GLint program = 0;
    GLfloat lineWidth = 0;

    bool operator==(const GlState& other) const
    {
        return vertexArray == other.vertexArray && vertexBuffer == other.vertexBuffer &&
               indexBuffer == other.indexBuffer && program == other.program && lineWidth == other.lineWidth;
    }

    QString Describe() const
    {
        return QStringLiteral("VAO=%1 VBO=%2 EBO=%3 program=%4 lineWidth=%5")
            .arg(vertexArray).arg(vertexBuffer).arg(indexBuffer).arg(program).arg(lineWidth);
    }
};

bool foreground(QRgb pixel)
{
    return qRed(pixel) > 32 || qGreen(pixel) > 32 || qBlue(pixel) > 32;
}

int foregroundCount(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (foreground(image.pixel(x, y))) ++count;
    return count;
}

bool nearAny(int coordinate, std::initializer_list<int> lines)
{
    return std::any_of(lines.begin(), lines.end(), [coordinate](int line) { return std::abs(coordinate - line) <= 2; });
}

bool foregroundNear(const QImage& image, int centerX, int centerY)
{
    for (int y = centerY - 2; y <= centerY + 2; ++y)
        for (int x = centerX - 2; x <= centerX + 2; ++x)
            if (foreground(image.pixel(x, y))) return true;
    return false;
}

class WarpProbe : public CDrawWarp
{
public:
    QOpenGLContext* Context() const { return owningContext(); }
    bool Initialized() const { return isInitialized(); }
};
}

class WarpRenderTests : public QObject
{
    Q_OBJECT

    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    QOpenGLFunctions_3_3_Core m_gl;

    GlState State()
    {
        GlState state;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vertexArray);
        m_gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.vertexBuffer);
        m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state.indexBuffer);
        m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
        m_gl.glGetFloatv(GL_LINE_WIDTH, &state.lineWidth);
        return state;
    }

private slots:
    void initTestCase()
    {
        QSurfaceFormat requested;
        requested.setRenderableType(QSurfaceFormat::OpenGL);
        requested.setVersion(3, 3);
        requested.setProfile(QSurfaceFormat::CoreProfile);
        requested.setDepthBufferSize(24);
        requested.setStencilBufferSize(8);
        requested.setSamples(0);
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(requested);
        if (!m_context->create() || !m_context->isValid())
            QSKIP("The current Qt platform could not create the requested OpenGL 3.3 Core context.");
        const QSurfaceFormat actual = m_context->format();
        if (actual.renderableType() != QSurfaceFormat::OpenGL || actual.profile() != QSurfaceFormat::CoreProfile ||
            actual.majorVersion() < 3 || (actual.majorVersion() == 3 && actual.minorVersion() < 3)) {
            const QByteArray reason = QStringLiteral("Requested OpenGL 3.3 Core; received version %1.%2, profile %3, renderable type %4.")
                .arg(actual.majorVersion()).arg(actual.minorVersion()).arg(actual.profile()).arg(actual.renderableType()).toUtf8();
            QSKIP(reason.constData());
        }
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid()) QSKIP("Qt could not create an offscreen surface for the Core context.");
        if (!m_context->makeCurrent(m_surface.get())) QSKIP("The Core context could not make the offscreen surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable in the created context.");
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        qInfo() << "Warp GL test:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
    }

    void canvasGridRendersLinesAndRestoresGlState()
    {
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        const auto source = std::find_if(table.warp_deformers.begin(), table.warp_deformers.end(), [](const CWarp_deformers* warp) {
            return warp && warp->vertex_counts >= 12 && warp->keyform_sources_counts > 0 &&
                   warp->keyform_sources_begin_indices >= 0;
        });
        QVERIFY(source != table.warp_deformers.end());
        CWarp_deformers definition = **source;
        definition.columns = 2;
        definition.rows = 3;
        definition.vertex_counts = 12;
        definition.keyform_sources_counts = 1;
        CWarpNode node(QStringLiteral("render-test"));
        node.InitWarp(definition, table);
        QVERIFY(!node.GetKeyFormS().empty());
        const std::vector<float> positions{-0.75f, -0.75f, 0, -0.75f, 0.75f, -0.75f,
                                           -0.75f, -0.25f, 0, -0.25f, 0.75f, -0.25f,
                                           -0.75f, 0.25f, 0, 0.25f, 0.75f, 0.25f,
                                           -0.75f, 0.75f, 0, 0.75f, 0.75f, 0.75f};
        std::vector<float> unrelatedLocalPositions(24, 9.0f);
        node.GetCurKeyForm().SetPosition(unrelatedLocalPositions);
        QVERIFY(node.UpdateCanvasPositions(positions));

        QOpenGLFramebufferObjectFormat framebufferFormat;
        framebufferFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        framebufferFormat.setSamples(0);
        framebufferFormat.setInternalTextureFormat(GL_RGBA8);
        QOpenGLFramebufferObject framebuffer(256, 256, framebufferFormat);
        QVERIFY(framebuffer.isValid());
        QVERIFY(framebuffer.bind());
        CDrawWarp warp;
        warp.Init(&node);
        warp.SetDraw(true);
        warp.SetColor(QVector3D(1, 1, 1));

        QOpenGLShaderProgram sentinelProgram;
        QVERIFY2(sentinelProgram.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 330 core\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"), qPrintable(sentinelProgram.log()));
        QVERIFY2(sentinelProgram.addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 330 core\nout vec4 color; void main() { color = vec4(1, 0, 1, 1); }"), qPrintable(sentinelProgram.log()));
        QVERIFY2(sentinelProgram.link(), qPrintable(sentinelProgram.log()));
        GLuint sentinelVertexArray = 0;
        std::array<GLuint, 2> sentinelBuffers{};
        m_gl.glGenVertexArrays(1, &sentinelVertexArray);
        m_gl.glGenBuffers(static_cast<GLsizei>(sentinelBuffers.size()), sentinelBuffers.data());
        const auto releaseSentinels = qScopeGuard([&] {
            m_gl.glBindVertexArray(0);
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
            m_gl.glUseProgram(0);
            m_gl.glDeleteBuffers(static_cast<GLsizei>(sentinelBuffers.size()), sentinelBuffers.data());
            m_gl.glDeleteVertexArrays(1, &sentinelVertexArray);
        });
        QVERIFY(sentinelVertexArray != 0 && sentinelBuffers[0] != 0 && sentinelBuffers[1] != 0);
        m_gl.glBindVertexArray(sentinelVertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffers[0]);
        m_gl.glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sentinelBuffers[1]);
        m_gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        GLfloat lineWidthRange[2]{};
        m_gl.glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lineWidthRange);
        const GLfloat sentinelLineWidth = lineWidthRange[0] <= 2.0f && lineWidthRange[1] >= 2.0f ? 2.0f : 1.0f;
        QMatrix4x4 projection;
        projection.setToIdentity();
        m_gl.glViewport(0, 0, 256, 256);
        for (GLenum capability : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST,
                                  GL_SCISSOR_TEST, GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DITHER})
            m_gl.glDisable(capability);
        m_gl.glClearColor(0, 0, 0, 1);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));

        QString drawFailure;
        const auto draw = [&] {
            if (!framebuffer.bind() || !sentinelProgram.bind()) {
                drawFailure = QStringLiteral("Could not bind the framebuffer or sentinel program.");
                return false;
            }
            m_gl.glBindVertexArray(sentinelVertexArray);
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffers[0]);
            m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sentinelBuffers[1]);
            m_gl.glLineWidth(sentinelLineWidth);
            m_gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            const GlState before = State();
            warp.Draw(projection);
            const GlState after = State();
            const GLenum error = m_gl.glGetError();
            drawFailure = QStringLiteral("State before: %1; state after: %2; GL error: %3")
                .arg(before.Describe(), after.Describe()).arg(error);
            return after == before && error == GL_NO_ERROR;
        };
        QVERIFY2(draw(), qPrintable(drawFailure));
        const QImage original = framebuffer.toImage();
        QVERIFY(!original.isNull());
        const int initialForeground = foregroundCount(original);
        QVERIFY2(initialForeground > 800 && initialForeground < 2500, qPrintable(QString::number(initialForeground)));
        for (int y = 0; y < original.height(); ++y) {
            for (int x = 0; x < original.width(); ++x) {
                if (!foreground(original.pixel(x, y))) continue;
                QVERIFY2(nearAny(x, {32, 128, 224}) || nearAny(y, {32, 96, 160, 224}),
                         qPrintable(QStringLiteral("Unexpected foreground at (%1, %2)").arg(x).arg(y)));
            }
        }
        for (int centerX : {80, 176}) {
            for (int row : {32, 96, 160, 224}) QVERIFY(foregroundNear(original, centerX, row));
            for (int centerY : {64, 128, 192}) {
                for (int y = centerY - 8; y <= centerY + 8; ++y)
                    for (int x = centerX - 8; x <= centerX + 8; ++x)
                        QVERIFY(!foreground(original.pixel(x, y)));
            }
        }
        for (int column : {32, 128, 224})
            for (int centerY : {64, 128, 192}) QVERIFY(foregroundNear(original, column, centerY));

        std::vector<float> moved = positions;
        for (std::size_t index = 0; index < moved.size(); index += 2) {
            moved[index] += 0.125f;
            moved[index + 1] -= 0.125f;
        }
        QVERIFY(node.UpdateCanvasPositions(moved));
        warp.UpdatePositions(&node);
        QVERIFY2(draw(), qPrintable(drawFailure));
        const QImage updated = framebuffer.toImage();
        QVERIFY(foregroundCount(updated) > 800);
        QVERIFY(updated != original);
        for (int row : {48, 112, 176, 240}) QVERIFY(foregroundNear(updated, 96, row));
        for (int column : {48, 144, 240}) QVERIFY(foregroundNear(updated, column, 144));
        for (int row : {16, 80, 144, 208}) QVERIFY(!foregroundNear(updated, 96, row));
        for (int y = 0; y < updated.height(); ++y)
            for (int x = 0; x < updated.width(); ++x)
                if (foreground(updated.pixel(x, y)))
                    QVERIFY2(nearAny(x, {48, 144, 240}) || nearAny(y, {48, 112, 176, 240}),
                             qPrintable(QStringLiteral("Unexpected translated foreground at (%1, %2)").arg(x).arg(y)));

        QVERIFY(node.UpdateCanvasPositions({}));
        warp.UpdatePositions(&node);
        QVERIFY2(draw(), qPrintable(drawFailure));
        const QImage empty = framebuffer.toImage();
        QCOMPARE(foregroundCount(empty), 0);
        QVERIFY(node.UpdateCanvasPositions(positions));
        warp.Init(&node);
        QVERIFY(node.UpdateCanvasPositions({0, 0, 1, 1}));
        warp.UpdatePositions(&node);
        QVERIFY2(draw(), qPrintable(drawFailure));
        QCOMPARE(foregroundCount(framebuffer.toImage()), 0);
        QVERIFY(node.UpdateCanvasPositions(positions));
        warp.Init(&node);
        auto invalid = positions;
        invalid.front() = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!node.UpdateCanvasPositions(invalid));
        warp.UpdatePositions(&node);
        QVERIFY2(draw(), qPrintable(drawFailure));
        QCOMPARE(foregroundCount(framebuffer.toImage()), 0);
        QVERIFY(node.UpdateCanvasPositions(positions));
        warp.Init(&node);
        definition.columns = std::numeric_limits<int>::max();
        node.InitWarp(definition, table);
        warp.UpdatePositions(&node);
        QVERIFY2(draw(), qPrintable(drawFailure));
        QCOMPARE(foregroundCount(framebuffer.toImage()), 0);
        warp.Init(nullptr);
        QVERIFY2(draw(), qPrintable(drawFailure));
        QCOMPARE(foregroundCount(framebuffer.toImage()), 0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));

        const QString artifactDirectory = qEnvironmentVariable("LIVE2D_GL_TEST_ARTIFACT_DIR");
        if (!artifactDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDirectory));
            const QDir output(artifactDirectory);
            QVERIFY(original.save(output.filePath(QStringLiteral("warp-grid-original.png"))));
            QVERIFY(updated.save(output.filePath(QStringLiteral("warp-grid-updated.png"))));
            QVERIFY(empty.save(output.filePath(QStringLiteral("warp-grid-empty.png"))));
        }
    }

    void contextDestroyedBeforeWarpClearsQtOwner_data()
    {
        QTest::addColumn<bool>("initializeFirst");
        QTest::newRow("init-opengl") << true;
        QTest::newRow("prepare-opengl") << false;
    }

    void contextDestroyedBeforeWarpClearsQtOwner()
    {
        QFETCH(bool, initializeFirst);
        CReader reader(QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.moc3"));
        CSectionTable table(reader);
        QVERIFY(table.IsValid() && !table.warp_deformers.empty());
        CWarp_deformers definition = *table.warp_deformers.front();
        definition.columns = 1;
        definition.rows = 1;
        definition.vertex_counts = 4;
        definition.keyform_sources_counts = 1;
        CWarpNode node(QStringLiteral("owner-test"));
        node.InitWarp(definition, table);
        QVERIFY(node.UpdateCanvasPositions({-.5f, -.5f, .5f, -.5f, -.5f, .5f, .5f, .5f}));
        auto warp = std::make_unique<WarpProbe>();
        QVERIFY(warp->Init(&node));
        auto owner = std::make_unique<QOpenGLContext>();
        owner->setFormat(m_context->format());
        QVERIFY(owner->create());
        QOffscreenSurface surface;
        surface.setFormat(owner->format());
        surface.create();
        QVERIFY(surface.isValid());
        m_context->doneCurrent();
        QVERIFY(!warp->PrepareOpenGl());
        warp->InitOpenGl();
        QVERIFY(!warp->Context());
        QVERIFY(owner->makeCurrent(&surface));
        if (initializeFirst) warp->InitOpenGl();
        QVERIFY(warp->PrepareOpenGl());
        QCOMPARE(warp->Context(), owner.get());
        QVERIFY(warp->Initialized());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(!warp->PrepareOpenGl());
        warp->InitOpenGl();
        QCOMPARE(warp->Context(), owner.get());
        QVERIFY(owner->makeCurrent(&surface));
        QVERIFY(warp->PrepareOpenGl());
        owner->doneCurrent();
        owner.reset();
        QVERIFY(!warp->Context());
        QVERIFY(!warp->Initialized());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        GLuint sentinelArray = 0, sentinelBuffer = 0;
        m_gl.glGenVertexArrays(1, &sentinelArray);
        m_gl.glBindVertexArray(sentinelArray);
        m_gl.glGenBuffers(1, &sentinelBuffer);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffer);
        const auto cleanup = qScopeGuard([&] {
            m_gl.glDeleteBuffers(1, &sentinelBuffer);
            m_gl.glDeleteVertexArrays(1, &sentinelArray);
        });
        warp.reset();
        QVERIFY(m_gl.glIsVertexArray(sentinelArray));
        QVERIFY(m_gl.glIsBuffer(sentinelBuffer));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        if (m_context) m_context->doneCurrent();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    WarpRenderTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "warp_render_tests.moc"
