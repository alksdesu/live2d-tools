#include "opengl/cmesh.h"
#include "live2d/LAppSprite.hpp"
#include "opengl/cpsdrendersession.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QScopeGuard>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <memory>

namespace {
QPointer<QOpenGLContext> spriteContext;

class SpriteProbe : public LAppSprite
{
public:
    using LAppSprite::LAppSprite;
    QOpenGLContext* Context() const { return owningContext(); }
    bool Initialized() const { return isInitialized(); }
};

class MeshProbe : public CMesh
{
public:
    QOpenGLContext* Context() const { return owningContext(); }
    bool Initialized() const { return isInitialized(); }
};

void uploadQuad(CMesh& mesh)
{
    float positions[]{-1, -1, 1, -1, 1, 1, -1, 1};
    float uvs[]{0, 0, 1, 0, 1, 1, 0, 1};
    unsigned int indices[]{0, 1, 2, 0, 2, 3};
    mesh.SetConstPoint(uvs, positions, indices);
}
}

// Sprite 使用生产构造与 GL 查询，仅上下文来源由此 fixture 提供。
LAppDelegate::LAppDelegate() = default;
LAppDelegate::~LAppDelegate() = default;
LAppDelegate* LAppDelegate::GetInstance()
{
    static LAppDelegate delegate;
    return &delegate;
}
QOpenGLContext* LAppDelegate::getGlOwningContext() { return spriteContext; }
CPsdRenderSession::~CPsdRenderSession() { qFatal("The Sprite context fixture must not create a PSD render session."); }

