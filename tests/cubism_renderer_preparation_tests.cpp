#include "tests/cubism_model_fixture.h"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"

#include <QtTest>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QScopeGuard>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <unordered_set>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;
using CubismModelFixture::GetterState;
namespace Core = Live2D::Cubism::Core;

namespace {
std::unique_ptr<CubismIdManager> ids;
std::unordered_set<void*> allocations;

void* Allocate(csmSizeType size)
{
    void* memory = std::malloc(size);
    if (!memory) throw std::bad_alloc();
    allocations.insert(memory);
    return memory;
}

void Release(void* memory)
{
    if (!memory) return;
    if (allocations.erase(memory) != 1) qFatal("Unknown SDK allocation in renderer preparation test");
    std::free(memory);
}

[[noreturn]] void UnexpectedRuntime()
{
    qFatal("Renderer preparation must not access Delegate, draw, or export a PSD");
}

using ModelOwner = std::unique_ptr<CubismModel, void(*)(CubismModel*)>;
using RendererOwner = std::unique_ptr<CubismRenderer, void(*)(CubismRenderer*)>;
CubismRenderer_OpenGLES2& Gl(RendererOwner& renderer)
{
    return *static_cast<CubismRenderer_OpenGLES2*>(renderer.get());
}
}

void* operator new(csmSizeType size, CubismAllocationTag) { return Allocate(size); }
void operator delete(void* memory, CubismAllocationTag) { Release(memory); }

namespace Live2D { namespace Cubism { namespace Framework {
namespace Constant {
const csmInt32 VertexOffset = 0;
const csmInt32 VertexStep = 2;
}
void* CubismFramework::Allocate(csmSizeType size) { return ::Allocate(size); }
void CubismFramework::Deallocate(void* memory) { Release(memory); }
void CubismFramework::DeallocateAligned(void* memory)
{ ::operator delete(memory, std::align_val_t(Core::csmAlignofModel)); }
CubismIdManager* CubismFramework::GetIdManager() { return ids.get(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
}}}

LAppDelegate* LAppDelegate::GetInstance() { UnexpectedRuntime(); }
const QWidget* LAppDelegate::GetWindow() { UnexpectedRuntime(); }
QOpenGLContext* LAppDelegate::getGlOwningContext() { UnexpectedRuntime(); }
bool LAppDelegate::IsOffScreen() { UnexpectedRuntime(); }
void LAppDelegate::SetIsOffset(bool) { UnexpectedRuntime(); }
CPsdWriteList* LAppDelegate::GetCPsdWriteList() { UnexpectedRuntime(); }
bool CReadOpengl::PreDraw() { UnexpectedRuntime(); }
bool CReadOpengl::UpdateFrame() { UnexpectedRuntime(); }
bool CReadOpengl::GetScreenNode(const float*, int, const float*, int, int, QImage&, QRect&) { UnexpectedRuntime(); }
void CPsdWriteList::AddGroupStart(const QString&) { UnexpectedRuntime(); }
void CPsdWriteList::AddGroupEnd() { UnexpectedRuntime(); }
void CPsdWriteList::AddLayer(const QString&, const QRect&, const QImage&, uint8_t, uint32_t, bool, bool) { UnexpectedRuntime(); }
uint32_t CPsdWriteList::CubismBlendModeToPsd(CubismRenderer::CubismBlendMode) { UnexpectedRuntime(); }
QVector<QPair<int, CPsdWriteList::LayerType>> CPsdWriteList::GetSortDrawableIndexList(const QVector<int>&, CubismModel*)
{ UnexpectedRuntime(); }

class CubismRendererPreparationTests : public QObject
{
    Q_OBJECT
    QOpenGLContext m_context;
    QOffscreenSurface m_surface;
    QOpenGLFunctions_3_3_Core m_gl;

    ModelOwner Model(GetterState& state)
    {
        void* address = ::operator new(Core::csmAlignofModel, std::align_val_t(Core::csmAlignofModel));
        ModelOwner model(new CubismModel(CubismModelFixture::AttachModel(address, &state)),
                         [](CubismModel* model) { delete model; });
        model->Initialize();
        return model;
    }

    RendererOwner Renderer()
    {
        return RendererOwner(CubismRenderer::Create(), &CubismRenderer::Delete);
    }

