#include "opengl/ccubismshaderprograms.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <set>

using Programs = CCubismShaderPrograms;
using Variant = Programs::Variant;
using BlendMode = Programs::BlendMode;
using Color = std::array<float, 4>;

namespace {
constexpr Color sourceColor{0.5f, 0.25f, 0.125f, 0.5f};
constexpr Color baseColor{0.75f, 0.5f, 1.0f, 0.5f};
constexpr Color multiplyColor{0.5f, 0.75f, 1.0f, 1.0f};
constexpr Color screenColor{0.25f, 0.125f, 0.0625f, 0.0f};
constexpr Color maskColor{0.25f, 0.5f, 0.75f, 1.0f};
const Color background{32.0f / 255, 64.0f / 255, 96.0f / 255, 128.0f / 255};
constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr std::array<float, 16> textureMatrix{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};

Variant drawVariant(bool premultiplied, int mask)
{
    return static_cast<Variant>(1 + (premultiplied ? 3 : 0) + mask);
}

Color premultiply(Color color)
{
    for (int channel = 0; channel < 3; ++channel) color[channel] *= color[3];
    return color;
}

Color referenceColor(float coverage, BlendMode mode)
{
    Color result{};
    const float alpha = sourceColor[3] * baseColor[3] * coverage;
    for (int channel = 0; channel < 3; ++channel) {
        const double multiplied = double(sourceColor[channel]) * multiplyColor[channel];
        const double screened = multiplied + screenColor[channel] - multiplied * screenColor[channel];
        const double source = screened * baseColor[channel] * alpha;
        const double destination = background[channel];
        if (mode == BlendMode::Normal) result[channel] = source + destination * (1 - alpha);
        if (mode == BlendMode::Additive) result[channel] = source + destination;
        if (mode == BlendMode::Multiplicative) result[channel] = source * destination + destination * (1 - alpha);
    }
    result[3] = mode == BlendMode::Normal ? alpha + background[3] * (1 - alpha) : background[3];
    return result;
}

bool matches(const std::array<uchar, 4>& pixel, const Color& expected, int tolerance = 1)
{
    for (int channel = 0; channel < 4; ++channel) {
        const int value = qRound(std::clamp(double(expected[channel]), 0.0, 1.0) * 255);
        if (std::abs(int(pixel[channel]) - value) > tolerance) return false;
    }
    return true;
}
}

class CubismShaderTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<Programs> m_programs;
    QOpenGLFunctions_3_3_Core m_gl;
    GLuint m_vertexArray = 0, m_vertexBuffer = 0;

    GLuint texture(const Color* data, int width = 1, int height = 1)
    {
        GLuint id = 0;
        m_gl.glGenTextures(1, &id);
        m_gl.glBindTexture(GL_TEXTURE_2D, id);
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, data);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return id;
    }

    void uniform(GLint location, const Color& color) { m_gl.glUniform4fv(location, 1, color.data()); }

    bool prepare(QOpenGLFramebufferObject& target, const Color& color)
    {
        if (!target.isValid() || !target.bind()) return false;
        m_gl.glViewport(0, 0, 16, 16);
        for (GLenum capability : {GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                                  GL_FRAMEBUFFER_SRGB, GL_DITHER, GL_MULTISAMPLE}) m_gl.glDisable(capability);
        m_gl.glEnable(GL_BLEND);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.glDrawBuffer(GL_COLOR_ATTACHMENT0);
        m_gl.glClearBufferfv(GL_COLOR, 0, color.data());
        m_gl.glBindVertexArray(m_vertexArray);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    std::array<uchar, 4> pixel(int x = 8, int y = 8)
    {
        std::array<uchar, 4> value{};
        m_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        m_gl.glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value.data());
        return value;
    }

    bool draw(Variant variant, BlendMode mode, int maskChannel, GLuint source, GLuint mask,
              const Color& base = baseColor, const Color& multiply = multiplyColor, const Color& screen = screenColor)
    {
        if (!m_programs->Bind(variant, mode)) return false;
        const auto* program = m_programs->Get(variant);
        if (!program) return false;
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, source);
        m_gl.glUniform1i(program->sampler0, 0);
        m_gl.glActiveTexture(GL_TEXTURE1);
        m_gl.glBindTexture(GL_TEXTURE_2D, mask);
        m_gl.glUniform1i(program->sampler1, 1);
        m_gl.glUniformMatrix4fv(program->matrix, 1, GL_FALSE, identity.data());
        m_gl.glUniformMatrix4fv(program->clipMatrix, 1, GL_FALSE, textureMatrix.data());
        uniform(program->baseColor, base);
        uniform(program->multiplyColor, multiply);
        uniform(program->screenColor, screen);
        Color channel{};
        channel[maskChannel] = 1;
        uniform(program->channelFlag, channel);
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glDrawArrays(GL_TRIANGLES, 0, 6);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    std::set<GLuint> ids(Programs& programs)
    {
        std::set<GLuint> values;
        for (std::size_t index = 0; index < Programs::ProgramCount; ++index) {
            const auto* program = programs.Get(static_cast<Variant>(index));
            if (program) values.insert(program->id);
        }
        return values;
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
        if (actual.profile() != QSurfaceFormat::CoreProfile || actual.majorVersion() < 3 ||
            (actual.majorVersion() == 3 && actual.minorVersion() < 3)) QSKIP("OpenGL 3.3 Core is unavailable.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get())) QSKIP("Unable to make the surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable.");
        m_programs = std::make_unique<Programs>();
        QVERIFY2(m_programs->Initialize(m_context.get()), qPrintable(m_programs->Error()));
        constexpr std::array<float, 24> vertices{
            -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1,
            -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1};
        m_gl.glGenVertexArrays(1, &m_vertexArray);
        m_gl.glGenBuffers(1, &m_vertexBuffer);
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
        m_gl.glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
        m_gl.glEnableVertexAttribArray(0);
        m_gl.glEnableVertexAttribArray(1);
        m_gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        m_gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        qInfo() << "Cubism shader GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        m_gl.glUseProgram(0);
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        m_gl.glActiveTexture(GL_TEXTURE0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanup()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        m_gl.glUseProgram(0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        if (!m_context || !m_surface || !m_context->makeCurrent(m_surface.get())) return;
        m_gl.glUseProgram(0);
        m_programs.reset();
        if (m_vertexBuffer) m_gl.glDeleteBuffers(1, &m_vertexBuffer);
        if (m_vertexArray) m_gl.glDeleteVertexArrays(1, &m_vertexArray);
        m_context->doneCurrent();
    }

    void sevenProgramsCompileLinkAndShareAcrossBlendModes()
    {
        QCOMPARE(ids(*m_programs).size(), Programs::ProgramCount);
        for (std::size_t index = 0; index < Programs::ProgramCount; ++index) {
            const auto variant = static_cast<Variant>(index);
            const auto* program = m_programs->Get(variant);
            QVERIFY(program && program->id);
            GLint linked = GL_FALSE;
            m_gl.glGetProgramiv(program->id, GL_LINK_STATUS, &linked);
            QCOMPARE(linked, GLint(GL_TRUE));
            QCOMPARE(m_gl.glGetAttribLocation(program->id, "a_position"), 0);
            QCOMPARE(m_gl.glGetAttribLocation(program->id, "a_texCoord"), 1);
            QCOMPARE(m_gl.glGetFragDataLocation(program->id, "fragColor"), 0);
            for (const auto blend : {BlendMode::Normal, BlendMode::Additive, BlendMode::Multiplicative}) {
                QVERIFY(m_programs->Bind(variant, blend));
                GLint current = 0;
                m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &current);
                QCOMPARE(GLuint(current), program->id);
            }
        }
        const auto before = ids(*m_programs);
        QVERIFY(m_programs->Initialize(m_context.get()));
        QVERIFY(ids(*m_programs) == before);
        GLint current = 0;
        m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        QVERIFY(!m_programs->Get(Variant::Count));
        QVERIFY(!m_programs->Bind(Variant::Count));
        QVERIFY(!m_programs->Bind(Variant::Straight, static_cast<BlendMode>(99)));
        GLint after = 0;
        m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &after);
        QCOMPARE(after, current);
    }

    void colorsMasksAndBlendingMatchIndependentReference_data()
    {
        QTest::addColumn<bool>("premultiplied");
        QTest::addColumn<int>("mask");
        QTest::addColumn<int>("channel");
        QTest::addColumn<int>("blend");
        for (bool premultiplied : {false, true})
            for (int mask = 0; mask < 3; ++mask)
                for (int channel = 0; channel < (mask ? 4 : 1); ++channel)
                    for (int blend = 0; blend < 3; ++blend)
                        QTest::newRow(qPrintable(QString("alpha%1-mask%2-channel%3-blend%4")
                            .arg(premultiplied).arg(mask).arg(channel).arg(blend)))
                            << premultiplied << mask << channel << blend;
    }

    void colorsMasksAndBlendingMatchIndependentReference()
    {
        QFETCH(bool, premultiplied);
        QFETCH(int, mask);
        QFETCH(int, channel);
        QFETCH(int, blend);
        QOpenGLFramebufferObject target(16, 16);
        QVERIFY(prepare(target, background));
        const Color source = premultiplied ? premultiply(sourceColor) : sourceColor;
        const Color base = premultiplied ? premultiply(baseColor) : baseColor;
        GLuint sourceTexture = texture(&source), maskTexture = texture(&maskColor);
        const auto release = qScopeGuard([&] {
            m_gl.glDeleteTextures(1, &sourceTexture);
            m_gl.glDeleteTextures(1, &maskTexture);
        });
        m_gl.glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_SUBTRACT);
        const auto mode = static_cast<BlendMode>(blend);
        QVERIFY(draw(drawVariant(premultiplied, mask), mode, channel, sourceTexture, maskTexture, base));
        const float coverage = mask == 0 ? 1.0f : (mask == 1 ? 1.0f - maskColor[channel] : maskColor[channel]);
        const auto actual = pixel();
        QVERIFY2(matches(actual, referenceColor(coverage, mode)),
                 qPrintable(QString("actual RGBA %1,%2,%3,%4").arg(actual[0]).arg(actual[1]).arg(actual[2]).arg(actual[3])));
    }

    void straightAndPremultipliedInputsGiveEquivalentPixels()
    {
        QOpenGLFramebufferObject target(16, 16);
        const Color premultiplied = premultiply(sourceColor);
        GLuint straightTexture = texture(&sourceColor), premultipliedTexture = texture(&premultiplied), maskTexture = texture(&maskColor);
        const auto release = qScopeGuard([&] {
            m_gl.glDeleteTextures(1, &straightTexture);
            m_gl.glDeleteTextures(1, &premultipliedTexture);
            m_gl.glDeleteTextures(1, &maskTexture);
        });
        for (int mask = 0; mask < 3; ++mask) {
            QVERIFY(prepare(target, background));
            QVERIFY(draw(drawVariant(false, mask), BlendMode::Normal, 0, straightTexture, maskTexture));
            const auto straight = pixel();
            QVERIFY(prepare(target, background));
            QVERIFY(draw(drawVariant(true, mask), BlendMode::Normal, 0, premultipliedTexture, maskTexture, premultiply(baseColor)));
            QVERIFY(pixel() == straight);
        }
    }

    void textureCoordinatesKeepTheirVerticalFlip()
    {
        const std::array<Color, 4> data{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}}};
        GLuint source = texture(data.data(), 2, 2);
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(1, &source); });
        QOpenGLFramebufferObject target(16, 16);
        QVERIFY(prepare(target, Color{}));
        QVERIFY(draw(Variant::Straight, BlendMode::Normal, 0, source, 0, Color{1, 1, 1, 1}, Color{1, 1, 1, 1}, Color{}));
        QVERIFY(matches(pixel(2, 13), Color{1, 0, 0, 1}, 0));
        QVERIFY(matches(pixel(13, 13), Color{0, 1, 0, 1}, 0));
        QVERIFY(matches(pixel(2, 2), Color{0, 0, 1, 1}, 0));
        QVERIFY(matches(pixel(13, 2), Color{1, 1, 0, 1}, 0));
    }

    void maskGenerationAccumulatesOnlyTheSelectedChannel_data()
    {
        QTest::addColumn<int>("channel");
        for (int channel = 0; channel < 4; ++channel) QTest::newRow(qPrintable(QString::number(channel))) << channel;
    }

    void maskGenerationAccumulatesOnlyTheSelectedChannel()
    {
        QFETCH(int, channel);
        GLuint source = texture(&sourceColor);
        const auto release = qScopeGuard([&] { m_gl.glDeleteTextures(1, &source); });
        QOpenGLFramebufferObject target(16, 16);
        QVERIFY(prepare(target, Color{1, 1, 1, 1}));
        QVERIFY(m_programs->Bind(Variant::SetupMask));
        const auto* program = m_programs->Get(Variant::SetupMask);
        QVERIFY(program);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, source);
        m_gl.glUniform1i(program->sampler0, 0);
        m_gl.glUniformMatrix4fv(program->clipMatrix, 1, GL_FALSE, identity.data());
        uniform(program->baseColor, Color{-0.5f, -0.5f, 0.5f, 0.5f});
        Color flag{};
        flag[channel] = 1;
        uniform(program->channelFlag, flag);
        m_gl.glDrawArrays(GL_TRIANGLES, 0, 6);
        Color expected{1, 1, 1, 1};
        expected[channel] = 0.5f;
        QVERIFY(matches(pixel(), expected));
        QVERIFY(matches(pixel(1, 1), Color{1, 1, 1, 1}, 0));
        m_gl.glDrawArrays(GL_TRIANGLES, 0, 6);
        expected[channel] = 0.25f;
        QVERIFY(matches(pixel(), expected));

        QOpenGLFramebufferObject drawTarget(16, 16);
        QVERIFY(prepare(drawTarget, Color{}));
        QVERIFY(draw(Variant::StraightMasked, BlendMode::Normal, channel, source, target.texture(),
                     Color{1, 1, 1, 1}, Color{1, 1, 1, 1}, Color{}));
        QCOMPARE(pixel(1, 1)[3], uchar(0));
        const int normalAlpha = pixel()[3];
        QVERIFY(normalAlpha > 90 && normalAlpha < 100);
        QVERIFY(prepare(drawTarget, Color{}));
        QVERIFY(draw(Variant::StraightInverted, BlendMode::Normal, channel, source, target.texture(),
                     Color{1, 1, 1, 1}, Color{1, 1, 1, 1}, Color{}));
        QCOMPARE(pixel(1, 1)[3], uchar(128));
        const int invertedAlpha = pixel()[3];
        QVERIFY(invertedAlpha >= 31 && invertedAlpha <= 33);
        QVERIFY(std::abs(normalAlpha + invertedAlpha - 128) <= 1);
    }

    void compilerAndLinkerFailuresDiscardCandidatesAndPreserveOldPrograms()
    {
        Programs programs;
        auto sources = Programs::Sources();
        sources[0].vertex = "#version 330 core\nthis is not a shader";
        QVERIFY(!programs.Initialize(m_context.get(), sources));
        QVERIFY(!programs.Error().isEmpty());
        QVERIFY(ids(programs).empty());
        QVERIFY2(programs.Initialize(m_context.get()), qPrintable(programs.Error()));
        const auto original = ids(programs);
        QCOMPARE(original.size(), Programs::ProgramCount);
        for (int failure = 0; failure < 3; ++failure) {
            sources = Programs::Sources();
            if (failure == 0) sources[6].fragment = "#version 330 core\nnot valid";
            if (failure == 1) {
                sources[3].vertex.replace("out vec2 v_texCoord;", "out vec3 v_texCoord;");
                sources[3].vertex.replace("v_texCoord=a_texCoord;", "v_texCoord=vec3(a_texCoord,0.0);");
            }
            if (failure == 2) sources[5].fragment.clear();
            QVERIFY(!programs.Initialize(m_context.get(), sources));
            QVERIFY(!programs.Error().isEmpty());
            QVERIFY(ids(programs) == original);
            for (GLuint id : original) QVERIFY(m_gl.glIsProgram(id));
            QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        }
        QVERIFY(programs.Release());
        for (GLuint id : original) QVERIFY(!m_gl.glIsProgram(id));
        QVERIFY(programs.Release());
        QVERIFY(!programs.Get(Variant::Straight));
        QVERIFY(programs.Initialize(m_context.get()));
        QCOMPARE(ids(programs).size(), Programs::ProgramCount);
    }

    void wrongOrMissingContextCannotReplaceOrDeletePrograms()
    {
        const auto original = ids(*m_programs);
        QOpenGLContext other;
        other.setFormat(m_context->format());
        QVERIFY(other.create());
        QOffscreenSurface surface;
        surface.setFormat(other.format());
        surface.create();
        QVERIFY(surface.isValid());
        const auto restore = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        m_context->doneCurrent();
        QVERIFY(!m_programs->Initialize(m_context.get()));
        QVERIFY(!m_programs->Get(Variant::Straight));
        QVERIFY(!m_programs->Bind(Variant::Straight));
        QVERIFY(!m_programs->Release());
        QVERIFY(other.makeCurrent(&surface));
        QOpenGLFunctions_3_3_Core gl;
        QVERIFY(gl.initializeOpenGLFunctions());
        QOpenGLShaderProgram sentinel;
        QVERIFY(sentinel.addShaderFromSourceCode(QOpenGLShader::Vertex, Programs::Sources()[1].vertex));
        QVERIFY(sentinel.addShaderFromSourceCode(QOpenGLShader::Fragment, Programs::Sources()[1].fragment));
        QVERIFY(sentinel.link());
        const GLuint sentinelId = sentinel.programId();
        QVERIFY(!m_programs->Initialize(&other));
        QVERIFY(!m_programs->Initialize(m_context.get()));
        QVERIFY(!m_programs->Get(Variant::Straight));
        QVERIFY(!m_programs->Bind(Variant::Straight));
        QVERIFY(!m_programs->Release());
        QVERIFY(gl.glIsProgram(sentinelId));
        QCOMPARE(gl.glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(ids(*m_programs) == original);
        for (GLuint id : original) QVERIFY(m_gl.glIsProgram(id));
        QVERIFY(m_programs->Bind(Variant::Straight));
    }

    void destroyedOwnerCanBeReplacedWithoutTouchingCurrentPrograms()
    {
        Programs programs;
        const auto original = ids(*m_programs);
        {
            QOpenGLContext owner;
            owner.setFormat(m_context->format());
            QVERIFY(owner.create());
            QOffscreenSurface surface;
            surface.setFormat(owner.format());
            surface.create();
            QVERIFY(surface.isValid());
            QVERIFY(owner.makeCurrent(&surface));
            QVERIFY2(programs.Initialize(&owner), qPrintable(programs.Error()));
            QCOMPARE(ids(programs).size(), Programs::ProgramCount);
            owner.doneCurrent();
        }
        QVERIFY(!programs.Context());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY2(programs.Initialize(m_context.get()), qPrintable(programs.Error()));
        QCOMPARE(ids(programs).size(), Programs::ProgramCount);
        for (GLuint id : original) QVERIFY(m_gl.glIsProgram(id));
        QVERIFY(programs.Release());
        for (GLuint id : original) QVERIFY(m_gl.glIsProgram(id));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    CubismShaderTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "cubism_shader_tests.moc"
