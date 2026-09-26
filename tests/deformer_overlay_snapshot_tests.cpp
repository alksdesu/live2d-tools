#include "live2d/LAppView.hpp"
#include "deformer_snapshot_fixture.h"

#include <QtTest>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QScopeGuard>
#include <array>
#include <type_traits>

using namespace live2dstruct;
using namespace DeformerSnapshotData;
using Overlays = LAppView::PreparedDeformerOverlays;

class DeformerOverlaySnapshotTests : public QObject
{
    Q_OBJECT
    QOpenGLContext m_context;
    QOffscreenSurface m_surface;
    QOpenGLFunctions_3_3_Core m_gl;

    static bool makeSnapshot(CTreeManage::Snapshot& snapshot, float offset = 0)
    {
        Fixture fixture;
        if (!fixture.table.IsValid()) return false;
        fixture.column(fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 1, -1);
        const QVector<float> positions{-0.75f, -0.75f, 0.25f, -0.75f, -0.75f, 0.25f, 0.25f, 0.25f};
        for (int index = 0; index < positions.size(); ++index)
            real(fixture.bytes, fixture.table.sectionOffsetTable->keyform_positions->xys + (12 + index) * 4,
                 positions[index] + (index % 2 ? 0 : offset));
        return CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, snapshot);
    }

    static bool makeAnimatedSnapshot(CTreeManage::Snapshot& snapshot, bool differingReflections = false)
    {
        Fixture fixture;
        if (!fixture.table.IsValid()) return false;
        fixture.BindRoot(differingReflections);
        const auto& offsets = *fixture.table.sectionOffsetTable;
        for (int index = 0; index < 2; ++index) {
            real(fixture.bytes, offsets.rotation_deformer_keyforms->angles + index * 4, 0);
            real(fixture.bytes, offsets.rotation_deformer_keyforms->origin_x + index * 4, index ? 0.5f : 0);
            real(fixture.bytes, offsets.rotation_deformer_keyforms->origin_y + index * 4, 0);
        }
        const QVector<float> positions{-0.75f, -0.75f, 0.25f, -0.75f, -0.75f, 0.25f, 0.25f, 0.25f};
        for (int index = 0; index < positions.size(); ++index)
            real(fixture.bytes, offsets.keyform_positions->xys + (12 + index) * 4, positions[index]);
        return CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, snapshot);
    }

    QImage draw(Overlays& overlays, QOpenGLFramebufferObject& framebuffer)
    {
        if (!framebuffer.bind()) return {};
        m_gl.glViewport(0, 0, 256, 256);
        for (GLenum flag : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
                           GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DITHER}) m_gl.glDisable(flag);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.glClearColor(0, 0, 0, 1);
        m_gl.glClear(GL_COLOR_BUFFER_BIT);
        overlays.Draw(QMatrix4x4());
        return framebuffer.toImage();
    }

    static bool lineNear(const QImage& image, int x, int y)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const QRgb pixel = image.pixel(x + dx, y + dy);
                if (qRed(pixel) > 64 || qGreen(pixel) > 64 || qBlue(pixel) > 64) return true;
            }
        return false;
    }

    static int foregroundCount(const QImage& image)
    {
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixel(x, y);
                if (qRed(pixel) > 64 || qGreen(pixel) > 64 || qBlue(pixel) > 64) ++count;
            }
        return count;
    }

    std::array<GLint, 14> state()
    {
        std::array<GLint, 14> result{};
        const std::array<GLenum, 12> names{GL_VERTEX_ARRAY_BINDING, GL_ARRAY_BUFFER_BINDING, GL_ELEMENT_ARRAY_BUFFER_BINDING,
            GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE, GL_PIXEL_UNPACK_BUFFER_BINDING, GL_UNPACK_ALIGNMENT,
            GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_DRAW_FRAMEBUFFER_BINDING,
            GL_READ_FRAMEBUFFER_BINDING};
        for (size_t index = 0; index < names.size(); ++index) m_gl.glGetIntegerv(names[index], &result[index]);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &result[12]);
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &result[13]);
        m_gl.glActiveTexture(result[4]);
        return result;
    }