    GLuint Texture()
    {
        GLuint texture = 0;
        m_gl.glGenTextures(1, &texture);
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        const std::array<uchar, 4> pixel{17, 34, 51, 255};
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return texture;
    }

    std::array<GLint, 19> State()
    {
        std::array<GLint, 19> values{};
        constexpr GLenum queries[]{GL_VERTEX_ARRAY_BINDING, GL_ARRAY_BUFFER_BINDING, GL_CURRENT_PROGRAM,
            GL_ACTIVE_TEXTURE, GL_READ_FRAMEBUFFER_BINDING, GL_DRAW_FRAMEBUFFER_BINDING,
            GL_PIXEL_UNPACK_BUFFER_BINDING, GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_FRONT_FACE, GL_CULL_FACE_MODE};
        for (size_t index = 0; index < std::size(queries); ++index) m_gl.glGetIntegerv(queries[index], &values[index]);
        m_gl.glGetIntegerv(GL_VIEWPORT, values.data() + 11);
        for (int unit = 0; unit < 3; ++unit) {
            m_gl.glActiveTexture(GL_TEXTURE0 + (unit == 2 ? 7 : unit));
            m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &values[15 + unit]);
        }
        m_gl.glActiveTexture(values[3]);
        values[18] = m_gl.glIsEnabled(GL_SCISSOR_TEST);
        return values;
    }

    std::array<uchar, 4> MaskPixel(COffscreenBuffer& mask)
    {
        GLint previous = 0, readBuffer = 0;
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, mask.GetRenderTexture());
        m_gl.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        m_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        std::array<uchar, 4> value{};
        m_gl.glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value.data());
        m_gl.glReadBuffer(readBuffer);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
        return value;
    }

