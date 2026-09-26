#include "edit/cdrawmesh.h"
#include "edit/cmeshmap.h"
#include "edit/cdrawrect.h"
#include "edit/cdrawrotation.h"
#include "live2dstruct/coor/crotationcoortype.h"
#include "live2dstruct/coor/cwarpcoortype.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <QSet>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>

namespace {
struct GlState
{
    std::array<GLint, 16> integers{};
    std::array<GLfloat, 2> sizes{};
    GLboolean programPointSize = false;
    GLboolean culling = false;

    bool operator==(const GlState& other) const
    {
        return integers == other.integers && sizes == other.sizes && programPointSize == other.programPointSize && culling == other.culling;
    }
};

bool Foreground(QRgb pixel)
{
    return qRed(pixel) > 32 || qGreen(pixel) > 32 || qBlue(pixel) > 32;
}

int Count(const QImage& image, QRect rect, const std::function<bool(QRgb)>& predicate = Foreground)
{
    rect = rect.intersected(image.rect());
    int count = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            if (predicate(image.pixel(x, y))) ++count;
    return count;
}

QRect Near(float x, float y, int radius = 7)
{
    const QPoint center(qRound((x + 1) * 128), qRound((1 - y) * 128));
    return QRect(center.x() - radius, center.y() - radius, radius * 2 + 1, radius * 2 + 1);
}

bool rotationFrame(live2dstruct::CRotationNode& node, live2dstruct::CRotationNodeKeyform& form,
                   const std::shared_ptr<const live2dstruct::CCoordinateFrame>& parent = {})
{
    using namespace live2dstruct;
    std::shared_ptr<const CCoordinateFrame> frame;
    CCoordinateFrame::RotationBasis basis;
    if (!CCoordinateFrame::CreateRotation(node.GetBaseAngles(), form, parent, frame, basis)) return false;
    node.SetParentFrame(parent);
    node.SetCurKeyForm(form);
    node.SetCanvasBasis(basis);
    return node.UpdateCanvasPositions(form.GetPosition());
}
}

class EditorOverlayRenderTests : public QObject
{
    Q_OBJECT

    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLFramebufferObject> m_framebuffer;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    QOpenGLFunctions_3_3_Core m_gl;
    bool m_ready = false;
    GLuint m_vertexArray = 0;
    std::array<GLuint, 3> m_buffers{};
    std::array<GLuint, 2> m_textures{};