class MeshContextTests : public QObject
{
    Q_OBJECT
    QOpenGLContext m_context;
    QOffscreenSurface m_surface;
    QOpenGLFunctions_3_3_Core m_gl;

private slots:
    void initTestCase()
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        m_context.setFormat(format);
        if (!m_context.create() || !m_context.isValid()) QSKIP("A real OpenGL 3.3 Core context is unavailable.");
        const auto actual = m_context.format();
        if (actual.profile() != QSurfaceFormat::CoreProfile || actual.majorVersion() < 3 ||
            (actual.majorVersion() == 3 && actual.minorVersion() < 3)) QSKIP("The context is not OpenGL 3.3 Core.");
        m_surface.setFormat(actual);
        m_surface.create();
        if (!m_surface.isValid() || !m_context.makeCurrent(&m_surface)) QSKIP("The offscreen surface is unavailable.");
        QVERIFY(m_gl.initializeOpenGLFunctions());
        qInfo() << "Mesh context GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION));
    }

    void init()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void invalidOwnerRequestsCannotInitializeOrRetargetFunctions()
    {
        MeshProbe mesh;
        mesh.setGlOwningContext(nullptr);
        QVERIFY(!mesh.Context());
        QOpenGLContext foreign;
        foreign.setFormat(m_context.format());
        QVERIFY(foreign.create());
        QOffscreenSurface foreignSurface;
        foreignSurface.setFormat(foreign.format());
        foreignSurface.create();
        QVERIFY(foreignSurface.isValid());
        mesh.setGlOwningContext(&foreign);
        QVERIFY(!mesh.Context());
        QVERIFY(!mesh.Initialized());
        m_context.doneCurrent();
        mesh.setGlOwningContext(&m_context);
        mesh.SetConstPoint(nullptr, nullptr, nullptr);
        mesh.SetConstPointEBO(nullptr, nullptr);
        mesh.Draw();
        QVERIFY(!mesh.Context());
        QCOMPARE(mesh.getVAO(), GLuint(0));
        QVERIFY(m_context.makeCurrent(&m_surface));
        mesh.setGlOwningContext(&m_context);
        QCOMPARE(mesh.Context(), &m_context);
        QVERIFY(mesh.Initialized());
        uploadQuad(mesh);
        const GLuint ownedArray = mesh.getVAO();
        QVERIFY(m_gl.glIsVertexArray(ownedArray));
        QVERIFY(foreign.makeCurrent(&foreignSurface));
        QOpenGLFunctions_3_3_Core other;
        QVERIFY(other.initializeOpenGLFunctions());
        GLuint sentinel = 0;
        other.glGenVertexArrays(1, &sentinel);
        other.glBindVertexArray(sentinel);
        mesh.setGlOwningContext(&foreign);
        QCOMPARE(mesh.Context(), &m_context);
        mesh.SetConstPoint(nullptr, nullptr, nullptr);
        mesh.SetConstPointEBO(nullptr, nullptr);
        mesh.SetImage(QStringLiteral("missing.png"));
        mesh.Draw();
        QCOMPARE(mesh.getVAO(), ownedArray);
        QVERIFY(other.glIsVertexArray(sentinel));
        QCOMPARE(other.glGetError(), GLenum(GL_NO_ERROR));
        other.glDeleteVertexArrays(1, &sentinel);
        QVERIFY(m_context.makeCurrent(&m_surface));
        QVERIFY(m_gl.glIsVertexArray(ownedArray));
    }

    void destructorWithOwnerCurrentDeletesGeometry()
    {
        GLuint array = 0, element = 0;
        {
            MeshProbe mesh;
            mesh.setGlOwningContext(&m_context);
            uploadQuad(mesh);
            array = mesh.getVAO();
            QVERIFY(m_gl.glIsVertexArray(array));
            m_gl.glBindVertexArray(array);
            GLint binding = 0;
            m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &binding);
            element = static_cast<GLuint>(binding);
            QVERIFY(m_gl.glIsBuffer(element));
            m_gl.glBindVertexArray(0);
        }
        QVERIFY(!m_gl.glIsVertexArray(array));
        QVERIFY(!m_gl.glIsBuffer(element));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void destroyedOwnerInvalidatesFunctionsBeforeMeshDestruction()
    {
        auto mesh = std::make_unique<MeshProbe>();
        auto owner = std::make_unique<QOpenGLContext>();
        owner->setFormat(m_context.format());
        QVERIFY(owner->create());
        QOffscreenSurface surface;
        surface.setFormat(owner->format());
        surface.create();
        QVERIFY(surface.isValid());
        QVERIFY(owner->makeCurrent(&surface));
        mesh->setGlOwningContext(owner.get());
        uploadQuad(*mesh);
        QVERIFY(mesh->getVAO());
        QVERIFY(mesh->Initialized());
        owner->doneCurrent();
        owner.reset();
        QVERIFY(!mesh->Context());
        QVERIFY(!mesh->Initialized());
        QVERIFY(m_context.makeCurrent(&m_surface));
        GLuint sentinelArray = 0, sentinelBuffer = 0;
        m_gl.glGenVertexArrays(1, &sentinelArray);
        m_gl.glBindVertexArray(sentinelArray);
        m_gl.glGenBuffers(1, &sentinelBuffer);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, sentinelBuffer);
        const auto cleanup = qScopeGuard([&] {
            m_gl.glDeleteBuffers(1, &sentinelBuffer);
            m_gl.glDeleteVertexArrays(1, &sentinelArray);
        });
        mesh->SetConstPoint(nullptr, nullptr, nullptr);
        mesh->SetConstPointEBO(nullptr, nullptr);
        mesh->Draw();
        mesh.reset();
        QVERIFY(m_gl.glIsVertexArray(sentinelArray));
        QVERIFY(m_gl.glIsBuffer(sentinelBuffer));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void spriteInvalidContextConstructionDoesNotCallGl()
    {
        QOpenGLContext foreign;
        foreign.setFormat(m_context.format());
        QVERIFY(foreign.create());
        spriteContext = &foreign;
        const auto resetSource = qScopeGuard([&] { spriteContext.clear(); });
        SpriteProbe wrong(0, 0, 2, 2, 0, 0);
        QVERIFY(!wrong.Context());
        QVERIFY(!wrong.Initialized());
        wrong.SetWindowSize(100, 100);
        wrong.Render();
        wrong.RenderImmidiate(0, nullptr);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        m_context.doneCurrent();
        spriteContext = &m_context;
        SpriteProbe missing(0, 0, 2, 2, 0, 0);
        missing.SetWindowSize(100, 100);
        missing.Render();
        missing.RenderImmidiate(0, nullptr);
        QVERIFY(!missing.Context());
        QVERIFY(!missing.Initialized());
        QVERIFY(m_context.makeCurrent(&m_surface));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void spriteContextDestructionInvalidatesActualProductionFunctions()
    {
        auto owner = std::make_unique<QOpenGLContext>();
        owner->setFormat(m_context.format());
        QVERIFY(owner->create());
        QOffscreenSurface surface;
        surface.setFormat(owner->format());
        surface.create();
        QVERIFY(surface.isValid());
        QVERIFY(owner->makeCurrent(&surface));
        std::unique_ptr<SpriteProbe> sprite;
        spriteContext = owner.get();
        const auto resetSource = qScopeGuard([&] { spriteContext.clear(); });
        {
            QOpenGLShaderProgram shader;
            QVERIFY(shader.addShaderFromSourceCode(QOpenGLShader::Vertex,
                "#version 330 core\n"
                "in vec2 position; in vec2 uv; out vec2 texcoord;"
                "void main(){gl_Position=vec4(position,0,1);texcoord=uv;}"));
            QVERIFY(shader.addShaderFromSourceCode(QOpenGLShader::Fragment,
                "#version 330 core\n"
                "uniform sampler2D texture; uniform vec4 baseColor; in vec2 texcoord; out vec4 color;"
                "void main(){color=textureLod(texture,texcoord,0.0)*baseColor;}"));
            QVERIFY(shader.link());
            sprite = std::make_unique<SpriteProbe>(0, 0, 2, 2, 0, shader.programId());
            QCOMPARE(sprite->Context(), owner.get());
            QVERIFY(sprite->Initialized());
            QCOMPARE(sprite->glGetError(), GLenum(GL_NO_ERROR));
            QVERIFY(m_context.makeCurrent(&m_surface));
            sprite->setGlOwningContext(&m_context);
            QCOMPARE(sprite->Context(), owner.get());
            sprite->SetWindowSize(100, 100);
            sprite->Render();
            sprite->RenderImmidiate(0, nullptr);
            QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
            QVERIFY(owner->makeCurrent(&surface));
        }
        owner->doneCurrent();
        owner.reset();
        QVERIFY(!sprite->Context());
        QVERIFY(!sprite->Initialized());
        QVERIFY(m_context.makeCurrent(&m_surface));
        sprite->Render();
        sprite->RenderImmidiate(0, nullptr);
        sprite.reset();
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        m_context.doneCurrent();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    MeshContextTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "mesh_context_tests.moc"