private slots:
    void initTestCase()
    {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        m_context.setFormat(format);
        QVERIFY(m_context.create());
        m_surface.setFormat(m_context.format());
        m_surface.create();
        QVERIFY(m_surface.isValid());
        QVERIFY(m_context.makeCurrent(&m_surface));
        QVERIFY(m_gl.initializeOpenGLFunctions());
        ids = std::make_unique<CubismIdManager>();
    }

    void init()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        while (m_gl.glGetError() != GL_NO_ERROR) {}
    }

    void independentResourcesCopySettingsAndUploadCandidateVertices()
    {
        GetterState oldState, newState;
        newState.positions[0][0].X = -0.25f;
        auto oldModel = Model(oldState), candidateModel = Model(newState);
        auto previous = Renderer(), candidate = Renderer();
        auto& before = Gl(previous);
        QVERIFY(before.setGlOwningContext(&m_context));
        QVERIFY(before.Initialize(oldModel.get(), 2));
        before.SetClippingMaskBufferSize(32, 48);
        QCOMPARE(before.GetClippingMaskBufferSize().X, 32.0f);
        QCOMPARE(before.GetClippingMaskBufferSize().Y, 48.0f);
        const GLuint firstTexture = Texture(), secondTexture = Texture();
        const auto deleteTextures = qScopeGuard([&] {
            m_gl.glDeleteTextures(1, &firstTexture);
            m_gl.glDeleteTextures(1, &secondTexture);
        });
        before.BindTexture(3, firstTexture);
        before.BindTexture(11, secondTexture);
        before.SetModelColor(0.25f, 0.5f, 0.75f, 0.5f);
        before.IsPremultipliedAlpha(true);
        before.IsCulling(true);
        before.SetAnisotropy(2.0f);
        before.UseHighPrecisionMask(true);
        CubismMatrix44 matrix;
        matrix.Scale(0.25f, 0.5f);
        matrix.Translate(0.125f, -0.25f);
        before.SetMvpMatrix(&matrix);
        std::array<GLuint, 2> oldBuffers{before.GetMaskBuffer(0)->GetRenderTexture(), before.GetMaskBuffer(1)->GetRenderTexture()};

        QOpenGLFramebufferObject read(8, 8), draw(8, 8);
        QVERIFY(read.isValid());
        QVERIFY(draw.isValid());
        GLuint vao = 0, buffer = 0, unpack = 0;
        m_gl.glGenVertexArrays(1, &vao);
        m_gl.glGenBuffers(1, &buffer);
        m_gl.glGenBuffers(1, &unpack);
        const auto deleteSentinels = qScopeGuard([&] {
            m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_gl.glDeleteBuffers(1, &unpack);
            m_gl.glDeleteBuffers(1, &buffer);
            m_gl.glDeleteVertexArrays(1, &vao);
        });
        m_gl.glBindVertexArray(vao);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, read.handle());
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.handle());
        m_gl.glActiveTexture(GL_TEXTURE0);
        m_gl.glBindTexture(GL_TEXTURE_2D, firstTexture);
        m_gl.glActiveTexture(GL_TEXTURE1);
        m_gl.glBindTexture(GL_TEXTURE_2D, secondTexture);
        m_gl.glActiveTexture(GL_TEXTURE7);
        m_gl.glBindTexture(GL_TEXTURE_2D, firstTexture);
        m_gl.glViewport(2, 3, 4, 5);
        m_gl.glEnable(GL_SCISSOR_TEST);
        m_gl.glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA);
        m_gl.glFrontFace(GL_CW);
        m_gl.glCullFace(GL_FRONT);
        const auto saved = State();
        QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(candidateModel.get(), before, &error), qPrintable(error));
        QCOMPARE(State(), saved);
        QCOMPARE(oldState.updates, 0);
        QCOMPARE(newState.updates, 0);
        QCOMPARE(before.GetModel(), oldModel.get());
        QCOMPARE(Gl(candidate).GetModel(), candidateModel.get());
        QCOMPARE(Gl(candidate).GetRenderTextureCount(), 2);
        QCOMPARE(Gl(candidate).GetClippingMaskBufferSize().X, 32.0f);
        QCOMPARE(Gl(candidate).GetClippingMaskBufferSize().Y, 48.0f);
        QVERIFY(Gl(candidate).IsPremultipliedAlpha());
        QVERIFY(Gl(candidate).IsCulling());
        QVERIFY(Gl(candidate).IsUsingHighPrecisionMask());
        QCOMPARE(Gl(candidate).GetAnisotropy(), 2.0f);
        const auto color = Gl(candidate).GetModelColor();
        QCOMPARE(color.R, 0.25f); QCOMPARE(color.G, 0.5f); QCOMPARE(color.B, 0.75f); QCOMPARE(color.A, 0.5f);
        for (int index = 0; index < 16; ++index) QCOMPARE(Gl(candidate).GetMvpMatrix().GetArray()[index], matrix.GetArray()[index]);
        const auto& copied = Gl(candidate).GetBindedTextures();
        QCOMPARE(copied.GetSize(), 2);
        QCOMPARE(copied[3], firstTexture);
        QCOMPARE(copied[11], secondTexture);
        QCOMPARE(copied[999], GLuint(0));
        QCOMPARE(copied.GetSize(), 2);
        QCOMPARE(before.GetBindedTextures().GetSize(), 2);
        for (int index = 0; index < 2; ++index) {
            const auto* mask = Gl(candidate).GetMaskBuffer(index);
            QVERIFY(mask && mask->IsValid());
            QCOMPARE(mask->GetBufferWidth(), 32);
            QCOMPARE(mask->GetBufferHeight(), 48);
            QVERIFY(mask->GetRenderTexture() != oldBuffers[index]);
            QVERIFY(m_gl.glIsFramebuffer(oldBuffers[index]));
        }
        const auto* uploaded = Gl(candidate)._meshBuffers[0].get();
        QVERIFY(uploaded);
        m_gl.glBindVertexArray(uploaded->GetVertexArray());
        GLint vertices = 0, indices = 0;
        m_gl.glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vertices);
        m_gl.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &indices);
        QVERIFY(vertices != 0 && indices != 0);
        std::array<float, 16> packed{};
        std::array<quint16, 6> elements{};
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, vertices);
        m_gl.glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(packed), packed.data());
        m_gl.glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(elements), elements.data());
        QCOMPARE(packed[0], -0.25f);
        QCOMPARE(packed[1], -1.0f);
        QCOMPARE(packed[2], 0.0f);
        QCOMPARE(packed[3], 0.0f);
        QVERIFY(elements == newState.indices[0]);
        const auto candidateMask = Gl(candidate).GetMaskBuffer(0)->GetRenderTexture();
        const auto candidateVao = uploaded->GetVertexArray();
        m_gl.glBindVertexArray(vao);
        candidate.reset();
        QVERIFY(!m_gl.glIsFramebuffer(candidateMask));
        QVERIFY(!m_gl.glIsVertexArray(candidateVao));
        QVERIFY(m_gl.glIsFramebuffer(oldBuffers[0]));
        QVERIFY(m_gl.glIsTexture(firstTexture));
        QVERIFY(m_gl.glIsTexture(secondTexture));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void geometryFailuresPreservePreviousResources_data()
    {
        QTest::addColumn<QString>("failure");
        for (const char* name : {"nan-position", "null-position", "null-uv", "null-indices", "bad-index",
                                 "incomplete-triangle", "negative-vertices", "empty-with-indices", "bad-mask",
                                 "null-mask", "null-vertex-counts", "null-position-array", "prior-gl-error"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void geometryFailuresPreservePreviousResources()
    {
        QFETCH(QString, failure);
        GetterState oldState, newState;
        auto oldModel = Model(oldState), newModel = Model(newState);
        auto previous = Renderer(), candidate = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));
        QVERIFY(Gl(previous).Initialize(oldModel.get()));
        QVERIFY(Gl(previous).UploadDrawableGeometry(*oldModel, 0, oldModel->GetDrawableVertices(0),
                                                    Gl(previous)._meshBuffers[0], nullptr));
        const GLuint mask = Gl(previous).GetMaskBuffer(0)->GetRenderTexture();
        const GLuint vao = Gl(previous)._meshBuffers[0]->GetVertexArray();
        if (failure == "nan-position") newState.positions[1][0].X = std::numeric_limits<float>::quiet_NaN();
        else if (failure == "null-position") newState.positionPointers[1] = nullptr;
        else if (failure == "null-uv") newState.uvPointers[1] = nullptr;
        else if (failure == "null-indices") newState.indexPointers[1] = nullptr;
        else if (failure == "bad-index") newState.indices[1][0] = 99;
        else if (failure == "incomplete-triangle") newState.indexCounts[1] = 4;
        else if (failure == "negative-vertices") newState.vertexCounts[1] = -1;
        else if (failure == "empty-with-indices") newState.vertexCounts[1] = 0;
        else if (failure == "bad-mask") newState.masks[1][0] = 2;
        else if (failure == "null-mask") newState.maskPointers[1] = nullptr;
        else if (failure == "null-vertex-counts") newState.nullGetters.insert("csmGetDrawableVertexCounts");
        else if (failure == "null-position-array") newState.nullGetters.insert("csmGetDrawableVertexPositions");
        const auto state = State();
        if (failure == "prior-gl-error") m_gl.glEnable(0xffffffff);
        QString error;
        QVERIFY(!Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(State(), state);
        candidate.reset();
        QCOMPARE(Gl(previous).GetModel(), oldModel.get());
        QVERIFY(m_gl.glIsFramebuffer(mask));
        QVERIFY(m_gl.glIsVertexArray(vao));
        QCOMPARE(oldState.updates, 0);
        QCOMPARE(newState.updates, 0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void emptyGeometrySkipsBuffersAndWrongContextsFail()
    {
        GetterState oldState, newState;
        oldState.maskCounts = {0, 0};
        newState.maskCounts = {0, 0};
        newState.vertexCounts[0] = 0;
        newState.indexCounts = {0, 0};
        newState.positionPointers[0] = nullptr;
        newState.uvPointers[0] = nullptr;
        auto oldModel = Model(oldState), newModel = Model(newState);
        auto previous = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));
        QVERIFY(Gl(previous).Initialize(oldModel.get()));
        {
            auto candidate = Renderer();
            QVERIFY(Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous)));
            QCOMPARE(Gl(candidate).GetRenderTextureCount(), 0);
            QVERIFY(!Gl(candidate)._meshBuffers[0]);
            QVERIFY(!Gl(candidate)._meshBuffers[1]);
            QVERIFY(!Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous)));
        }
        auto candidate = Renderer();
        m_context.doneCurrent();
        QVERIFY(!Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous)));
        QOpenGLContext foreign;
        foreign.setFormat(m_context.format());
        QVERIFY(foreign.create());
        QVERIFY(foreign.makeCurrent(&m_surface));
        QVERIFY(!Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous)));
        QVERIFY(m_context.makeCurrent(&m_surface));
        QVERIFY(Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous)));
        QCOMPARE(newState.updates, 0);
    }

    void ordinaryInitializationDoesNotReadUnevaluatedGeometry_data()
    {
        QTest::addColumn<QString>("geometry");
        QTest::newRow("null-position") << QString("null-position");
        QTest::newRow("nan-position") << QString("nan-position");
        QTest::newRow("null-geometry-arrays") << QString("null-geometry-arrays");
    }

    void ordinaryInitializationDoesNotReadUnevaluatedGeometry()
    {
        QFETCH(QString, geometry);
        GetterState state;
        auto model = Model(state);
        if (geometry == "null-position") state.positionPointers[0] = nullptr;
        else if (geometry == "nan-position") state.positions[0][0].X = std::numeric_limits<float>::quiet_NaN();
        else {
            state.nullGetters.insert("csmGetDrawableVertexCounts");
            state.nullGetters.insert("csmGetDrawableIndexCounts");
            state.nullGetters.insert("csmGetDrawableVertexPositions");
            state.nullGetters.insert("csmGetDrawableVertexUvs");
            state.nullGetters.insert("csmGetDrawableIndices");
        }
        auto renderer = Renderer();
        QVERIFY(Gl(renderer).setGlOwningContext(&m_context));
        QVERIFY(Gl(renderer).Initialize(model.get()));
        QCOMPARE(Gl(renderer)._meshBuffers.size(), size_t(2));
        QVERIFY(!Gl(renderer)._meshBuffers[0]);
        QVERIFY(!Gl(renderer)._meshBuffers[1]);
        QCOMPARE(state.updates, 0);
        auto candidate = Renderer();
        QString error;
        QVERIFY(!Gl(candidate).PrepareModelReplacement(model.get(), Gl(renderer), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(state.updates, 0);
        QCOMPARE(Gl(renderer).GetModel(), model.get());
        QVERIFY(Gl(renderer).GetMaskBuffer(0)->IsValid());
    }

    void maskResizeFailureAndBoundResourcesKeepPrevious()
    {
        GetterState state;
        auto model = Model(state);
        auto renderer = Renderer();
        QVERIFY(Gl(renderer).setGlOwningContext(&m_context));
        QVERIFY(Gl(renderer).Initialize(model.get(), 2));
        Gl(renderer).SetClippingMaskBufferSize(32, 48);
        const auto mask = Gl(renderer).GetMaskBuffer(0)->GetRenderTexture();
        GLint maximum = 0;
        m_gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        Gl(renderer).SetClippingMaskBufferSize(float(maximum) + 1.0f, 48);
        QCOMPARE(Gl(renderer).GetMaskBuffer(0)->GetRenderTexture(), mask);
        QCOMPARE(Gl(renderer).GetClippingMaskBufferSize().X, 32.0f);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, mask);
        Gl(renderer).SetClippingMaskBufferSize(64, 64);
        QCOMPARE(Gl(renderer).GetMaskBuffer(0)->GetRenderTexture(), mask);
        GLint read = 0;
        m_gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        QCOMPARE(GLuint(read), mask);
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        Gl(renderer).SetClippingMaskBufferSize(64, 64);
        QVERIFY(Gl(renderer).GetMaskBuffer(0)->GetRenderTexture() != mask);
        QCOMPARE(Gl(renderer).GetMaskBuffer(0)->GetBufferWidth(), 64);
        QCOMPARE(Gl(renderer).GetMaskBuffer(1)->GetBufferHeight(), 64);
        QVERIFY(!m_gl.glIsFramebuffer(mask));
    }

    void unavailableMaskVerticesStayGuarded_data()
    {
        QTest::addColumn<bool>("highPrecision");
        QTest::addColumn<bool>("dirty");
        QTest::newRow("regular-dirty") << false << true;
        QTest::newRow("regular-clean") << false << false;
        QTest::newRow("high-precision-dirty") << true << true;
        QTest::newRow("high-precision-clean") << true << false;
    }

    void unavailableMaskVerticesStayGuarded()
    {
        QFETCH(bool, highPrecision);
        QFETCH(bool, dirty);
        GetterState state;
        auto model = Model(state);
        auto renderer = Renderer();
        auto& glRenderer = Gl(renderer);
        QVERIFY(glRenderer.setGlOwningContext(&m_context));
        QVERIFY(glRenderer.Initialize(model.get()));
        glRenderer.SetClippingMaskBufferSize(8, 8);
        glRenderer.UseHighPrecisionMask(highPrecision);
        auto* mask = glRenderer.GetMaskBuffer(0);
        QVERIFY(mask);
        m_gl.glDisable(GL_SCISSOR_TEST);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        QVERIFY(mask->BeginDraw());
        QVERIFY(mask->Clear(0, 0, 0, 0));
        QVERIFY(mask->EndDraw());
        QVERIFY((MaskPixel(*mask) == std::array<uchar, 4>{0, 0, 0, 0}));
        state.dynamicFlags[0] = Core::csmIsVisible;
        state.positionPointers[0] = nullptr;
        glRenderer._forceMaskRedraw = dirty;
        glRenderer._maskTopologyUpdated = false;

        QOpenGLFramebufferObject callerRead(4, 4), callerDraw(4, 4);
        QVERIFY(callerRead.isValid() && callerDraw.isValid());
        m_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, callerRead.handle());
        m_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, callerDraw.handle());
        m_gl.glViewport(1, 2, 3, 4);
        m_gl.glEnable(GL_SCISSOR_TEST);
        m_gl.glScissor(2, 2, 1, 1);
        m_gl.glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        const auto caller = State();
        QVERIFY(glRenderer._rendererProfile.Save(&m_context));
        if (highPrecision) {
            glRenderer._clippingManager->SetupMatrixForHighPrecision(*model, false);
            auto* clipping = (*glRenderer._clippingManager->GetClippingContextListForDraw())[1];
            QVERIFY(clipping && clipping->_isUsing);
            QVERIFY(glRenderer.DrawHighPrecisionMask(clipping));
        } else {
            QVERIFY(glRenderer._clippingManager->SetupClippingContext(*model, &glRenderer,
                        glRenderer._rendererProfile.Viewport().data()));
        }
        QVERIFY(glRenderer._rendererProfile.Restore());
        QCOMPARE(State(), caller);
        GLboolean colorMask[4]{};
        m_gl.glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        QCOMPARE(colorMask[0], GLboolean(GL_FALSE));
        QCOMPARE(colorMask[1], GLboolean(GL_TRUE));
        QCOMPARE(colorMask[2], GLboolean(GL_FALSE));
        QCOMPARE(colorMask[3], GLboolean(GL_TRUE));
        const uchar expected = highPrecision || dirty ? 255 : 0;
        QVERIFY((MaskPixel(*mask) == std::array<uchar, 4>{expected, expected, expected, expected}));
        QVERIFY(!glRenderer._maskTopologyUpdated);
        QVERIFY(!glRenderer._clippingContextBufferForMask);
        QCOMPARE(state.updates, 0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void dirtyRegularMasksClearEveryUsedBufferWithoutDrawing()
    {
        GetterState state;
        state.maskCounts = {1, 1};
        state.masks[0][0] = 1;
        state.masks[1][0] = 0;
        auto model = Model(state);
        auto renderer = Renderer();
        auto& glRenderer = Gl(renderer);
        QVERIFY(glRenderer.setGlOwningContext(&m_context));
        QVERIFY(glRenderer.Initialize(model.get(), 2));
        glRenderer.SetClippingMaskBufferSize(8, 8);
        m_gl.glDisable(GL_SCISSOR_TEST);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (int index = 0; index < 2; ++index) {
            auto* mask = glRenderer.GetMaskBuffer(index);
            QVERIFY(mask && mask->BeginDraw());
            QVERIFY(mask->Clear(0, 0, 0, 0));
            QVERIFY(mask->EndDraw());
        }
        state.dynamicFlags = {Core::csmIsVisible, Core::csmIsVisible};
        glRenderer._forceMaskRedraw = true;
        QVERIFY(glRenderer._rendererProfile.Save(&m_context));
        QVERIFY(glRenderer._clippingManager->SetupClippingContext(*model, &glRenderer,
                    glRenderer._rendererProfile.Viewport().data()));
        QVERIFY(glRenderer._rendererProfile.Restore());
        for (int index = 0; index < 2; ++index)
            QVERIFY((MaskPixel(*glRenderer.GetMaskBuffer(index)) == std::array<uchar, 4>{255, 255, 255, 255}));
        QVERIFY(!glRenderer._maskTopologyUpdated);
        QCOMPARE(state.updates, 0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void cleanupTestCase()
    {
        QVERIFY(m_context.makeCurrent(&m_surface));
        CubismRenderer::StaticRelease();
        ids.reset();
        QVERIFY(allocations.empty());
        m_context.doneCurrent();
    }
};

QTEST_MAIN(CubismRendererPreparationTests)
#include "cubism_renderer_preparation_tests.moc"