    GlState State(QOpenGLFunctions_3_3_Core& gl)
    {
        GlState state;
        const std::array<GLenum, 12> names{GL_VERTEX_ARRAY_BINDING, GL_ARRAY_BUFFER_BINDING,
            GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE,
            GL_PIXEL_UNPACK_BUFFER_BINDING, GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
            GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_DRAW_FRAMEBUFFER_BINDING, GL_READ_FRAMEBUFFER_BINDING};
        for (size_t index = 0; index < names.size(); ++index) gl.glGetIntegerv(names[index], &state.integers[index]);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.integers[12]);
        gl.glActiveTexture(GL_TEXTURE3);
        gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.integers[13]);
        gl.glActiveTexture(state.integers[4]);
        gl.glGetIntegerv(GL_POLYGON_MODE, &state.integers[14]);
        gl.glGetFloatv(GL_POINT_SIZE, &state.sizes[0]);
        gl.glGetFloatv(GL_LINE_WIDTH, &state.sizes[1]);
        state.programPointSize = gl.glIsEnabled(GL_PROGRAM_POINT_SIZE);
        state.culling = gl.glIsEnabled(GL_CULL_FACE);
        return state;
    }

    bool Prepare()
    {
        if (!m_context->makeCurrent(m_surface.get()) || !m_framebuffer->bind() || !m_program->bind()) return false;
        m_gl.glViewport(0, 0, 256, 256);
        for (GLenum flag : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                           GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DITHER}) m_gl.glDisable(flag);
        m_gl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.glClearColor(0, 0, 0, 1);
        m_gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[0]);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffers[1]);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_buffers[2]);
        m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 9);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        m_gl.glEnable(GL_PROGRAM_POINT_SIZE);
        m_gl.glPointSize(7);
        GLfloat lineRange[2];
        m_gl.glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lineRange);
        m_gl.glLineWidth(lineRange[1] >= 2 ? 2 : 1);
        return m_gl.glGetError() == GL_NO_ERROR;
    }

    bool Draw(const std::function<void()>& draw)
    {
        const GlState before = State(m_gl);
        draw();
        const GlState after = State(m_gl);
        const GLenum error = m_gl.glGetError();
        if (!(before == after) || error != GL_NO_ERROR) {
            qWarning() << "Overlay state changed:" << !(before == after) << "GL error:" << error;
            return false;
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        format.setSamples(0);
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(format);
        if (!m_context->create() || !m_context->isValid()) QSKIP("OpenGL 3.3 Core context is unavailable.");
        const auto actual = m_context->format();
        if (actual.profile() != QSurfaceFormat::CoreProfile || actual.majorVersion() < 3 ||
            (actual.majorVersion() == 3 && actual.minorVersion() < 3)) QSKIP("The created context does not support OpenGL 3.3 Core.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get()) || !m_gl.initializeOpenGLFunctions())
            QSKIP("Core functions or the offscreen surface are unavailable.");
        m_ready = true;
        QOpenGLFramebufferObjectFormat framebufferFormat;
        framebufferFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        framebufferFormat.setInternalTextureFormat(GL_RGBA8);
        framebufferFormat.setSamples(0);
        m_framebuffer = std::make_unique<QOpenGLFramebufferObject>(256, 256, framebufferFormat);
        QVERIFY(m_framebuffer->isValid());
        m_program = std::make_unique<QOpenGLShaderProgram>();
        QVERIFY(m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 330 core\nvoid main(){gl_Position=vec4(0,0,0,1);gl_PointSize=1;}"));
        QVERIFY(m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 330 core\nout vec4 color;void main(){color=vec4(1,0,1,1);}"));
        QVERIFY(m_program->link());
        m_gl.glGenVertexArrays(1, &m_vertexArray);
        m_gl.glGenBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
        m_gl.glBindVertexArray(m_vertexArray);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, m_buffers[0]);
        m_gl.glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffers[1]);
        m_gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, 16, nullptr, GL_STATIC_DRAW);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_buffers[2]);
        m_gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        m_gl.glGenTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        qInfo() << "Overlay GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION));
    }

    void meshStateMigrationKeepsCandidateGeometryAndGpuOwnership()
    {
        m_context->doneCurrent();
        const float original[]{-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f};
        const float changed[]{-0.8f, -0.2f, 0.2f, -0.2f, -0.3f, 0.8f};
        const short indices[]{0, 1, 2};
        auto source = std::make_unique<CDrawMesh>(3, indices, 6, original, original);
        CDrawMesh candidate(3, indices, 6, original, changed);
        source->SetShow(true);
        QVERIFY(source->SelectNearestPoint({0, 0.5f}, 0.01f));
        candidate.SetIsOver(true);
        CMeshMap sourceMap, candidateMap;
        sourceMap.insert("mesh", source.get());
        candidateMap.insert("mesh", &candidate);
        QMatrix4x4 projection;
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { source->Draw(projection); }));
        const auto sourcePositions = source->Positions();
        const auto sourceArray = source->m_regularPoints.GetVertexArray();
        const auto sourceShader = source->shader.get();
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { candidate.Draw(projection); }));
        const auto candidatePositions = candidate.Positions();
        const auto candidateArray = candidate.m_regularPoints.GetVertexArray();
        const auto candidateShader = candidate.shader.get();
        QVERIFY(sourceArray != 0 && candidateArray != 0 && sourceArray != candidateArray);
        QVERIFY(sourceShader != candidateShader);
        m_context->doneCurrent();
        QVERIFY(candidateMap.CopyEditorStateFrom(sourceMap));
        QCOMPARE(candidate.m_regularPoints.GetVertexArray(), candidateArray);
        QCOMPARE(candidate.shader.get(), candidateShader);
        QCOMPARE(source->m_regularPoints.GetVertexArray(), sourceArray);
        QCOMPARE(source->shader.get(), sourceShader);
        QCOMPARE(candidate.Positions(), candidatePositions);
        QCOMPARE(source->Positions(), sourcePositions);
        QCOMPARE(candidate.GetSelectPoint().size(), size_t(1));
        QCOMPARE(candidate.GetSelectPoint().front(), candidate.GetMesh()->GetWolrd()[2]);
        QVERIFY(candidate.GetSelectPoint().front() != source->GetSelectPoint().front());
        QVERIFY(source->IsShown());
        QVERIFY(candidate.IsShown());
        QVERIFY(!candidate.IsHovered());
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { candidate.Draw(projection); }));
        const auto red = [](QRgb pixel) { return qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) < 80; };
        QVERIFY(Count(m_framebuffer->toImage(), Near(-0.3f, 0.8f), red) >= 4);
        QCOMPARE(Count(m_framebuffer->toImage(), Near(0, 0.5f), red), 0);
        QCOMPARE(candidate.shader.get(), candidateShader);
        QCOMPARE(source->Positions(), sourcePositions);
        source->CancelSelect();
        source->SetShow(false);
        source->SetIsOver(true);
        bool copied = false;
        QVERIFY(Draw([&] { copied = candidateMap.CopyEditorStateFrom(sourceMap); }));
        QVERIFY(copied);
        QVERIFY(!candidate.IsShown());
        QVERIFY(candidate.IsHovered());
        QVERIFY(candidate.GetSelectPoint().empty());
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { candidate.Draw(projection); }));
        const auto green = [](QRgb pixel) { return qGreen(pixel) > 200 && qRed(pixel) < 80 && qBlue(pixel) < 80; };
        QVERIFY(Count(m_framebuffer->toImage(), Near(-0.3f, 0.8f), green) >= 4);
        source->SetShow(true);
        QVERIFY(source->SelectNearestPoint({0, 0.5f}, 0.01f));
        QVERIFY(candidateMap.CopyEditorStateFrom(sourceMap));
        source.reset();
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { candidate.Draw(projection); }));
        QVERIFY(Count(m_framebuffer->toImage(), Near(-0.3f, 0.8f), red) >= 4);
        QCOMPARE(candidate.Positions(), candidatePositions);
        QCOMPARE(candidate.GetSelectPoint().front(), candidate.GetMesh()->GetWolrd()[2]);
    }

    void meshConstructsWithoutContextAndBatchesSelection()
    {
        m_context->doneCurrent();
        const float positions[]{-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f};
        const short indices[]{0, 1, 2};
        CDrawMesh mesh(3, indices, 6, positions, positions);
        mesh.SetShow(true);
        QVERIFY(!mesh.shader);
        QCOMPARE(mesh.m_regularPoints.GetVertexArray(), GLuint(0));
        QMatrix4x4 projection;
        mesh.Draw(projection);
        QVERIFY(!mesh.shader);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        const QImage initial = m_framebuffer->toImage();
        QVERIFY(Count(initial, Near(-0.5f, -0.5f)) > 8);
        QVERIFY(Count(initial, Near(0, 0.5f)) > 8);
        QCOMPARE(Count(initial, Near(0, 0, 12)), 0);
        const GLuint initialPoints = mesh.m_regularPoints.GetVertexArray();
        QVERIFY(initialPoints != 0);
        QVERIFY(mesh.SelectNearestPoint(QVector2D(0, 0.5f), 0.01f));
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        const auto red = [](QRgb pixel) { return qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) < 80; };
        QVERIFY(Count(m_framebuffer->toImage(), Near(0, 0.5f), red) >= 4);
        const GLuint selectedPoints = mesh.m_selectedPoints.GetVertexArray();
        const GLuint regularPoints = mesh.m_regularPoints.GetVertexArray();
        const GLuint lines = mesh.m_lines.front()->GetVertexArray();
        QVERIFY(selectedPoints != 0 && regularPoints != 0 && lines != 0);
        auto* shader = mesh.shader.get();
        for (int pass = 0; pass < 3; ++pass) {
            QVERIFY(Prepare());
            QVERIFY(Draw([&] { mesh.Draw(projection); }));
            QCOMPARE(mesh.m_selectedPoints.GetVertexArray(), selectedPoints);
            QCOMPARE(mesh.m_regularPoints.GetVertexArray(), regularPoints);
            QCOMPARE(mesh.m_lines.front()->GetVertexArray(), lines);
            QCOMPARE(mesh.shader.get(), shader);
        }
        mesh.MoveSelected(QVector2D(0.25f, 0));
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        const QImage moved = m_framebuffer->toImage();
        QVERIFY(Count(moved, Near(0.25f, 0.5f), red) >= 4);
        QCOMPARE(Count(moved, Near(0, 0.5f), red), 0);
        mesh.SetSelect(QRectF(-0.75, -0.75, 1.5, 0.5));
        QCOMPARE(mesh.GetSelectPoint().size(), size_t(2));
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        const QImage multiple = m_framebuffer->toImage();
        QVERIFY(Count(multiple, Near(-0.5f, -0.5f), red) >= 4);
        QVERIFY(Count(multiple, Near(0.5f, -0.5f), red) >= 4);
        QCOMPARE(Count(multiple, Near(0.25f, 0.5f), red), 0);
        mesh.CancelSelect();
        mesh.SetShow(false);
        mesh.SetIsOver(true);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        const auto green = [](QRgb pixel) { return qGreen(pixel) > 200 && qRed(pixel) < 80 && qBlue(pixel) < 80; };
        QVERIFY(Count(m_framebuffer->toImage(), Near(0.25f, 0.5f), green) > 8);
        QSet<GLuint> lineArrays;
        for (int pass = 0; pass < 8; ++pass) {
            QVERIFY(mesh.SetPositions({-0.5f, -0.5f, 0.5f, -0.5f, 0.1f * pass, 0.5f}));
            QVERIFY(Prepare());
            QVERIFY(Draw([&] { mesh.Draw(projection); }));
            lineArrays.insert(mesh.m_lines.front()->GetVertexArray());
        }
        QCOMPARE(lineArrays.size(), 2);
        projection(0, 0) = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { mesh.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
    }

    void rectangleDrawsOnlyOutlineAndUpdatesWithoutChangingPolygonMode()
    {
        m_context->doneCurrent();
        CDrawRect rectangle(QVector3D(1, 1, 1));
        rectangle.InitOpenGl();
        rectangle.SetSelectRect(QRectF(-0.5, 0, 1, 0.5));
        rectangle.SetDraw(true);
        QVERIFY(!rectangle.shader);
        QMatrix4x4 projection;
        rectangle.Draw(projection);
        QVERIFY(!rectangle.shader);
        QVERIFY(Prepare());
        m_gl.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        QVERIFY(Draw([&] { rectangle.Draw(projection); }));
        const QImage initial = m_framebuffer->toImage();
        QVERIFY(Count(initial, Near(0, 0.5f)) > 8);
        QVERIFY(Count(initial, Near(0.5f, 0.25f)) > 8);
        QCOMPARE(Count(initial, Near(0, 0.25f, 12)), 0);
        const GLuint first = rectangle.m_geometry.GetVertexArray();
        auto* shader = rectangle.shader.get();
        QVERIFY(first != 0);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rectangle.Draw(projection); }));
        QCOMPARE(rectangle.m_geometry.GetVertexArray(), first);
        QCOMPARE(rectangle.shader.get(), shader);
        rectangle.SetSelectRect(QRectF(-0.5, -0.5, 1, 0.25));
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rectangle.Draw(projection); }));
        const QImage moved = m_framebuffer->toImage();
        QCOMPARE(Count(moved, Near(0, 0.5f)), 0);
        QVERIFY(Count(moved, Near(0, -0.5f)) > 8);
        rectangle.SetSelectRect(QRectF(std::numeric_limits<double>::infinity(), 0, 1, 1));
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rectangle.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
        rectangle.SetSelectRect(QRectF(-0.5, 0, 1, 0.5));
        projection(1, 1) = std::numeric_limits<float>::infinity();
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rectangle.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
    }

    void rotationUsesCoreYAndParentTransform()
    {
        using namespace live2dstruct;
        CRotationNode node(QStringLiteral("rotation"));
        CRotationNodeKeyform key(1, 0, 0.25f, 0.5f, 1, false, false);
        QVERIFY(rotationFrame(node, key));
        m_context->doneCurrent();
        CDrawRotation rotation;
        rotation.InitOpenGl();
        rotation.Init(&node);
        rotation.SetDraw(true);
        QVERIFY(!rotation.shader && !rotation.m_rclTex && !rotation.m_dirTex);
        QMatrix4x4 projection;
        rotation.Draw(projection);
        QVERIFY(!rotation.shader);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        const QImage initial = m_framebuffer->toImage();
        QVERIFY(Count(initial, Near(0.25f, -0.5f)) > 8);
        QCOMPARE(Count(initial, Near(0.25f, 0.5f, 16)), 0);
        const GLuint origin = rotation.m_originGeometry.GetVertexArray();
        const GLuint direction = rotation.m_directionGeometry.GetVertexArray();
        const GLuint originTexture = rotation.m_rclTex->textureId();
        const GLuint directionTexture = rotation.m_dirTex->textureId();
        auto* shader = rotation.shader.get();
        QVERIFY(origin && direction && originTexture && directionTexture);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        QCOMPARE(rotation.m_originGeometry.GetVertexArray(), origin);
        QCOMPARE(rotation.m_directionGeometry.GetVertexArray(), direction);
        QCOMPARE(rotation.m_rclTex->textureId(), originTexture);
        QCOMPARE(rotation.m_dirTex->textureId(), directionTexture);
        QCOMPARE(rotation.shader.get(), shader);
        CRotationNodeKeyform parentKey(1, 90, 0, 0, 1, false, false);
        auto parent = std::make_shared<CCoordinateFrame>(std::make_unique<CRotationCoorType>(0, parentKey));
        key.Setorigin_x(0.4f);
        key.Setorigin_y(0.2f);
        QVERIFY(rotationFrame(node, key, parent));
        rotation.Init(&node);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        const QImage moved = m_framebuffer->toImage();
        QVERIFY(Count(moved, Near(-0.2f, -0.4f)) > 8);
        QCOMPARE(Count(moved, Near(0.25f, -0.5f)), 0);
        key.Setangles(std::numeric_limits<float>::infinity());
        node.SetCurKeyForm(key);
        rotation.Init(&node);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
        key.Setangles(0);
        node.SetCurKeyForm(key);
        rotation.Init(nullptr);
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
        rotation.Init(&node);
        projection(2, 2) = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(Prepare());
        QVERIFY(Draw([&] { rotation.Draw(projection); }));
        QCOMPARE(Count(m_framebuffer->toImage(), initial.rect()), 0);
    }

    void rotationGlyphKeepsItsSizeUnderWarpAndAtZeroScale()
    {
        using namespace live2dstruct;
        const std::vector<float> grid{-0.3f, -0.5f, 1.7f, -0.5f, 0.7f, 2.5f, 2.7f, 2.5f};
        auto parent = std::make_shared<CCoordinateFrame>(std::make_unique<CWarpCoorType>(1, 1, grid, true));
        QImage baseline;
        for (float scale : {0.0f, 1.0f / 2400, 2.0f}) {
            CRotationNode node("rotation");
            CRotationNodeKeyform form(1, 0, 0.1f, 0.1f, scale, false, false);
            QVERIFY(rotationFrame(node, form, parent));
            CDrawRotation rotation; QVERIFY(rotation.Init(&node)); rotation.SetDraw(true);
            QVERIFY(Prepare()); m_gl.glEnable(GL_CULL_FACE);
            QVERIFY(Draw([&] { rotation.Draw(QMatrix4x4()); }));
            const auto rendered = m_framebuffer->toImage();
            QVERIFY(Count(rendered, Near(0, 0.2f)) > 8);
            QVERIFY(std::abs(rotation.vetdirPos[2] - (-0.01 / std::sqrt(1.0 + 1.0 / 9) - 0.1 / std::sqrt(10.0))) < 1e-5);
            if (baseline.isNull()) baseline = rendered;
            else QCOMPARE(rendered, baseline);
        }
    }

    void rotationGlyphFollowsNegativeScaleAndRestoresCulling()
    {
        using namespace live2dstruct;
        CRotationNode node("rotation");
        CRotationNodeKeyform form(1, 0, 0.25f, 0.5f, -2, false, false);
        QVERIFY(rotationFrame(node, form));
        CDrawRotation rotation; QVERIFY(rotation.Init(&node)); rotation.SetDraw(true);
        QVERIFY(std::abs(rotation.vetdirPos[2] - 0.26f) < 1e-6);
        QVERIFY(std::abs(rotation.vetdirPos[3] - (-0.6f)) < 1e-6);
        QVERIFY(Prepare()); m_gl.glEnable(GL_CULL_FACE);
        QVERIFY(Draw([&] { rotation.Draw(QMatrix4x4()); }));
        QVERIFY(Count(m_framebuffer->toImage(), Near(0.25f, -0.5f)) > 8);
        QVERIFY(m_gl.glIsEnabled(GL_CULL_FACE));
    }

    void establishedOverlaysRejectForeignAndMissingContexts()
    {
        const float positions[]{-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f};
        const short indices[]{0, 1, 2};
        CDrawMesh mesh(3, indices, 6, positions, positions);
        CDrawRect rectangle(QVector3D(1, 1, 1));
        live2dstruct::CRotationNode node(QStringLiteral("rotation"));
        live2dstruct::CRotationNodeKeyform key(1, 0, 0.25f, 0.5f, 1, false, false);
        QVERIFY(rotationFrame(node, key));
        CDrawRotation rotation;
        mesh.SetShow(true);
        rectangle.SetSelectRect(QRectF(-0.5, 0, 1, 0.5));
        rectangle.SetDraw(true);
        rotation.Init(&node);
        rotation.SetDraw(true);
        QMatrix4x4 projection;
        const auto drawAll = [&] { mesh.Draw(projection); rectangle.Draw(projection); rotation.Draw(projection); };
        QVERIFY(Prepare());
        QVERIFY(Draw(drawAll));
        const std::array<GLuint, 3> arrays{mesh.m_regularPoints.GetVertexArray(), rectangle.m_geometry.GetVertexArray(),
                                          rotation.m_originGeometry.GetVertexArray()};
        const auto owner = m_context.get();
        m_context->doneCurrent();
        drawAll();
        QCOMPARE(mesh.m_regularPoints.GetVertexArray(), arrays[0]);
        QCOMPARE(rectangle.m_geometry.GetVertexArray(), arrays[1]);
        QCOMPARE(rotation.m_originGeometry.GetVertexArray(), arrays[2]);
        auto foreign = std::make_unique<QOpenGLContext>();
        foreign->setFormat(m_context->format());
        QVERIFY(foreign->create());
        auto surface = std::make_unique<QOffscreenSurface>();
        surface->setFormat(foreign->format());
        surface->create();
        QVERIFY(surface->isValid());
        QVERIFY(foreign->makeCurrent(surface.get()));
        const auto restoreOwner = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(foreign.get());
        QVERIFY(gl && gl->initializeOpenGLFunctions());
        const GlState before = State(*gl);
        drawAll();
        QVERIFY(State(*gl) == before);
        QCOMPARE(gl->glGetError(), GLenum(GL_NO_ERROR));
        QCOMPARE(mesh.m_context.data(), owner);
        QCOMPARE(rectangle.m_context.data(), owner);
        QCOMPARE(rotation.m_context.data(), owner);
        QVERIFY(Prepare());
        QVERIFY(Draw(drawAll));
        QVERIFY(Count(m_framebuffer->toImage(), Near(0.25f, 0.5f)) > 8);
        QCOMPARE(mesh.m_regularPoints.GetVertexArray(), arrays[0]);
        QCOMPARE(rectangle.m_geometry.GetVertexArray(), arrays[1]);
        QCOMPARE(rotation.m_originGeometry.GetVertexArray(), arrays[2]);
    }

    void cleanupTestCase()
    {
        if (!m_ready || !m_context || !m_surface || !m_context->makeCurrent(m_surface.get())) return;
        m_program.reset();
        m_framebuffer.reset();
        m_gl.glBindVertexArray(0);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        m_gl.glDeleteBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
        m_gl.glDeleteVertexArrays(1, &m_vertexArray);
        m_gl.glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
        m_context->doneCurrent();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    EditorOverlayRenderTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "editor_overlay_render_tests.moc"