private slots:
    void cleanup()
    {
        CTreeManage::ReleaseInstance();
    }

    void initTestCase()
    {
        QVERIFY2(!QImage(":/awesomeface.png").isNull(), "Rotation origin texture is missing from the test resources.");
        QVERIFY2(!QImage(":/read.png").isNull(), "Rotation direction texture is missing from the test resources.");
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(0);
        m_context.setFormat(format);
        if (!m_context.create() || !m_context.isValid()) QSKIP("OpenGL 3.3 Core context is unavailable.");
        const auto actual = m_context.format();
        if (actual.profile() != QSurfaceFormat::CoreProfile || actual.majorVersion() < 3 ||
            (actual.majorVersion() == 3 && actual.minorVersion() < 3)) QSKIP("OpenGL 3.3 Core context is unavailable.");
        m_surface.setFormat(actual);
        m_surface.create();
        if (!m_surface.isValid() || !m_context.makeCurrent(&m_surface) || !m_gl.initializeOpenGLFunctions())
            QSKIP("OpenGL 3.3 Core functions are unavailable.");
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void preparedGeometryCanOutliveItsTreeAndSwapWithoutChangingTheOldPreview()
    {
        static_assert(!std::is_copy_constructible_v<Overlays>);
        static_assert(noexcept(std::declval<Overlays&>().Swap(std::declval<Overlays&>())));
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot before, after;
        QVERIFY(makeSnapshot(before));
        QVERIFY(makeSnapshot(after, 0.5f));
        Overlays active, candidate;
        QString error;
        QVERIFY2(active.Prepare(before, active, &error), qPrintable(error));
        auto* warp = dynamic_cast<CDrawWarp*>(active.Find("Warp"));
        QVERIFY(warp);
        warp->SetColor(QVector3D(1, 0.25f, 0.5f));
        active.Show({"Warp"});
        QVERIFY(warp->IsShown());
        QOpenGLFramebufferObject framebuffer(256, 256);
        QVERIFY(framebuffer.isValid());
        const QImage original = draw(active, framebuffer);
        QVERIFY(lineNear(original, 32, 96));
        QVERIFY(!lineNear(original, 96, 96));
        QVERIFY2(candidate.Prepare(after, active, &error), qPrintable(error));
        QCOMPARE(active.Find("Warp"), warp);
        auto* preparedWarp = dynamic_cast<CDrawWarp*>(candidate.Find("Warp"));
        QVERIFY(preparedWarp && preparedWarp != warp);
        QCOMPARE(preparedWarp->Color(), warp->Color());
        QVERIFY(preparedWarp->IsShown());
        before.tree.reset();
        after.tree.reset();
        QCOMPARE(draw(active, framebuffer), original);
        const QImage changed = draw(candidate, framebuffer);
        QVERIFY(!lineNear(changed, 32, 96));
        QVERIFY(lineNear(changed, 96, 96));
        active.Swap(candidate);
        QCOMPARE(active.Find("Warp"), preparedWarp);
        QCOMPARE(candidate.Find("Warp"), warp);
        QCOMPARE(draw(active, framebuffer), changed);
        QCOMPARE(draw(candidate, framebuffer), original);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void prepareRestoresHostileStateWithoutDrawing()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot snapshot;
        QVERIFY(makeSnapshot(snapshot));
        QOpenGLFramebufferObject framebuffer(256, 256);
        QVERIFY(framebuffer.isValid());
        Overlays empty, prepared;
        const QImage before = draw(empty, framebuffer);
        QOpenGLShaderProgram program;
        QVERIFY(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 330 core\nvoid main(){ gl_Position=vec4(0); }"));
        QVERIFY(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 330 core\nout vec4 color; void main(){ color=vec4(1); }"));
        QVERIFY(program.link());
        QVERIFY(program.bind());
        GLuint vao = 0;
        std::array<GLuint, 3> buffers{};
        std::array<GLuint, 2> textures{};
        m_gl.glGenVertexArrays(1, &vao);
        m_gl.glGenBuffers(3, buffers.data());
        m_gl.glGenTextures(2, textures.data());
        const auto cleanup = qScopeGuard([&] {
            m_gl.glBindVertexArray(0);
            m_gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
            m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            m_gl.glDeleteTextures(2, textures.data());
            m_gl.glDeleteBuffers(3, buffers.data());
            m_gl.glDeleteVertexArrays(1, &vao);
        });
        m_gl.glBindVertexArray(vao);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        m_gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffers[2]);
        m_gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, 128, nullptr, GL_STATIC_DRAW);
        m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 9);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, textures[0]);
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, textures[1]);
        const auto saved = state();
        QString error;
        QVERIFY2(prepared.Prepare(snapshot, empty, &error), qPrintable(error));
        QCOMPARE(state(), saved);
        QCOMPARE(framebuffer.toImage(), before);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void failurePreservesExistingObjectsAndUnsupportedChainsStayHidden()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot snapshot;
        QVERIFY(makeSnapshot(snapshot));
        Overlays active, candidate;
        QVERIFY(active.Prepare(snapshot, active));
        dynamic_cast<CDrawWarp*>(active.Find("Warp"))->SetColor(QVector3D(0.25f, 1, 0.5f));
        active.Show({"Warp", "Child"});
        QVERIFY(candidate.Prepare(snapshot, active));
        auto* original = candidate.Find("Warp");
        const auto count = candidate.Count();
        QString error;
        CTreeManage::Snapshot missing;
        QVERIFY(!candidate.Prepare(missing, active, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(candidate.Find("Warp"), original);
        QCOMPARE(candidate.Count(), count);
        m_context.doneCurrent();
        CDrawWarp noContextWarp;
        CDrawRotation noContextRotation;
        QVERIFY(!candidate.Prepare(snapshot, active, &error));
        QCOMPARE(candidate.Find("Warp"), original);
        QVERIFY(m_context.makeCurrent(&m_surface));
        snapshot.unsupportedDeformers = {"Warp", "Child"};
        QVERIFY(candidate.Prepare(snapshot, active));
        QVERIFY(!candidate.Find("Warp"));
        QVERIFY(!candidate.Find("Child"));
        QVERIFY(candidate.Find("Root"));
        snapshot.unsupportedDeformers.clear();
        QVERIFY(active.Prepare(snapshot, candidate));
        QVERIFY(dynamic_cast<CDrawWarp*>(active.Find("Warp"))->IsShown());
        QCOMPARE(dynamic_cast<CDrawWarp*>(active.Find("Warp"))->Color(), QVector3D(0.25f, 1, 0.5f));
        QVERIFY(dynamic_cast<CDrawRotation*>(active.Find("Child"))->IsShown());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void foreignContextCannotReplacePreparedResources()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot snapshot;
        QVERIFY(makeSnapshot(snapshot));
        Overlays active, candidate;
        QVERIFY(active.Prepare(snapshot, active));
        QVERIFY(candidate.Prepare(snapshot, active));
        auto* original = candidate.Find("Warp");
        QOpenGLContext foreign;
        foreign.setFormat(m_context.format());
        QVERIFY(foreign.create());
        QOffscreenSurface foreignSurface;
        foreignSurface.setFormat(foreign.format());
        foreignSurface.create();
        QVERIFY(foreignSurface.isValid());
        QVERIFY(foreign.makeCurrent(&foreignSurface));
        const auto restore = qScopeGuard([&] { m_context.makeCurrent(&m_surface); });
        QString error;
        QVERIFY(!candidate.Prepare(snapshot, active, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(candidate.Find("Warp"), original);
    }

    void unsupportedReflectionInterpolationHidesOnlyThatChainAndCanRecover()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        Fixture fixture;
        fixture.BindRoot(true);
        fixture.column(fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 2, -1);
        CTreeManage::Snapshot snapshot;
        QString error;
        QVERIFY2(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0.5f}}, snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.unsupportedDeformers, (QSet<QString>{"Root", "Warp"}));
        Overlays active, candidate;
        active.Show({"Root", "Warp", "Child"});
        QVERIFY2(candidate.Prepare(snapshot, active, &error), qPrintable(error));
        QCOMPARE(candidate.Count(), qsizetype(1));
        QVERIFY(!candidate.Find("Root"));
        QVERIFY(!candidate.Find("Warp"));
        auto* child = dynamic_cast<CDrawRotation*>(candidate.Find("Child"));
        QVERIFY(child && child->IsShown());
        QOpenGLFramebufferObject framebuffer(256, 256);
        QVERIFY(framebuffer.isValid());
        const QImage supportedImage = draw(candidate, framebuffer);
        QVERIFY(lineNear(supportedImage, 160, 160));
        snapshot.tree->SetParaValue("P", 0);
        QVERIFY(CTreeManage::EvaluateSnapshot(snapshot, &error));
        QVERIFY(snapshot.unsupportedDeformers.isEmpty());
        QVERIFY2(active.Prepare(snapshot, candidate, &error), qPrintable(error));
        QCOMPARE(active.Count(), qsizetype(3));
        QVERIFY(dynamic_cast<CDrawWarp*>(active.Find("Warp"))->IsShown());
        QVERIFY(dynamic_cast<CDrawRotation*>(active.Find("Root"))->IsShown());
        snapshot.tree->SetParaValue("P", 0.5f);
        QVERIFY(CTreeManage::EvaluateSnapshot(snapshot, &error));
        QVERIFY2(candidate.Prepare(snapshot, active, &error), qPrintable(error));
        QCOMPARE(candidate.Count(), qsizetype(1));
        QCOMPARE(draw(candidate, framebuffer), supportedImage);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void runtimeFrameSyncUpdatesCoordinatesWithoutRecreatingOverlays()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot snapshot;
        QVERIFY(makeAnimatedSnapshot(snapshot));
        const auto digest = snapshot.MocDigest();
        CTreeManage::SwapSnapshot(snapshot);
        Overlays active;
        QVERIFY(active.Prepare(CTreeManage::CurrentSnapshot(), active));
        active.Show({"Warp"});
        auto* warp = active.Find("Warp");
        QVERIFY(warp);
        QOpenGLFramebufferObject framebuffer(256, 256);
        QVERIFY(framebuffer.isValid());
        const auto revision = CTreeManage::CurrentSnapshot().PoseRevision();
        QString error;
        QVERIFY2(active.Synchronize(digest, {{"P", 0}}, &error), qPrintable(error));
        const QImage first = draw(active, framebuffer);
        QVERIFY(lineNear(first, 32, 96));
        QVERIFY(!lineNear(first, 96, 96));
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision);
        QVERIFY2(active.Synchronize(digest, {{"P", 1}}, &error), qPrintable(error));
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision + 1);
        QCOMPARE(active.Find("Warp"), warp);
        const QImage second = draw(active, framebuffer);
        QVERIFY(!lineNear(second, 32, 96));
        QVERIFY(lineNear(second, 96, 96));
        QVERIFY(active.Synchronize(digest, {{"P", 1}}, &error));
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision + 1);
        QCOMPARE(active.Find("Warp"), warp);
        QCOMPARE(draw(active, framebuffer), second);

        QVERIFY(!active.Synchronize(digest, {{"wrong", 0}}, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision + 1);
        QCOMPARE(CTreeManage::CurrentSnapshot().Pose(), (QMap<QString, float>{{"P", 1}}));
        QCOMPARE(foregroundCount(draw(active, framebuffer)), 0);
        QVERIFY(active.Synchronize(digest, {{"P", 1}}, &error));
        QCOMPARE(draw(active, framebuffer), second);
        QVERIFY(!active.Synchronize(QByteArray(32, 'x'), {{"P", 0}}, &error));
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision + 1);
        QCOMPARE(foregroundCount(draw(active, framebuffer)), 0);
        m_context.doneCurrent();
        QVERIFY(!active.Synchronize(digest, {{"P", 1}}, &error));
        QVERIFY(m_context.makeCurrent(&m_surface));
        QVERIFY(active.Synchronize(digest, {{"P", 1}}, &error));
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), revision + 1);
        QCOMPARE(draw(active, framebuffer), second);
        QCOMPARE(active.Find("Warp"), warp);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void runtimeFrameSyncHidesAndRestoresDynamicallyUnsupportedGeometry()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CTreeManage::Snapshot snapshot;
        QVERIFY(makeAnimatedSnapshot(snapshot, true));
        const auto digest = snapshot.MocDigest();
        CTreeManage::SwapSnapshot(snapshot);
        Overlays active;
        QVERIFY(active.Prepare(CTreeManage::CurrentSnapshot(), active));
        active.Show({"Warp"});
        auto* warp = dynamic_cast<CDrawWarp*>(active.Find("Warp"));
        QVERIFY(warp && warp->IsShown());
        QOpenGLFramebufferObject framebuffer(256, 256);
        QVERIFY(framebuffer.isValid());
        const QImage first = draw(active, framebuffer);
        QVERIFY(foregroundCount(first) > 100);
        QString error;
        QVERIFY2(active.Synchronize(digest, {{"P", 0.5f}}, &error), qPrintable(error));
        QCOMPARE(CTreeManage::UnsupportedDeformers(), (QSet<QString>{"Root", "Warp", "Child"}));
        QVERIFY(!warp->IsShown());
        QCOMPARE(foregroundCount(draw(active, framebuffer)), 0);
        active.Show({"Warp"});
        QVERIFY(active.Synchronize(digest, {{"P", 0.5f}}, &error));
        QVERIFY(!warp->IsShown());
        QCOMPARE(foregroundCount(draw(active, framebuffer)), 0);
        QVERIFY(active.Synchronize(digest, {{"P", 0}}, &error));
        QVERIFY(CTreeManage::UnsupportedDeformers().isEmpty());
        QVERIFY(warp->IsShown());
        QCOMPARE(active.Find("Warp"), warp);
        QCOMPARE(draw(active, framebuffer), first);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }
};

QTEST_MAIN(DeformerOverlaySnapshotTests)
#include "deformer_overlay_snapshot_tests.moc"
