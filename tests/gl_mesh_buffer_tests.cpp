#include "opengl/cglmeshbuffer.h"

#include <QtTest>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<CGlMeshBuffer>);
static_assert(!std::is_copy_assignable_v<CGlMeshBuffer>);

namespace {
constexpr std::array<float, 8> Quad{-0.75f, -0.75f, 0.75f, -0.75f, 0.75f, 0.75f, -0.75f, 0.75f};
constexpr std::array<float, 8> Uvs{0, 0, 1, 0, 1, 1, 0, 1};
constexpr std::array<quint16, 6> QuadIndices{0, 1, 2, 0, 2, 3};
constexpr std::array<float, 6> Triangle{-0.8f, -0.5f, -0.4f, -0.5f, -0.6f, 0.1f};
constexpr std::array<quint16, 3> TriangleIndices{0, 1, 2};

struct State
{
    GLint vao = 0, array = 0, element = 0, program = 0, activeTexture = 0, texture = 0;
    GLfloat lineWidth = 0, pointSize = 0;
    bool operator==(const State& other) const
    {
        return vao == other.vao && array == other.array && element == other.element && program == other.program
            && activeTexture == other.activeTexture && texture == other.texture
            && lineWidth == other.lineWidth && pointSize == other.pointSize;
    }
};

struct BufferIds
{
    GLuint vao = 0, vertices = 0, indices = 0;
    bool operator==(const BufferIds& other) const
    {
        return vao == other.vao && vertices == other.vertices && indices == other.indices;
    }
};

int foregroundCount(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qRed(image.pixel(x, y)) || qGreen(image.pixel(x, y)) || qBlue(image.pixel(x, y))) ++count;
    return count;
}
}

class GlMeshBufferTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    QOpenGLFunctions_3_3_Core m_gl;
    bool m_functionsReady = false;
    GLuint m_texture = 0, m_sentinelVao = 0;
    std::array<GLuint, 2> m_sentinelBuffers{};

    State state()
    {
        State result;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &result.vao);
        m_gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &result.array);
        m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &result.element);
        m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &result.program);
        m_gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &result.activeTexture);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &result.texture);
        m_gl.glGetFloatv(GL_LINE_WIDTH, &result.lineWidth);
        m_gl.glGetFloatv(GL_POINT_SIZE, &result.pointSize);
        return result;
    }

    BufferIds ids(const CGlMeshBuffer& mesh)
    {
        GLint previous = 0, vertexBuffer = 0, indexBuffer = 0;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous);
        m_gl.glBindVertexArray(mesh.GetVertexArray());
        m_gl.glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vertexBuffer);
        m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &indexBuffer);
        m_gl.glBindVertexArray(static_cast<GLuint>(previous));
        return {mesh.GetVertexArray(), static_cast<GLuint>(vertexBuffer), static_cast<GLuint>(indexBuffer)};
    }

    GLint attribute(const CGlMeshBuffer& mesh, GLuint index, GLenum name)
    {
        GLint previous = 0, value = 0;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous);
        m_gl.glBindVertexArray(mesh.GetVertexArray());
        m_gl.glGetVertexAttribiv(index, name, &value);
        m_gl.glBindVertexArray(static_cast<GLuint>(previous));
        return value;
    }

    GLint bufferSize(GLuint buffer)
    {
        GLint previous = 0, bytes = 0;
        m_gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
        m_gl.glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bytes);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous));
        return bytes;
    }

    bool render(CGlMeshBuffer& mesh, GLenum mode, bool textured, QImage& image)
    {
        QOpenGLFramebufferObjectFormat format;
        format.setInternalTextureFormat(GL_RGBA8);
        format.setSamples(0);
        QOpenGLFramebufferObject framebuffer(64, 64, format);
        if (!framebuffer.isValid() || !framebuffer.bind() || !m_program->bind()) return false;
        m_gl.glViewport(0, 0, 64, 64);
        m_program->setUniformValue("textured", textured);
        m_program->setUniformValue("atlas", 3);
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_texture);
        const GLfloat background[] = {0, 0, 0, 1};
        m_gl.glClearBufferfv(GL_COLOR, 0, background);
        const State before = state();
        if (!mesh.Draw(mode) || !(state() == before)) return false;
        image = framebuffer.toImage();
        return !image.isNull() && m_gl.glGetError() == GL_NO_ERROR;
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
        if (actual.renderableType() != QSurfaceFormat::OpenGL || actual.profile() != QSurfaceFormat::CoreProfile
            || actual.majorVersion() < 3 || (actual.majorVersion() == 3 && actual.minorVersion() < 3))
            QSKIP("The returned context does not provide OpenGL 3.3 Core.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get()))
            QSKIP("Unable to make the offscreen surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable.");
        m_functionsReady = true;
        m_program = std::make_unique<QOpenGLShaderProgram>();
        QVERIFY2(m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 330 core\nlayout(location=0) in vec2 position; layout(location=1) in vec2 uv;\n"
            "out vec2 textureUv; void main(){textureUv=uv; gl_Position=vec4(position,0,1);}"),
            qPrintable(m_program->log()));
        QVERIFY2(m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 330 core\nin vec2 textureUv; uniform sampler2D atlas; uniform bool textured;\n"
            "out vec4 color; void main(){color=textured ? texture(atlas,textureUv) : vec4(1);}"),
            qPrintable(m_program->log()));
        QVERIFY2(m_program->link(), qPrintable(m_program->log()));
        m_gl.glGenTextures(1, &m_texture);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_texture);
        const QByteArray pixels = QByteArray::fromHex("ff0000ff00ff00ff0000ffffffff00ff");
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.constData());
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_gl.glGenVertexArrays(1, &m_sentinelVao);
        m_gl.glGenBuffers(2, m_sentinelBuffers.data());
        m_gl.glBindVertexArray(m_sentinelVao);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_sentinelBuffers[0]);
        m_gl.glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_sentinelBuffers[1]);
        m_gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        qInfo() << "Mesh buffer GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        m_gl.glBindVertexArray(m_sentinelVao);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_sentinelBuffers[0]);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_sentinelBuffers[1]);
        QVERIFY(m_program->bind());
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_texture);
        m_gl.glLineWidth(1);
        m_gl.glPointSize(3);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (GLenum capability : {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                                  GL_CULL_FACE, GL_DITHER, GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB,
                                  GL_PROGRAM_POINT_SIZE}) m_gl.glDisable(capability);
    }

    void cleanup()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QCOMPARE(state().vao, GLint(m_sentinelVao));
        QCOMPARE(state().array, GLint(m_sentinelBuffers[0]));
        QCOMPARE(state().element, GLint(m_sentinelBuffers[1]));
    }

    void cleanupTestCase()
    {
        if (!m_functionsReady || !m_context || !m_surface || !m_context->makeCurrent(m_surface.get())) return;
        m_program.reset();
        m_gl.glDeleteTextures(1, &m_texture);
        m_gl.glDeleteBuffers(2, m_sentinelBuffers.data());
        m_gl.glDeleteVertexArrays(1, &m_sentinelVao);
    }

    void unsignedHighIndicesAndLargeIndexCountReachGpuWithoutTruncation()
    {
        QVector<float> positions(65536 * 2, 0);
        const quint16 corners[]{32768, 65534, 65535};
        const float triangle[]{-.75f, -.75f, .75f, -.75f, 0, .75f};
        for (int index = 0; index < 3; ++index) {
            positions[corners[index] * 2] = triangle[index * 2];
            positions[corners[index] * 2 + 1] = triangle[index * 2 + 1];
        }
        QVector<quint16> indices;
        for (int triangle = 0; triangle < 21846; ++triangle) indices.append({32768, 65534, 65535});
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        const auto before = state();
        QVERIFY(mesh.Upload(positions.constData(), 65536, nullptr, indices.constData(), indices.size()));
        QVERIFY(state() == before);
        const auto gpu = ids(mesh);
        QCOMPARE(bufferSize(gpu.indices), GLint(indices.size() * sizeof(quint16)));
        QVector<quint16> readback(indices.size(), 0);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, gpu.indices);
        m_gl.glGetBufferSubData(GL_ARRAY_BUFFER, 0, readback.size() * sizeof(quint16), readback.data());
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, GLuint(before.array));
        QCOMPARE(readback, indices);
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image.pixelColor(32, 32), QColor(Qt::white));
        QVERIFY(foregroundCount(image) > 900);
        QVERIFY(!mesh.Upload(positions.constData(), 65535, nullptr, indices.constData(), indices.size()));
        QVERIFY(ids(mesh) == gpu);
        QImage preserved; QVERIFY(render(mesh, GL_TRIANGLES, false, preserved)); QCOMPARE(preserved, image);
        const quint16 line[]{65534, 65535};
        QVERIFY(mesh.Upload(positions.constData(), 65536, nullptr, line, 2));
        QVERIFY(render(mesh, GL_LINES, false, image));
        QVERIFY(foregroundCount(image) > 30);
    }

    void texturedGeometryUsesBothAttributesAndPreservesCallerState()
    {
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        const State before = state();
        QVERIFY(mesh.Upload(Quad.data(), 4, Uvs.data(), QuadIndices.data(), 6));
        QVERIFY(state() == before);
        QCOMPARE(attribute(mesh, 0, GL_VERTEX_ATTRIB_ARRAY_ENABLED), GLint(GL_TRUE));
        QCOMPARE(attribute(mesh, 1, GL_VERTEX_ATTRIB_ARRAY_ENABLED), GLint(GL_TRUE));
        QCOMPARE(attribute(mesh, 0, GL_VERTEX_ATTRIB_ARRAY_SIZE), 2);
        QCOMPARE(attribute(mesh, 1, GL_VERTEX_ATTRIB_ARRAY_SIZE), 2);
        QCOMPARE(attribute(mesh, 0, GL_VERTEX_ATTRIB_ARRAY_STRIDE), GLint(4 * sizeof(float)));
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, true, image));
        QCOMPARE(image.pixelColor(16, 48), QColor(Qt::red));
        QCOMPARE(image.pixelColor(48, 48), QColor(Qt::green));
        QCOMPARE(image.pixelColor(16, 16), QColor(Qt::blue));
        QCOMPARE(image.pixelColor(48, 16), QColor(Qt::yellow));
        const BufferIds original = ids(mesh);
        for (int i = 0; i < 5; ++i) {
            QVERIFY(mesh.Upload(Quad.data(), 4, Uvs.data(), QuadIndices.data(), 6));
            QVERIFY(ids(mesh) == original);
        }
        auto flippedUvs = Uvs;
        for (size_t i = 0; i < flippedUvs.size(); i += 2) flippedUvs[i] = 1 - flippedUvs[i];
        QVERIFY(mesh.Upload(Quad.data(), 4, flippedUvs.data(), QuadIndices.data(), 6));
        QVERIFY(render(mesh, GL_TRIANGLES, true, image));
        QCOMPARE(image.pixelColor(16, 48), QColor(Qt::green));
        QCOMPARE(image.pixelColor(48, 48), QColor(Qt::red));
        auto shifted = Quad;
        shifted[0] += 0.05f;
        QVERIFY(mesh.Upload(shifted.data(), 4, Uvs.data(), QuadIndices.data(), 6));
        QVERIFY(mesh.Upload(Quad.data(), 4, nullptr, QuadIndices.data(), 6));
        QCOMPARE(attribute(mesh, 1, GL_VERTEX_ATTRIB_ARRAY_ENABLED), GLint(GL_FALSE));
        QCOMPARE(attribute(mesh, 0, GL_VERTEX_ATTRIB_ARRAY_STRIDE), GLint(2 * sizeof(float)));
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image.pixelColor(16, 16), QColor(Qt::white));
    }

    void positionUpdatesReuseExactlyTwoCompleteBufferGroups()
    {
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        std::map<GLuint, BufferIds> observed;
        QImage previous;
        for (int iteration = 0; iteration < 7; ++iteration) {
            auto moved = Triangle;
            for (size_t i = 0; i < moved.size(); i += 2) moved[i] += float(iteration) * 0.15f;
            const State before = state();
            QVERIFY(mesh.Upload(moved.data(), 3, nullptr, TriangleIndices.data(), 3));
            QVERIFY(state() == before);
            const BufferIds current = ids(mesh);
            QVERIFY(current.vao && current.vertices && current.indices);
            const auto found = observed.find(current.vao);
            if (found == observed.end()) observed.emplace(current.vao, current);
            else QVERIFY(found->second == current);
            QCOMPARE(observed.size(), size_t(iteration == 0 ? 1 : 2));
            for (const auto& entry : observed) {
                QVERIFY(m_gl.glIsVertexArray(entry.second.vao));
                QVERIFY(m_gl.glIsBuffer(entry.second.vertices));
                QVERIFY(m_gl.glIsBuffer(entry.second.indices));
            }
            QImage image;
            QVERIFY(render(mesh, GL_TRIANGLES, false, image));
            QVERIFY(foregroundCount(image) > 70);
            if (!previous.isNull()) QVERIFY(image != previous);
            previous = image;
        }
        QVERIFY(mesh.Upload(Quad.data(), 4, Uvs.data(), QuadIndices.data(), 6));
        const BufferIds grown = ids(mesh);
        QVERIFY(observed.at(grown.vao) == grown);
        QCOMPARE(bufferSize(grown.vertices), GLint(16 * sizeof(float)));
        QCOMPARE(bufferSize(grown.indices), GLint(6 * sizeof(quint16)));
        QVERIFY(mesh.Upload(Quad.data(), 4, nullptr, QuadIndices.data(), 6));
        const BufferIds other = ids(mesh);
        QVERIFY(observed.at(other.vao) == other);
        QVERIFY(other.vao != grown.vao);
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        QVERIFY(ids(mesh) == grown);
        QCOMPARE(bufferSize(grown.vertices), GLint(16 * sizeof(float)));
        QCOMPARE(bufferSize(grown.indices), GLint(6 * sizeof(quint16)));
        QCOMPARE(attribute(mesh, 1, GL_VERTEX_ATTRIB_ARRAY_ENABLED), GLint(GL_FALSE));
        QVERIFY(mesh.Clear());
        for (const auto& entry : observed) {
            QVERIFY(!m_gl.glIsVertexArray(entry.second.vao));
            QVERIFY(!m_gl.glIsBuffer(entry.second.vertices));
            QVERIFY(!m_gl.glIsBuffer(entry.second.indices));
        }
    }

    void topologyAndPrimitiveChangesProduceTheExpectedPixels()
    {
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        const std::array<quint16, 3> lower{0, 1, 2}, upper{0, 2, 3};
        QVERIFY(mesh.Upload(Quad.data(), 4, nullptr, lower.data(), 3));
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image.pixelColor(16, 16), QColor(Qt::black));
        QCOMPARE(image.pixelColor(48, 48), QColor(Qt::white));
        QVERIFY(mesh.Upload(Quad.data(), 4, nullptr, upper.data(), 3));
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image.pixelColor(16, 16), QColor(Qt::white));
        QCOMPARE(image.pixelColor(48, 48), QColor(Qt::black));
        const std::array<quint16, 4> perimeter{0, 1, 2, 3};
        QVERIFY(mesh.Upload(Quad.data(), 4, nullptr, perimeter.data(), 4));
        QVERIFY(render(mesh, GL_LINE_LOOP, false, image));
        const int lines = foregroundCount(image);
        QVERIFY(lines > 100 && lines < 300);
        QCOMPARE(image.pixelColor(32, 32), QColor(Qt::black));
        QVERIFY(render(mesh, GL_POINTS, false, image));
        const int points = foregroundCount(image);
        QVERIFY(points >= 4 && points <= 64);
        QCOMPARE(image.pixelColor(32, 32), QColor(Qt::black));
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        QImage indexed;
        QVERIFY(render(mesh, GL_TRIANGLES, false, indexed));
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, nullptr, 0));
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image, indexed);
    }

    void invalidInputAndPriorGlErrorsKeepTheLastGeometry()
    {
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        const BufferIds original = ids(mesh);
        QImage originalImage;
        QVERIFY(render(mesh, GL_TRIANGLES, false, originalImage));
        auto badPosition = Triangle;
        badPosition[2] = std::numeric_limits<float>::quiet_NaN();
        std::array<float, 6> badUvs{};
        badUvs[3] = std::numeric_limits<float>::infinity();
        const std::array<quint16, 3> badIndices{0, 1, 3};
        const std::vector<std::function<bool()>> invalid{
            [&] { return mesh.Upload(nullptr, 3, nullptr, TriangleIndices.data(), 3); },
            [&] { return mesh.Upload(Triangle.data(), -1, nullptr, nullptr, 0); },
            [&] { return mesh.Upload(Triangle.data(), std::numeric_limits<int>::max(), nullptr, nullptr, 0); },
            [&] { return mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), -1); },
            [&] { return mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), std::numeric_limits<int>::max()); },
            [&] { return mesh.Upload(badPosition.data(), 3, nullptr, nullptr, 0); },
            [&] { return mesh.Upload(Triangle.data(), 3, badUvs.data(), nullptr, 0); },
            [&] { return mesh.Upload(Triangle.data(), 3, nullptr, badIndices.data(), 3); },
            [&] { return mesh.Upload(Triangle.data(), 3, nullptr, nullptr, 3); },
            [&] { return mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 0); },
            [&] { return mesh.Upload(nullptr, 0, badUvs.data(), nullptr, 0); },
            [&] { return mesh.Upload(nullptr, 0, nullptr, TriangleIndices.data(), 3); }
        };
        for (const auto& operation : invalid) {
            const State before = state();
            QVERIFY(!operation());
            QVERIFY(state() == before);
            QVERIFY(ids(mesh) == original);
            QImage image;
            QVERIFY(render(mesh, GL_TRIANGLES, false, image));
            QCOMPARE(image, originalImage);
        }
        const State before = state();
        QVERIFY(!mesh.Draw(GLenum(0xffff)));
        QVERIFY(state() == before);
        m_gl.glEnable(GLenum(0xffff));
        QVERIFY(!mesh.Upload(Quad.data(), 4, nullptr, QuadIndices.data(), 6));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(ids(mesh) == original);
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image, originalImage);
    }

    void partiallyWrittenStagingBuffersNeverReplaceValidGeometry()
    {
        CGlMeshBuffer mesh;
        QVERIFY(mesh.SetContext(m_context.get()));
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        const BufferIds staging = ids(mesh);
        auto moved = Triangle;
        for (size_t i = 0; i < moved.size(); i += 2) moved[i] += 0.3f;
        QVERIFY(mesh.Upload(moved.data(), 3, nullptr, TriangleIndices.data(), 3));
        const BufferIds active = ids(mesh);
        QImage originalImage;
        QVERIFY(render(mesh, GL_TRIANGLES, false, originalImage));
        m_gl.glBindVertexArray(staging.vao);
        void* mapped = m_gl.glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
        m_gl.glBindVertexArray(m_sentinelVao);
        QVERIFY(mapped);
        bool isMapped = true;
        const auto unmap = qScopeGuard([&] {
            if (!isMapped) return;
            m_gl.glBindVertexArray(staging.vao);
            m_gl.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            m_gl.glBindVertexArray(m_sentinelVao);
        });
        for (size_t i = 0; i < moved.size(); i += 2) moved[i] += 0.3f;
        const State before = state();
        QVERIFY(!mesh.Upload(moved.data(), 3, nullptr, TriangleIndices.data(), 3));
        QVERIFY(state() == before);
        QVERIFY(ids(mesh) == active);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QCOMPARE(image, originalImage);
        m_gl.glBindVertexArray(staging.vao);
        QVERIFY(m_gl.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER));
        isMapped = false;
        m_gl.glBindVertexArray(m_sentinelVao);
        QVERIFY(mesh.Upload(moved.data(), 3, nullptr, TriangleIndices.data(), 3));
        QVERIFY(ids(mesh) == staging);
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QVERIFY(image != originalImage);
    }

    void emptyUploadAndDestructionDeleteBothOwnedGroups()
    {
        std::array<BufferIds, 2> owned;
        {
            CGlMeshBuffer mesh;
            QVERIFY(mesh.SetContext(m_context.get()));
            QVERIFY(mesh.Draw(GL_TRIANGLES));
            QVERIFY(mesh.Upload(Quad.data(), 4, Uvs.data(), QuadIndices.data(), 6));
            owned[0] = ids(mesh);
            QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
            owned[1] = ids(mesh);
            const State before = state();
            QVERIFY(mesh.Upload(nullptr, 0, nullptr, nullptr, 0));
            QVERIFY(state() == before);
            QCOMPARE(mesh.GetVertexArray(), GLuint(0));
            QImage image;
            QVERIFY(render(mesh, GL_TRIANGLES, false, image));
            QCOMPARE(foregroundCount(image), 0);
            for (const BufferIds& buffer : owned) {
                QVERIFY(!m_gl.glIsVertexArray(buffer.vao));
                QVERIFY(!m_gl.glIsBuffer(buffer.vertices));
                QVERIFY(!m_gl.glIsBuffer(buffer.indices));
            }
            QVERIFY(mesh.Upload(Quad.data(), 4, Uvs.data(), QuadIndices.data(), 6));
            owned[0] = ids(mesh);
            QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
            owned[1] = ids(mesh);
        }
        for (const BufferIds& buffer : owned) {
            QVERIFY(!m_gl.glIsVertexArray(buffer.vao));
            QVERIFY(!m_gl.glIsBuffer(buffer.vertices));
            QVERIFY(!m_gl.glIsBuffer(buffer.indices));
        }
    }

    void absentAndForeignContextsCannotUseOrResetGeometry()
    {
        CGlMeshBuffer mesh;
        QVERIFY(!mesh.SetContext(nullptr));
        QVERIFY(!mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        QVERIFY(mesh.SetContext(m_context.get()));
        QVERIFY(mesh.Upload(Triangle.data(), 3, nullptr, TriangleIndices.data(), 3));
        const BufferIds original = ids(mesh);
        m_context->doneCurrent();
        const auto restore = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        QVERIFY(!mesh.SetContext(m_context.get()));
        QVERIFY(!mesh.Upload(Quad.data(), 4, nullptr, QuadIndices.data(), 6));
        QVERIFY(!mesh.Draw(GL_TRIANGLES));
        QVERIFY(!mesh.Clear());
        auto foreign = std::make_unique<QOpenGLContext>();
        foreign->setFormat(m_context->format());
        if (!foreign->create() || !foreign->makeCurrent(m_surface.get()))
            QSKIP("Unable to create a second real context for ownership checks.");
        QVERIFY(!mesh.SetContext(foreign.get()));
        QVERIFY(!mesh.Upload(Quad.data(), 4, nullptr, QuadIndices.data(), 6));
        QVERIFY(!mesh.Draw(GL_TRIANGLES));
        QVERIFY(!mesh.Clear());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(ids(mesh) == original);
        QImage image;
        QVERIFY(render(mesh, GL_TRIANGLES, false, image));
        QVERIFY(foregroundCount(image) > 70);
        QVERIFY(mesh.Clear());
        QVERIFY(!m_gl.glIsVertexArray(original.vao));
        QVERIFY(!m_gl.glIsBuffer(original.vertices));
        QVERIFY(!m_gl.glIsBuffer(original.indices));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    GlMeshBufferTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "gl_mesh_buffer_tests.moc"
