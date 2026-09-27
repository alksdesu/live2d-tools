#include "opengl/cglrenderstate.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <array>
#include <memory>
#include <vector>

namespace {
constexpr std::array<GLenum, 4> Capabilities{GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE};
constexpr std::array<GLenum, 3> TextureUnits{GL_TEXTURE0, GL_TEXTURE1, GL_TEXTURE3};

struct State
{
    GLint vao = 0, array = 0, program = 0, activeTexture = 0;
    std::array<GLint, 3> textures{};
    std::array<GLint, 3> samplers{};
    std::array<GLboolean, 4> capabilities{};
    std::vector<GLboolean> blendEnabled;
    std::vector<std::array<GLboolean, 4>> colorMasks;
    GLint frontFace = 0, cullFace = 0;
    std::array<GLint, 4> blendFactors{};
    std::array<GLint, 2> blendEquations{};
    GLint readFramebuffer = 0, drawFramebuffer = 0, readBuffer = 0;
    std::array<GLint, 4> viewport{};
    std::array<GLfloat, 4> clearColor{};

    QString Difference(const State& other) const
    {
        QStringList changed;
        if (vao != other.vao) changed << "VAO";
        if (array != other.array) changed << "ARRAY_BUFFER";
        if (program != other.program) changed << "program";
        if (activeTexture != other.activeTexture) changed << "active texture";
        if (textures != other.textures) changed << "texture bindings";
        if (samplers != other.samplers) changed << "sampler bindings";
        if (capabilities != other.capabilities) changed << "capabilities";
        if (blendEnabled != other.blendEnabled) changed << "indexed blend enable";
        if (colorMasks != other.colorMasks) changed << "indexed color masks";
        if (frontFace != other.frontFace || cullFace != other.cullFace) changed << "culling";
        if (blendFactors != other.blendFactors) changed << "blend factors";
        if (blendEquations != other.blendEquations) changed << "blend equations";
        if (readFramebuffer != other.readFramebuffer) changed << "READ_FRAMEBUFFER";
        if (drawFramebuffer != other.drawFramebuffer) changed << "DRAW_FRAMEBUFFER";
        if (readBuffer != other.readBuffer) changed << "read buffer";
        if (viewport != other.viewport) changed << "viewport";
        if (clearColor != other.clearColor) changed << "clear color";
        return changed.join(", ");
    }
};

struct Attribute
{
    GLint enabled = 0, size = 0, type = 0, normalized = 0, stride = 0, buffer = 0, divisor = 0;
    void* offset = nullptr;
    bool operator==(const Attribute& other) const
    {
        return enabled == other.enabled && size == other.size && type == other.type
            && normalized == other.normalized && stride == other.stride && buffer == other.buffer
            && divisor == other.divisor && offset == other.offset;
    }
};

struct VaoState
{
    GLint element = 0;
    std::array<Attribute, 2> attributes{};
    bool operator==(const VaoState& other) const
    {
        return element == other.element && attributes == other.attributes;
    }
};
}

class GlRenderStateTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::array<std::unique_ptr<QOpenGLShaderProgram>, 2> m_programs;
    QOpenGLFunctions_3_3_Core m_gl;
    bool m_functionsReady = false;
    GLint m_drawBufferCount = 0;
    std::array<GLuint, 2> m_vaos{}, m_framebuffers{}, m_attachments{};
    std::array<GLuint, 5> m_buffers{};
    std::array<GLuint, 4> m_textures{};
    std::array<GLuint, 3> m_samplers{};
    std::array<VaoState, 2> m_expectedVaos;

    State state()
    {
        State result;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &result.vao);
        m_gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &result.array);
        m_gl.glGetIntegerv(GL_CURRENT_PROGRAM, &result.program);
        m_gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &result.activeTexture);
        for (size_t index = 0; index < TextureUnits.size(); ++index) {
            m_gl.glActiveTexture(TextureUnits[index]);
            m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &result.textures[index]);
            m_gl.glGetIntegerv(GL_SAMPLER_BINDING, &result.samplers[index]);
        }
        m_gl.glActiveTexture(static_cast<GLenum>(result.activeTexture));
        for (size_t index = 0; index < Capabilities.size(); ++index)
            result.capabilities[index] = m_gl.glIsEnabled(Capabilities[index]);
        for (GLint index = 0; index < m_drawBufferCount; ++index) {
            result.blendEnabled.push_back(m_gl.glIsEnabledi(GL_BLEND, static_cast<GLuint>(index)));
            std::array<GLboolean, 4> mask{};
            m_gl.glGetBooleani_v(GL_COLOR_WRITEMASK, static_cast<GLuint>(index), mask.data());
            result.colorMasks.push_back(mask);
        }
        m_gl.glGetIntegerv(GL_FRONT_FACE, &result.frontFace);
        m_gl.glGetIntegerv(GL_CULL_FACE_MODE, &result.cullFace);
        m_gl.glGetIntegerv(GL_BLEND_SRC_RGB, &result.blendFactors[0]);
        m_gl.glGetIntegerv(GL_BLEND_DST_RGB, &result.blendFactors[1]);
        m_gl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &result.blendFactors[2]);
        m_gl.glGetIntegerv(GL_BLEND_DST_ALPHA, &result.blendFactors[3]);
        m_gl.glGetIntegerv(GL_BLEND_EQUATION_RGB, &result.blendEquations[0]);
        m_gl.glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &result.blendEquations[1]);
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &result.readFramebuffer);
        m_gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &result.drawFramebuffer);
        m_gl.glGetIntegerv(GL_READ_BUFFER, &result.readBuffer);
        m_gl.glGetIntegerv(GL_VIEWPORT, result.viewport.data());
        m_gl.glGetFloatv(GL_COLOR_CLEAR_VALUE, result.clearColor.data());
        return result;
    }

    VaoState vaoState(GLuint vao)
    {
        VaoState result;
        GLint previous = 0;
        m_gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous);
        m_gl.glBindVertexArray(vao);
        m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &result.element);
        for (GLuint index = 0; index < result.attributes.size(); ++index) {
            Attribute& attribute = result.attributes[index];
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute.enabled);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute.size);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute.type);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribute.normalized);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute.stride);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribute.buffer);
            m_gl.glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &attribute.divisor);
            m_gl.glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribute.offset);
        }
        m_gl.glBindVertexArray(static_cast<GLuint>(previous));
        return result;
    }

    GLint framebufferReadBuffer(GLuint framebuffer)
    {
        GLint previous = 0, result = 0;
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        m_gl.glGetIntegerv(GL_READ_BUFFER, &result);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
        return result;
    }

    bool setBaseline(int caller)
    {
        if (!m_programs[0]->bind()) return false;
        m_gl.glBindVertexArray(m_vaos[static_cast<size_t>(caller)]);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[4]);
        for (size_t index = 0; index < TextureUnits.size(); ++index) {
            m_gl.glActiveTexture(TextureUnits[index]);
            m_gl.glBindTexture(GL_TEXTURE_2D, m_textures[index]);
            m_gl.glBindSampler(TextureUnits[index] - GL_TEXTURE0, m_samplers[index]);
        }
        for (size_t index = 0; index < Capabilities.size(); ++index) {
            if ((int(index) + caller) % 2 == 0) m_gl.glEnable(Capabilities[index]);
            else m_gl.glDisable(Capabilities[index]);
        }
        for (GLint index = 0; index < m_drawBufferCount; ++index) {
            const bool even = index % 2 == 0;
            if (even) m_gl.glEnablei(GL_BLEND, static_cast<GLuint>(index));
            else m_gl.glDisablei(GL_BLEND, static_cast<GLuint>(index));
            m_gl.glColorMaski(static_cast<GLuint>(index), even, !even, GL_TRUE, even);
        }
        m_gl.glFrontFace(GL_CW);
        m_gl.glCullFace(GL_FRONT);
        m_gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA);
        m_gl.glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_SUBTRACT);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffers[1]);
        m_gl.glReadBuffer(GL_NONE);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffers[0]);
        m_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffers[1]);
        m_gl.glViewport(3, 5, 117, 91);
        m_gl.glClearColor(0.125f, 0.25f, 0.5f, 0.75f);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    bool simulateRenderer(int caller)
    {
        if (!m_programs[1]->bind()) return false;
        m_gl.glBindVertexArray(m_vaos[static_cast<size_t>(1 - caller)]);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[static_cast<size_t>(2 * (1 - caller))]);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_textures[3]);
        m_gl.glActiveTexture(GL_TEXTURE1);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        m_gl.glBindSampler(0, m_samplers[2]);
        m_gl.glBindSampler(1, m_samplers[0]);
        for (size_t index = 0; index < Capabilities.size(); ++index) {
            if ((int(index) + caller) % 2 == 0) m_gl.glDisable(Capabilities[index]);
            else m_gl.glEnable(Capabilities[index]);
        }
        m_gl.glDisable(GL_BLEND);
        m_gl.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        m_gl.glFrontFace(GL_CCW);
        m_gl.glCullFace(GL_BACK);
        m_gl.glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        m_gl.glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        m_gl.glReadBuffer(GL_NONE);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffers[1]);
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffers[0]);
        m_gl.glViewport(0, 0, 16, 16);
        m_gl.glClearColor(1, 0, 1, 0);
        return m_gl.glGetError() == GL_NO_ERROR;
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
        m_functionsReady = true;
        m_gl.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &m_drawBufferCount);
        QVERIFY(m_drawBufferCount >= 2);
        for (auto& program : m_programs) {
            program = std::make_unique<QOpenGLShaderProgram>();
            QVERIFY2(program->addShaderFromSourceCode(QOpenGLShader::Vertex,
                "#version 330 core\nvoid main(){gl_Position=vec4(0,0,0,1);}"), qPrintable(program->log()));
            QVERIFY2(program->addShaderFromSourceCode(QOpenGLShader::Fragment,
                "#version 330 core\nout vec4 color;void main(){color=vec4(1);}"), qPrintable(program->log()));
            QVERIFY2(program->link(), qPrintable(program->log()));
        }
        m_gl.glGenVertexArrays(2, m_vaos.data());
        m_gl.glGenSamplers(m_samplers.size(), m_samplers.data());
        m_gl.glGenBuffers(5, m_buffers.data());
        for (size_t index = 0; index < m_vaos.size(); ++index) {
            m_gl.glBindVertexArray(m_vaos[index]);
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[index * 2]);
            m_gl.glBufferData(GL_ARRAY_BUFFER, 32, nullptr, GL_STATIC_DRAW);
            m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffers[index * 2 + 1]);
            m_gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6, nullptr, GL_STATIC_DRAW);
            for (GLuint attribute = 0; attribute < 2; ++attribute) {
                m_gl.glVertexAttribPointer(attribute, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                    reinterpret_cast<const void*>(static_cast<quintptr>(attribute * 2 * sizeof(float))));
                if (attribute == index) m_gl.glEnableVertexAttribArray(attribute);
                else m_gl.glDisableVertexAttribArray(attribute);
            }
            m_expectedVaos[index] = vaoState(m_vaos[index]);
        }
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[4]);
        m_gl.glBufferData(GL_ARRAY_BUFFER, 24, nullptr, GL_STATIC_DRAW);
        m_gl.glGenTextures(4, m_textures.data());
        m_gl.glGenTextures(2, m_attachments.data());
        for (GLuint texture : m_textures) {
            m_gl.glBindTexture(GL_TEXTURE_2D, texture);
            m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        m_gl.glGenFramebuffers(2, m_framebuffers.data());
        for (size_t index = 0; index < m_framebuffers.size(); ++index) {
            m_gl.glBindTexture(GL_TEXTURE_2D, m_attachments[index]);
            m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            m_gl.glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[index]);
            m_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_attachments[index], 0);
            QCOMPARE(m_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        }
        qInfo() << "Render state GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(setBaseline(0));
    }

    void cleanup()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        for (size_t index = 0; index < m_vaos.size(); ++index)
            QVERIFY(vaoState(m_vaos[index]) == m_expectedVaos[index]);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        if (!m_functionsReady || !m_context || !m_surface || !m_context->makeCurrent(m_surface.get())) return;
        for (auto& program : m_programs) program.reset();
        m_gl.glDeleteFramebuffers(2, m_framebuffers.data());
        m_gl.glDeleteTextures(2, m_attachments.data());
        m_gl.glDeleteTextures(4, m_textures.data());
        m_gl.glDeleteSamplers(m_samplers.size(), m_samplers.data());
        m_gl.glDeleteBuffers(5, m_buffers.data());
        m_gl.glDeleteVertexArrays(2, m_vaos.data());
    }

    void saveAndRestorePreserveAllRendererState_data()
    {
        QTest::addColumn<int>("caller");
        QTest::newRow("first-vao") << 0;
        QTest::newRow("second-vao") << 1;
    }

    void saveAndRestorePreserveAllRendererState()
    {
        QFETCH(int, caller);
        QVERIFY(setBaseline(caller));
        const State before = state();
        CGlRenderState profile;
        QVERIFY(profile.Save(m_context.get()));
        const QString saveDifference = before.Difference(state());
        QVERIFY2(saveDifference.isEmpty(), qPrintable(saveDifference));
        QVERIFY(profile.Viewport() == (std::array<GLint, 4>{3, 5, 117, 91}));
        QVERIFY(simulateRenderer(caller));
        QVERIFY(!before.Difference(state()).isEmpty());
        QVERIFY(profile.Restore());
        const QString restoreDifference = before.Difference(state());
        QVERIFY2(restoreDifference.isEmpty(), qPrintable(restoreDifference));
        QCOMPARE(framebufferReadBuffer(m_framebuffers[0]), GLint(GL_COLOR_ATTACHMENT0));
        QCOMPARE(framebufferReadBuffer(m_framebuffers[1]), GLint(GL_NONE));
        for (size_t index = 0; index < m_vaos.size(); ++index)
            QVERIFY(vaoState(m_vaos[index]) == m_expectedVaos[index]);
        QVERIFY(!profile.Restore());
    }

    void zeroVaoRequiresNoAttributeQueries()
    {
        m_gl.glBindVertexArray(0);
        const State before = state();
        QCOMPARE(before.vao, GLint(0));
        CGlRenderState profile;
        QVERIFY(profile.Save(m_context.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        const QString saveDifference = before.Difference(state());
        QVERIFY2(saveDifference.isEmpty(), qPrintable(saveDifference));
        QVERIFY(simulateRenderer(0));
        QVERIFY(profile.Restore());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        const QString restoreDifference = before.Difference(state());
        QVERIFY2(restoreDifference.isEmpty(), qPrintable(restoreDifference));
    }

    void nestedSaveDoesNotOverwriteTheFirstSnapshot()
    {
        CGlRenderState profile;
        QVERIFY(!profile.Restore());
        const State before = state();
        QVERIFY(profile.Save(m_context.get()));
        QVERIFY(simulateRenderer(0));
        const State changed = state();
        QVERIFY(!profile.Save(m_context.get()));
        QVERIFY(!profile.Save(nullptr));
        const QString nestedDifference = changed.Difference(state());
        QVERIFY2(nestedDifference.isEmpty(), qPrintable(nestedDifference));
        QVERIFY(profile.Restore());
        const QString restoreDifference = before.Difference(state());
        QVERIFY2(restoreDifference.isEmpty(), qPrintable(restoreDifference));
        QVERIFY(profile.Save(m_context.get()));
        QVERIFY(profile.Restore());
    }

    void failedSaveDoesNotArmOrChangeTheProfile()
    {
        CGlRenderState profile;
        const State before = state();
        QOpenGLContext invalidContext;
        QVERIFY(!profile.Save(nullptr));
        QVERIFY(!profile.Save(&invalidContext));
        QVERIFY(!profile.Restore());
        m_gl.glEnable(GLenum(0xffff));
        QVERIFY(!profile.Save(m_context.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(!profile.Restore());
        const QString failureDifference = before.Difference(state());
        QVERIFY2(failureDifference.isEmpty(), qPrintable(failureDifference));
        QVERIFY(profile.Save(m_context.get()));
        QVERIFY(simulateRenderer(0));
        QVERIFY(profile.Restore());
        const QString restoreDifference = before.Difference(state());
        QVERIFY2(restoreDifference.isEmpty(), qPrintable(restoreDifference));
    }

    void missingAndForeignContextsAllowRestoreRetryOnTheOwner()
    {
        CGlRenderState profile;
        const State before = state();
        m_context->doneCurrent();
        const auto restoreOwner = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        QVERIFY(!profile.Save(m_context.get()));
        QVERIFY(!profile.Restore());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(profile.Save(m_context.get()));
        QVERIFY(simulateRenderer(0));
        m_context->doneCurrent();
        QVERIFY(!profile.Restore());
        auto foreign = std::make_unique<QOpenGLContext>();
        foreign->setFormat(m_context->format());
        if (!foreign->create() || !foreign->makeCurrent(m_surface.get()))
            QSKIP("Unable to create a second real context for ownership checks.");
        auto* foreignGl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(foreign.get());
        QVERIFY(foreignGl && foreignGl->initializeOpenGLFunctions());
        foreignGl->glViewport(7, 11, 13, 17);
        foreignGl->glClearColor(0.75f, 0.5f, 0.25f, 0.125f);
        QVERIFY(!profile.Restore());
        QVERIFY(!profile.Save(foreign.get()));
        CGlRenderState unsaved;
        QVERIFY(!unsaved.Save(m_context.get()));
        std::array<GLint, 4> foreignViewport{};
        std::array<GLfloat, 4> foreignClear{};
        foreignGl->glGetIntegerv(GL_VIEWPORT, foreignViewport.data());
        foreignGl->glGetFloatv(GL_COLOR_CLEAR_VALUE, foreignClear.data());
        QVERIFY(foreignViewport == (std::array<GLint, 4>{7, 11, 13, 17}));
        QVERIFY(foreignClear == (std::array<GLfloat, 4>{0.75f, 0.5f, 0.25f, 0.125f}));
        QCOMPARE(foreignGl->glGetError(), GLenum(GL_NO_ERROR));
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(profile.Restore());
        const QString restoreDifference = before.Difference(state());
        QVERIFY2(restoreDifference.isEmpty(), qPrintable(restoreDifference));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    GlRenderStateTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "gl_render_state_tests.moc"
