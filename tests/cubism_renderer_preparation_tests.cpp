#include "tests/cubism_model_fixture.h"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "opengl/cpsdrendersession.h"
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
QOpenGLContext* controlledContext = nullptr;
bool allowDraw = false;
bool allowLayerCapture = false;
bool isolatedLayer = false;
QImage capturedFrame, capturedLayer;
bool capturedClipping = true;
bool capturedPremultiplied = false;
uint8_t capturedOpacity = 0;
uint32_t capturedBlendKey = 0;
CubismRenderer::CubismBlendMode capturedBlendMode = CubismRenderer::CubismBlendMode_Normal;
int layerCalls = 0, frameClears = 0, emptyLayerCalls = 0, readCalls = 0;
std::optional<quint32> capturedLayerId;
enum class ReadCapture { Pixels, Empty, MissingImage, Failure };
ReadCapture readCapture = ReadCapture::Pixels;

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

void* LAppAllocator::Allocate(csmSizeType) { UnexpectedRuntime(); }
void LAppAllocator::Deallocate(void*) { UnexpectedRuntime(); }
void* LAppAllocator::AllocateAligned(csmSizeType, csmUint32) { UnexpectedRuntime(); }
void LAppAllocator::DeallocateAligned(void*) { UnexpectedRuntime(); }
LAppDelegate::LAppDelegate() : m_IsPrintALl(false), m_IsPrintFenzu(false) {}
LAppDelegate::~LAppDelegate() = default;
CPsdRenderSession::~CPsdRenderSession() { UnexpectedRuntime(); }
LAppDelegate* LAppDelegate::GetInstance()
{
    if (!allowDraw && !allowLayerCapture) UnexpectedRuntime();
    static LAppDelegate delegate;
    return &delegate;
}
const QWidget* LAppDelegate::GetWindow() { if (!allowDraw) UnexpectedRuntime(); return nullptr; }
QOpenGLContext* LAppDelegate::getGlOwningContext()
{
    if (!allowDraw) UnexpectedRuntime();
    return controlledContext;
}
bool LAppDelegate::IsOffScreen() { if (!allowDraw) UnexpectedRuntime(); return isolatedLayer; }
void LAppDelegate::SetIsOffset(bool) { UnexpectedRuntime(); }
CPsdWriteList::CPsdWriteList() : m_list(nullptr) {}
CPsdWriteList::~CPsdWriteList() = default;
CPsdWriteList* LAppDelegate::GetCPsdWriteList()
{
    if (!allowLayerCapture) UnexpectedRuntime();
    static CPsdWriteList writer;
    return &writer;
}
bool CReadOpengl::PreDraw() { UnexpectedRuntime(); }
bool CReadOpengl::setGlOwningContext(const QOpenGLContext*) { UnexpectedRuntime(); }
bool CReadOpengl::UpdateFrame() { if (!allowLayerCapture) UnexpectedRuntime(); ++frameClears; return true; }
bool CReadOpengl::GetScreenNode(const float*, int, const float*, int width, int height, QImage& image, QRect& rect)
{
    if (!allowLayerCapture || capturedFrame.isNull() || width != capturedFrame.width() || height != capturedFrame.height())
        UnexpectedRuntime();
    ++readCalls;
    if (readCapture == ReadCapture::Failure) return false;
    image = readCapture == ReadCapture::Pixels ? capturedFrame : QImage();
    rect = readCapture == ReadCapture::Empty ? QRect() : capturedFrame.rect();
    return true;
}
bool CPsdWriteList::AddGroupStart(const QString&) { UnexpectedRuntime(); }
bool CPsdWriteList::AddGroupEnd() { UnexpectedRuntime(); }
bool CPsdWriteList::AddLayer(const QString&, const QRect&, const QImage& image, uint8_t opacity, uint32_t blendKey,
                           bool clipping, bool premultiplied, std::optional<quint32> layerId)
{
    if (!allowLayerCapture) UnexpectedRuntime();
    capturedLayer = image;
    capturedLayerId = layerId;
    capturedClipping = clipping;
    capturedPremultiplied = premultiplied;
    capturedOpacity = opacity;
    capturedBlendKey = blendKey;
    ++layerCalls;
    return true;
}
bool CPsdWriteList::AddEmptyLayer(const QString&, quint8 opacity, quint32 blend, bool clipping, std::optional<quint32> layerId)
{
    if (!allowLayerCapture) UnexpectedRuntime();
    capturedLayer = {};
    capturedLayerId = layerId;
    capturedOpacity = opacity;
    capturedBlendKey = blend;
    capturedClipping = clipping;
    capturedPremultiplied = false;
    ++layerCalls;
    ++emptyLayerCalls;
    return true;
}
uint32_t CPsdWriteList::CubismBlendModeToPsd(CubismRenderer::CubismBlendMode mode)
{
    if (!allowLayerCapture) UnexpectedRuntime();
    capturedBlendMode = mode;
    return 0x12345600u + static_cast<uint32_t>(mode);
}
QVector<QPair<int, CPsdWriteList::LayerType>> CPsdWriteList::GetSortDrawableIndexList(const QVector<int>&, CubismModel*)
{ UnexpectedRuntime(); }

namespace {
struct ClipMatchFixture
{
    const csmInt32* _clippingIdList;
    csmInt32 _clippingIdCount;
};

class ClipMatchManager : public CubismClippingManager<ClipMatchFixture, COffscreenBuffer>
{
public:
    ClipMatchFixture* Add(const QVector<int>& values)
    {
        auto* context = CSM_NEW ClipMatchFixture{values.constData(), int(values.size())};
        _clippingContextListForMask.PushBack(context);
        return context;
    }
};
}

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

    GLuint Texture(uchar alpha = 255)
    {
        GLuint texture = 0;
        m_gl.glGenTextures(1, &texture);
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        const std::array<uchar, 4> pixel{255, 255, 255, alpha};
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
    void clippingGroupsMatchReferenceMultiplicity_data()
    {
        QTest::addColumn<QVector<int>>("existing");
        QTest::addColumn<QVector<int>>("requested");
        QTest::addColumn<bool>("reuse");
        QTest::newRow("aab-versus-abb") << QVector<int>{0, 0, 1} << QVector<int>{0, 1, 1} << false;
        QTest::newRow("abb-versus-aab") << QVector<int>{0, 1, 1} << QVector<int>{0, 0, 1} << false;
        QTest::newRow("same-order") << QVector<int>{0, 0, 1} << QVector<int>{0, 0, 1} << true;
        QTest::newRow("same-counts-reordered") << QVector<int>{0, 0, 1} << QVector<int>{1, 0, 0} << true;
        QTest::newRow("unique-reordered") << QVector<int>{0, 1, 2} << QVector<int>{2, 0, 1} << true;
        QTest::newRow("different-unique") << QVector<int>{0, 1, 2} << QVector<int>{0, 1, 3} << false;
        QTest::newRow("different-count") << QVector<int>{0, 0, 1} << QVector<int>{0, 1} << false;
        QTest::newRow("empty") << QVector<int>{} << QVector<int>{} << true;
        QTest::newRow("singleton") << QVector<int>{2} << QVector<int>{2} << true;
    }

    void clippingGroupsMatchReferenceMultiplicity()
    {
        QFETCH(QVector<int>, existing);
        QFETCH(QVector<int>, requested);
        QFETCH(bool, reuse);
        const QVector<int> originalExisting(existing.cbegin(), existing.cend());
        const QVector<int> originalRequested(requested.cbegin(), requested.cend());
        ClipMatchManager manager;
        auto* context = manager.Add(existing);
        QCOMPARE(manager.FindSameClip(requested.constData(), requested.size()), reuse ? context : nullptr);
        QCOMPARE(existing, originalExisting);
        QCOMPARE(requested, originalRequested);
        QCOMPARE(context->_clippingIdList, existing.constData());
        QCOMPARE(context->_clippingIdCount, existing.size());
    }

    void clippingGroupSearchContinuesPastDifferentMultiplicity()
    {
        const QVector<int> aab{0, 0, 1}, abb{0, 1, 1}, reordered{1, 0, 1};
        ClipMatchManager manager;
        manager.Add(aab);
        auto* expected = manager.Add(abb);
        QCOMPARE(manager.FindSameClip(reordered.constData(), reordered.size()), expected);
    }

    void maskMultiplicityChangesCoverage_data()
    {
        QTest::addColumn<bool>("high");
        QTest::addColumn<bool>("inverted");
        for (bool high : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(high).arg(inverted))) << high << inverted;
    }

    void maskMultiplicityChangesCoverage()
    {
        QFETCH(bool, high);
        QFETCH(bool, inverted);
        const std::array<GLuint, 3> textures{Texture(64), Texture(128), Texture()};
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        QVector<int> alphas;
        for (const QVector<int>& masks : {QVector<int>{0, 0, 1}, QVector<int>{0, 1, 1}}) {
            GetterState state;
            state.drawableCount = 3;
            state.flags.fill(Core::csmIsDoubleSided);
            if (inverted) state.flags[2] |= Core::csmIsInvertedMask;
            state.maskCounts = {0, 0, 3};
            state.maskPointers[2] = masks.constData();
            state.dynamicFlags.fill(Core::csmIsVisible);
            state.drawableOpacities.fill(1);
            state.multiply.fill({1, 1, 1, 1});
            state.screen.fill({0, 0, 0, 0});
            auto model = Model(state);
            auto renderer = Renderer();
            auto& target = Gl(renderer);
            QVERIFY(target.setGlOwningContext(&m_context));
            QVERIFY(target.Initialize(model.get()));
            target.UseHighPrecisionMask(high);
            target.SetClippingMaskBufferSize(64, 64);
            for (int slot = 0; slot < 3; ++slot) target.BindTexture(slot, textures[slot]);
            auto* context = (*target._clippingManager->GetClippingContextListForDraw())[2];
            QCOMPARE(context->_clippingIdCount, 3);
            QCOMPARE(context->_clippingIdList, masks.constData());
            QOpenGLFramebufferObject framebuffer(64, 64);
            QVERIFY(framebuffer.isValid() && framebuffer.bind());
            m_gl.glViewport(0, 0, 64, 64);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDisable(GL_DITHER);
            m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_gl.glClearColor(0, 0, 0, 0);
            m_gl.glClear(GL_COLOR_BUFFER_BIT);
            CubismRenderer_OpenGLES2::SnapshotScene scene;
            scene.hiddenDrawables = {"MeshA", "MeshB"};
            QString error;
            QVERIFY2(target.DrawSnapshotModel(scene, &error), qPrintable(error));
            const int actual = qAlpha(framebuffer.toImage().pixel(32, 32));
            double transmission = 1;
            for (int source : masks) transmission *= 1.0 - (source == 0 ? 64.0 : 128.0) / 255;
            const int expected = qRound(255 * (inverted ? transmission : 1 - transmission));
            QVERIFY2(std::abs(actual - expected) <= 1, qPrintable(QString("actual %1 expected %2").arg(actual).arg(expected)));
            alphas.append(actual);
            QCOMPARE(state.updates, 0);
            const QVector<int> other = masks[1] == 0 ? QVector<int>{0, 1, 1} : QVector<int>{0, 0, 1};
            QVERIFY(!target._clippingManager->FindSameClip(other.constData(), other.size()));
        }
        QVERIFY(std::abs(alphas[0] - alphas[1]) >= 20);
    }

    void snapshotScenePreservesBlendAndMasksWithoutDelegate_data()
    {
        QTest::addColumn<int>("blend"); QTest::addColumn<bool>("high"); QTest::addColumn<bool>("inverted");
        for (int blend = 0; blend < 3; ++blend) for (bool high : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(blend).arg(high).arg(inverted))) << blend << high << inverted;
    }
    void snapshotScenePreservesBlendAndMasksWithoutDelegate()
    {
        QFETCH(int, blend); QFETCH(bool, high); QFETCH(bool, inverted);
        GetterState state; state.drawableCount = 3; state.flags.fill(Core::csmIsDoubleSided);
        if (blend == 1) state.flags[2] |= Core::csmBlendAdditive;
        if (blend == 2) state.flags[2] |= Core::csmBlendMultiplicative;
        if (inverted) state.flags[2] |= Core::csmIsInvertedMask;
        state.maskCounts = {0, 0, 1}; state.masks[2] = {0, 0};
        state.dynamicFlags.fill(Core::csmIsVisible); state.drawableOpacities.fill(.9f);
        state.multiply[2] = {.8f, .6f, .4f, 1}; state.screen[2] = {.1f, .2f, .3f, 1};
        auto model = Model(state); auto renderer = Renderer(); auto& target = Gl(renderer);
        QVERIFY(target.setGlOwningContext(&m_context)); QVERIFY(target.Initialize(model.get()));
        target.UseHighPrecisionMask(high); target.SetClippingMaskBufferSize(32, 32); target.SetModelColor(.6f, .8f, .4f, .5f);
        CubismMatrix44 matrix; matrix.LoadIdentity(); target.SetMvpMatrix(&matrix);
        const std::array<GLuint, 3> textures{Texture(64), Texture(), Texture()};
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        const std::array<uchar, 4> pixel{128, 64, 192, 128};
        m_gl.glBindTexture(GL_TEXTURE_2D, textures[2]); m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        for (int slot = 0; slot < 3; ++slot) target.BindTexture(slot, textures[slot]);
        CubismRenderer_OpenGLES2::SnapshotScene scene; scene.hiddenDrawables = {"MeshA", "MeshB"}; scene.opacity["MeshC"] = .6f;
        allowDraw = true; auto* delegate = LAppDelegate::GetInstance(); delegate->m_hiddenDrawables = {"MeshC"};
        delegate->m_isDraggingDeformer = true; delegate->m_savedVertices[2] = {999}; delegate->m_IsPrintALl = true;
        const auto restoreDelegate = qScopeGuard([&] {
            allowDraw = true; delegate->m_hiddenDrawables.clear(); delegate->m_isDraggingDeformer = false;
            delegate->m_savedVertices.clear(); delegate->m_IsPrintALl = false; allowDraw = false;
        });
        allowDraw = false;
        QOpenGLFramebufferObject framebuffer(64, 64); QVERIFY(framebuffer.isValid());
        const std::array<float, 3> background{40.f / 255, 91.f / 255, 133.f / 255};
        for (int maskAlpha : {64, 192}) {
            const std::array<uchar, 4> mask{255, 255, 255, uchar(maskAlpha)};
            m_gl.glBindTexture(GL_TEXTURE_2D, textures[0]); m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, mask.data());
            QVERIFY(framebuffer.bind()); m_gl.glViewport(0, 0, 64, 64); m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDisable(GL_DITHER); m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_gl.glClearColor(background[0], background[1], background[2], 1); m_gl.glClear(GL_COLOR_BUFFER_BIT);
            const auto before = State(); QString error;
            QVERIFY2(target.DrawSnapshotModel(scene, &error), qPrintable(error)); QCOMPARE(State(), before);
            const auto image = framebuffer.toImage(); const QRgb actual = image.pixel(32, 32);
            const double coverage = inverted ? 1 - maskAlpha / 255. : maskAlpha / 255.;
            const double alpha = 128. / 255 * .6 * .5 * coverage;
            const std::array<double, 3> multiply{.8, .6, .4}, screen{.1, .2, .3}, rgb{.6, .8, .4};
            const int channels[]{qRed(actual), qGreen(actual), qBlue(actual)};
            for (int channel = 0; channel < 3; ++channel) {
                const double multiplied = pixel[channel] / 255. * multiply[channel];
                const double color = (multiplied + screen[channel] - multiplied * screen[channel]) * rgb[channel];
                const double expected = blend == 0 ? color * alpha + background[channel] * (1 - alpha)
                    : blend == 1 ? color * alpha + background[channel] : background[channel] * (color * alpha + 1 - alpha);
                QVERIFY2(std::abs(channels[channel] - qRound(expected * 255)) <= 3, qPrintable(QString("blend %1 mask %2 channel %3 actual %4 expected %5").arg(blend).arg(maskAlpha).arg(channel).arg(channels[channel]).arg(expected * 255)));
            }
            QCOMPARE(qAlpha(actual), 255); QCOMPARE(state.updates, 0); QVERIFY(!target._snapshotScene);
        }
        scene.opacity["MeshC"] = std::numeric_limits<float>::quiet_NaN(); const auto before = State();
        QVERIFY(!target.DrawSnapshotModel(scene)); QCOMPARE(State(), before); QVERIFY(!target._snapshotScene);
    }

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
        controlledContext = &m_context;
        allowDraw = allowLayerCapture = isolatedLayer = false;
        capturedFrame = capturedLayer = {};
        capturedClipping = true;
        layerCalls = frameClears = emptyLayerCalls = readCalls = 0;
        capturedLayerId.reset(); readCapture = ReadCapture::Pixels;
        while (m_gl.glGetError() != GL_NO_ERROR) {}
    }

    void legacyPsdLayerRetainsEmptyIdentityAndRejectsInvalidInput_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"pixels", "empty", "no-triangles", "no-vertices", "missing-image", "read-failure",
                                 "bad-index", "negative-count", "incomplete-indices", "nan-opacity", "invalid-drawable"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void legacyPsdLayerRetainsEmptyIdentityAndRejectsInvalidInput()
    {
        QFETCH(QString, mode);
        GetterState state; state.drawableCount = 3; state.maskCounts = {0, 0, 0};
        state.flags.fill(Core::csmIsDoubleSided | Core::csmBlendAdditive);
        auto model = Model(state); auto renderer = Renderer(); auto& gl = Gl(renderer);
        QVERIFY(gl.setGlOwningContext(&m_context)); QVERIFY(gl.Initialize(model.get()));
        gl.SetModelColor(1, 1, 1, 0.5f);
        m_gl.glViewport(0, 0, 4, 4); QVERIFY(gl._rendererProfile.Save(&m_context));
        capturedFrame = QImage(4, 4, QImage::Format_RGBA8888); capturedFrame.fill(qRgba(13, 29, 41, 91));
        if (mode == "empty" || mode == "no-vertices") readCapture = ReadCapture::Empty;
        if (mode == "missing-image") readCapture = ReadCapture::MissingImage;
        if (mode == "read-failure") readCapture = ReadCapture::Failure;
        if (mode == "no-triangles" || mode == "no-vertices") state.indexCounts[2] = 0;
        if (mode == "no-vertices") { state.vertexCounts[2] = 0; state.positionPointers[2] = nullptr; }
        if (mode == "bad-index") state.indices[2][0] = 99;
        if (mode == "negative-count") state.indexCounts[2] = -1;
        if (mode == "incomplete-indices") state.indexCounts[2] = 4;
        const float opacity = mode == "nan-opacity" ? std::numeric_limits<float>::quiet_NaN() : 0.25f;
        allowLayerCapture = true;
        const bool accepted = gl.WritePsdLayer(mode == "invalid-drawable" ? -1 : 2, "identity", opacity, true);
        const bool expected = mode == "pixels" || mode == "empty" || mode == "no-triangles" || mode == "no-vertices";
        QCOMPARE(accepted, expected);
        QCOMPARE(layerCalls, expected ? 1 : 0); QCOMPARE(frameClears, expected ? 1 : 0);
        if (expected) {
            QCOMPARE(readCalls, 1); QCOMPARE(capturedLayerId, std::optional<quint32>(3));
            QCOMPARE(capturedOpacity, quint8(31)); QCOMPARE(capturedBlendMode, CubismRenderer::CubismBlendMode_Additive);
            QCOMPARE(capturedBlendKey, 0x12345601u); QVERIFY(!capturedClipping);
            QCOMPARE(emptyLayerCalls, mode == "pixels" ? 0 : 1);
            if (mode == "pixels") { QCOMPARE(capturedLayer, capturedFrame); QVERIFY(capturedPremultiplied); }
            else QVERIFY(capturedLayer.isNull());
        }
        QVERIFY(gl._rendererProfile.Restore());
    }

    void replacementTextureDirectoryIsExactAndTransactional()
    {
        GetterState before, after;
        auto source = Model(before), target = Model(after);
        auto previous = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));
        QVERIFY(Gl(previous).Initialize(source.get()));
        const std::array<GLuint, 3> textures{Texture(64), Texture(128), Texture(255)};
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        Gl(previous).BindTexture(0, textures[0]);
        Gl(previous).BindTexture(3, textures[1]);
        const auto beforeState = State();
        const QMap<int, GLuint> bindings{{0, textures[2]}, {7, textures[2]}};
        auto candidate = Renderer();
        QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(target.get(), Gl(previous), &error, &bindings), qPrintable(error));
        QCOMPARE(State(), beforeState);
        QCOMPARE(Gl(candidate).GetBindedTextures().GetSize(), 2);
        for (auto it = Gl(candidate).GetBindedTextures().Begin(); it != Gl(candidate).GetBindedTextures().End(); ++it) {
            QVERIFY(bindings.contains(it->First));
            QCOMPARE(it->Second, bindings[it->First]);
        }
        QCOMPARE(Gl(previous).GetBindedTextures().GetSize(), 2);
        QCOMPARE(Gl(previous).GetBindedTextures()[0], textures[0]);
        QCOMPARE(Gl(previous).GetBindedTextures()[3], textures[1]);
        QVERIFY(Gl(candidate)._textureMaskReplay);
        QVERIFY(Gl(candidate)._maskBuffersNeedRedraw);
        for (const auto& invalid : {QMap<int, GLuint>{{-1, textures[0]}}, QMap<int, GLuint>{{0, 0}},
                                    QMap<int, GLuint>{{0, 0x7ffffffe}}}) {
            auto rejected = Renderer();
            QVERIFY(!Gl(rejected).PrepareModelReplacement(target.get(), Gl(previous), &error, &invalid));
            QVERIFY(!error.isEmpty());
            QCOMPARE(State(), beforeState);
            for (auto id : textures) QVERIFY(m_gl.glIsTexture(id));
        }
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void clippingTransitionsRetainConfigurationAndComposeAlpha_data()
    {
        QTest::addColumn<bool>("highPrecision");
        QTest::newRow("packed") << false;
        QTest::newRow("high-precision") << true;
    }

    void clippingTransitionsRetainConfigurationAndComposeAlpha()
    {
        QFETCH(bool, highPrecision);
        const std::array<GLuint, 3> textures{Texture(64), Texture(128), Texture(255)};
        const auto deleteTextures = qScopeGuard([&] { m_gl.glDeleteTextures(3, textures.data()); });
        const auto definition = [](int masks, bool inverted) {
            auto state = std::make_unique<GetterState>();
            state->drawableCount = 3;
            state->flags.fill(Core::csmIsDoubleSided);
            if (inverted) state->flags[2] |= Core::csmIsInvertedMask;
            state->maskCounts = {0, 0, masks};
            state->masks[2] = {0, 1};
            state->drawableOpacities = {0.01f, 0.02f, 1};
            state->multiply.fill({1, 1, 1, 1});
            state->screen.fill({0, 0, 0, 1});
            state->positions[1] = state->positions[0];
            state->dynamicFlags.fill(Core::csmIsVisible | Core::csmVertexPositionsDidChange);
            return state;
        };
        auto state = definition(1, false);
        auto model = Model(*state);
        auto renderer = Renderer();
        QVERIFY(Gl(renderer).setGlOwningContext(&m_context));
        QVERIFY(Gl(renderer).Initialize(model.get(), 2));
        Gl(renderer).SetClippingMaskBufferSize(32, 48);
        Gl(renderer).UseHighPrecisionMask(highPrecision);
        for (int index = 0; index < 3; ++index) Gl(renderer).BindTexture(index, textures[index]);
        CubismMatrix44 identity;
        identity.LoadIdentity();
        Gl(renderer).SetMvpMatrix(&identity);
        QOpenGLFramebufferObject framebuffer(64, 64);
        QVERIFY(framebuffer.isValid());
        const std::array<std::pair<int, bool>, 6> edits{{{0, false}, {1, false}, {2, false}, {2, true}, {0, true}, {2, false}}};
        for (const auto& edit : edits) {
            auto nextState = definition(edit.first, edit.second);
            auto nextModel = Model(*nextState);
            QVERIFY(nextModel->CopyRuntimeStateFrom(*model, CubismModelUpdatePolicy::AllowClippingEdits));
            auto candidate = Renderer();
            allowDraw = allowLayerCapture = false;
            const auto previousState = State();
            const auto oldMask = Gl(renderer).GetMaskBuffer(0) ? Gl(renderer).GetMaskBuffer(0)->GetRenderTexture() : 0;
            QString error;
            QVERIFY2(Gl(candidate).PrepareModelReplacement(nextModel.get(), Gl(renderer), &error), qPrintable(error));
            QCOMPARE(State(), previousState);
            QCOMPARE(Gl(candidate).GetRenderTextureCount(), edit.first ? 2 : 0);
            QCOMPARE(Gl(candidate)._requestedMaskBufferCount, 2);
            QCOMPARE(Gl(candidate)._requestedMaskBufferSize.X, 32.0f);
            QCOMPARE(Gl(candidate)._requestedMaskBufferSize.Y, 48.0f);
            QCOMPARE(Gl(candidate).IsUsingHighPrecisionMask(), highPrecision);
            if (edit.first) {
                QCOMPARE(Gl(candidate).GetMaskBuffer(0)->GetBufferWidth(), 32);
                QCOMPARE(Gl(candidate).GetMaskBuffer(0)->GetBufferHeight(), 48);
                QVERIFY(Gl(candidate).GetMaskBuffer(0)->GetRenderTexture() != oldMask);
                auto* clipping = (*Gl(candidate)._clippingManager->GetClippingContextListForDraw())[2];
                QVERIFY(clipping);
                QCOMPARE(clipping->_clippingIdCount, edit.first);
                for (int index = 0; index < edit.first; ++index) QCOMPARE(clipping->_clippingIdList[index], index);
            } else QVERIFY(!Gl(candidate)._clippingManager);
            if (oldMask) QVERIFY(m_gl.glIsFramebuffer(oldMask));
            for (int index = 0; index < 3; ++index) QCOMPARE(Gl(candidate).GetBindedTextures()[index], textures[index]);

            renderer.swap(candidate);
            model.swap(nextModel);
            state.swap(nextState);
            allowDraw = true;
            QVERIFY(framebuffer.bind());
            m_gl.glViewport(0, 0, 64, 64);
            m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDisable(GL_DEPTH_TEST);
            m_gl.glDisable(GL_STENCIL_TEST);
            m_gl.glClearColor(0, 0, 0, 0);
            m_gl.glClear(GL_COLOR_BUFFER_BIT);
            QVERIFY(Gl(renderer)._rendererProfile.Save(&m_context));
            auto* clipping = edit.first ? (*Gl(renderer)._clippingManager->GetClippingContextListForDraw())[2] : nullptr;
            if (clipping) {
                if (highPrecision) {
                    Gl(renderer)._clippingManager->SetupMatrixForHighPrecision(*model, false);
                    QVERIFY(Gl(renderer).DrawHighPrecisionMask(clipping));
                } else QVERIFY(Gl(renderer)._clippingManager->SetupClippingContext(*model, &Gl(renderer),
                                        Gl(renderer)._rendererProfile.Viewport().data()));
            }
            Gl(renderer).PreDraw();
            Gl(renderer).SetClippingContextBufferForDraw(clipping);
            QVERIFY(Gl(renderer).DrawMeshOpenGL(*model, 2, false));
            const double unionAlpha = edit.first == 1 ? 64.0 / 255 : 1.0 - (1.0 - 64.0 / 255) * (1.0 - 128.0 / 255);
            const int expected = qRound(255 * (edit.first == 0 ? 1.0 : edit.second ? 1.0 - unionAlpha : unionAlpha));
            capturedFrame = framebuffer.toImage();
            QVERIFY(qAbs(qAlpha(capturedFrame.pixel(32, 32)) - expected) <= 2);
            allowLayerCapture = true;
            const int previousCalls = layerCalls;
            QVERIFY(Gl(renderer).WritePsdLayer(2, "Target", 1, edit.first != 0));
            QCOMPARE(layerCalls, previousCalls + 1);
            QCOMPARE(frameClears, layerCalls);
            QVERIFY(!capturedClipping);
            QVERIFY(capturedPremultiplied);
            QCOMPARE(capturedLayer, capturedFrame);
            QVERIFY(Gl(renderer)._rendererProfile.Restore());
            QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        }
        allowDraw = allowLayerCapture = false;
    }

    void materialMappingRedrawsStableMasks_data()
    {
        QTest::addColumn<bool>("high"); QTest::addColumn<bool>("inverted"); QTest::addColumn<int>("mapping");
        for (bool high : {false, true}) for (bool inverted : {false, true}) for (int mapping = 0; mapping < 4; ++mapping)
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(high).arg(inverted).arg(mapping))) << high << inverted << mapping;
    }
    void materialMappingRedrawsStableMasks()
    {
        QFETCH(bool, high); QFETCH(bool, inverted); QFETCH(int, mapping);
        const bool slot = mapping == 3;
        const float u = mapping == 0 ? .75f : mapping == 1 ? -.25f : mapping == 2 ? 3.25f : .25f;
        const std::array<GLuint, 4> textures{Texture(), Texture(), Texture(), Texture(224)};
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        const std::array<uchar, 8> pixels{255,255,255,64, 255,255,255,192};
        m_gl.glBindTexture(GL_TEXTURE_2D, textures[0]);
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const auto configure = [&](GetterState& state) {
            state.drawableCount = 3; state.flags.fill(Core::csmIsDoubleSided);
            if (inverted) state.flags[2] |= Core::csmIsInvertedMask;
            state.maskCounts = {0, 0, 1}; state.masks[2] = {0, 1};
            state.drawableOpacities.fill(1); state.multiply.fill({1,1,1,1}); state.screen.fill({0,0,0,0});
            state.dynamicFlags.fill(Core::csmIsVisible);
            for (auto& uv : state.uvs[0]) uv.X = .25f;
        };
        GetterState before, after; configure(before); configure(after);
        if (slot) after.textures[0] = 3;
        else for (auto& uv : after.uvs[0]) uv.X = u;
        auto source = Model(before), candidate = Model(after);
        QVERIFY(candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowDrawableMaterialEdits));
        auto previous = Renderer(), renderer = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context)); QVERIFY(Gl(previous).Initialize(source.get()));
        Gl(previous).UseHighPrecisionMask(high); Gl(previous).SetClippingMaskBufferSize(32, 32);
        for (int index = 0; index < 4; ++index) Gl(previous).BindTexture(index, textures[index]);
        CubismMatrix44 identity; identity.LoadIdentity(); Gl(previous).SetMvpMatrix(&identity);
        const auto oldMask = Gl(previous).GetMaskBuffer(0)->GetRenderTexture();
        const auto caller = State(); QString error;
        QVERIFY2(Gl(renderer).PrepareModelReplacement(candidate.get(), Gl(previous), &error), qPrintable(error));
        QCOMPARE(State(), caller); QVERIFY(m_gl.glIsFramebuffer(oldMask));
        auto& gl = Gl(renderer);
        QVERIFY(!gl.MaskSourceAvailable(0));
        gl.InvalidateTextureMapping();
        QVERIFY(gl.MaskSourceAvailable(0));
        for (int index = 0; index < 4; ++index) QCOMPARE(gl.GetBindedTextures()[index], textures[index]);
        const auto vao = gl._meshBuffers[0]->GetVertexArray();
        m_gl.glBindVertexArray(vao); GLint buffer = 0;
        m_gl.glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        m_gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
        std::array<float, 16> packed{};
        m_gl.glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(packed), packed.data());
        for (int vertex = 0; vertex < 4; ++vertex) QCOMPARE(packed[vertex * 4 + 2], u);
        m_gl.glBindVertexArray(caller[0]); m_gl.glBindBuffer(GL_ARRAY_BUFFER, caller[1]);
        QOpenGLFramebufferObject framebuffer(64, 64); QVERIFY(framebuffer.isValid()); allowDraw = true;
        for (int pass = 0; pass < 3; ++pass) {
            QVERIFY(framebuffer.bind()); m_gl.glViewport(0,0,64,64); m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); m_gl.glClearColor(0,0,0,0); m_gl.glClear(GL_COLOR_BUFFER_BIT);
            QVERIFY(gl._rendererProfile.Save(&m_context));
            auto* clipping = (*gl._clippingManager->GetClippingContextListForDraw())[2]; QVERIFY(clipping);
            if (high) { gl._clippingManager->SetupMatrixForHighPrecision(*candidate, false); QVERIFY(gl.DrawHighPrecisionMask(clipping)); }
            else { gl._forceMaskRedraw = gl._maskBuffersNeedRedraw; QVERIFY(gl._clippingManager->SetupClippingContext(*candidate,&gl,gl._rendererProfile.Viewport().data())); }
            gl.PreDraw(); gl.SetClippingContextBufferForDraw(clipping); QVERIFY(gl.DrawMeshOpenGL(*candidate,2,false));
            const auto image = framebuffer.toImage(); const int alpha = slot ? 224 : mapping == 2 ? 64 : 192;
            QVERIFY2(qAbs(qAlpha(image.pixel(32,32)) - (inverted ? 255-alpha : alpha)) <= 2,
                qPrintable(QString("pass %1 alpha %2").arg(pass).arg(qAlpha(image.pixel(32,32)))));
            allowLayerCapture = true; capturedFrame = image;
            QVERIFY(gl.WritePsdLayer(2, "Target", 1, true)); QCOMPARE(capturedLayer, image);
            QVERIFY(gl._rendererProfile.Restore()); QCOMPARE(after.updates, 0);
        }
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        allowDraw = allowLayerCapture = false;
    }

    void textureAlphaReplacementRedrawsStableMaskVertices_data()
    {
        QTest::addColumn<bool>("high"); QTest::addColumn<bool>("inverted");
        for (bool high : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(high).arg(inverted))) << high << inverted;
    }
    void textureAlphaReplacementRedrawsStableMaskVertices()
    {
        QFETCH(bool, high); QFETCH(bool, inverted);
        GetterState state; state.drawableCount = 3; state.flags.fill(Core::csmIsDoubleSided);
        if (inverted) state.flags[2] |= Core::csmIsInvertedMask;
        state.maskCounts = {0, 0, 1}; state.masks[2] = {0, 1};
        state.drawableOpacities = {1, 1, 1}; state.multiply.fill({1,1,1,1}); state.screen.fill({0,0,0,0});
        state.dynamicFlags.fill(Core::csmIsVisible | Core::csmVertexPositionsDidChange);
        auto model = Model(state); auto renderer = Renderer(); auto& gl = Gl(renderer);
        QVERIFY(gl.setGlOwningContext(&m_context)); QVERIFY(gl.Initialize(model.get()));
        gl.UseHighPrecisionMask(high); gl.SetClippingMaskBufferSize(32, 32);
        const std::array<GLuint, 4> textures{Texture(64), Texture(255), Texture(255), Texture(192)};
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(textures.size(), textures.data()); });
        for (int i = 0; i < 3; ++i) gl.BindTexture(i, textures[i]);
        CubismMatrix44 identity; identity.LoadIdentity(); gl.SetMvpMatrix(&identity);
        QOpenGLFramebufferObject framebuffer(64,64); QVERIFY(framebuffer.isValid()); allowDraw = true;
        for (int pass = 0; pass < 4; ++pass) {
            if (pass == 1) { state.dynamicFlags.fill(Core::csmIsVisible); gl.BindTexture(0, textures[3]); }
            QVERIFY(framebuffer.bind()); m_gl.glViewport(0,0,64,64); m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); m_gl.glClearColor(0,0,0,0); m_gl.glClear(GL_COLOR_BUFFER_BIT);
            QVERIFY(gl._rendererProfile.Save(&m_context));
            auto* clipping = (*gl._clippingManager->GetClippingContextListForDraw())[2]; QVERIFY(clipping);
            if (high) { gl._clippingManager->SetupMatrixForHighPrecision(*model, false); QVERIFY(gl.DrawHighPrecisionMask(clipping)); }
            else { gl._forceMaskRedraw = gl._maskBuffersNeedRedraw; QVERIFY(gl._clippingManager->SetupClippingContext(*model,&gl,gl._rendererProfile.Viewport().data())); }
            gl.PreDraw(); gl.SetClippingContextBufferForDraw(clipping); QVERIFY(gl.DrawMeshOpenGL(*model,2,false));
            const auto image = framebuffer.toImage(); const int alpha = pass ? 192 : 64;
            QVERIFY2(qAbs(qAlpha(image.pixel(32,32)) - (inverted ? 255-alpha : alpha)) <= 2, qPrintable(QString("pass %1 alpha %2").arg(pass).arg(qAlpha(image.pixel(32,32)))));
            QVERIFY(gl._rendererProfile.Restore()); QCOMPARE(state.updates, 0);
        }
        allowDraw = false;
    }

    void isolatedPsdLayersKeepOpacityAndBlendInMetadata_data()
    {
        QTest::addColumn<int>("blend");
        QTest::addColumn<int>("textureAlpha");
        QTest::addColumn<int>("opacityCase");
        QTest::addColumn<float>("modelAlpha");
        QTest::addColumn<int>("mask");
        QTest::addColumn<bool>("premultipliedTexture");
        for (int blend = 0; blend < 3; ++blend)
            for (int textureAlpha : {128, 255})
                for (int opacityCase = 0; opacityCase < 4; ++opacityCase)
                    for (float modelAlpha : {0.5f, 1.0f})
                        for (int mask = 0; mask < 3; ++mask)
                            QTest::newRow(qPrintable(QString("blend%1-alpha%2-opacity%3-model%4-mask%5")
                                .arg(blend).arg(textureAlpha).arg(opacityCase).arg(modelAlpha).arg(mask)))
                                << blend << textureAlpha << opacityCase << modelAlpha << mask << false;
        for (int blend = 0; blend < 3; ++blend)
            QTest::newRow(qPrintable(QString("premultiplied-texture-blend%1").arg(blend)))
                << blend << 128 << 0 << 0.5f << 2 << true;
        for (int blend = 0; blend < 3; ++blend)
            for (bool premultiplied : {false, true})
                for (int opacityCase : {0, 2})
                    QTest::newRow(qPrintable(QString("zero-global-blend%1-premult%2-opacity%3")
                        .arg(blend).arg(premultiplied).arg(opacityCase)))
                        << blend << (premultiplied ? 128 : 255) << opacityCase << 0.0f << 0 << premultiplied;
    }

    void isolatedPsdLayersKeepOpacityAndBlendInMetadata()
    {
        QFETCH(int, blend);
        QFETCH(int, textureAlpha);
        QFETCH(int, opacityCase);
        QFETCH(float, modelAlpha);
        QFETCH(int, mask);
        QFETCH(bool, premultipliedTexture);
        GetterState state;
        state.drawableCount = 3;
        state.maskCounts = {0, 0, mask ? 2 : 0};
        state.flags.fill(Core::csmIsDoubleSided);
        if (blend == 1) state.flags[2] |= Core::csmBlendAdditive;
        if (blend == 2) state.flags[2] |= Core::csmBlendMultiplicative;
        if (mask == 2) state.flags[2] |= Core::csmIsInvertedMask;
        state.positions[1] = state.positions[0];
        state.dynamicFlags.fill(Core::csmIsVisible | Core::csmVertexPositionsDidChange);
        state.drawableOpacities = {0.02f, 0.03f, opacityCase >= 2 ? 0.0f : 0.5f};
        state.multiply[2] = {0.8f, 0.6f, 0.4f, 1};
        state.screen[2] = {0.1f, 0.2f, 0.3f, 1};
        auto model = Model(state);
        auto renderer = Renderer();
        QVERIFY(Gl(renderer).setGlOwningContext(&m_context));
        QVERIFY(Gl(renderer).Initialize(model.get()));
        Gl(renderer).SetClippingMaskBufferSize(32, 32);
        Gl(renderer).SetModelColor(0.6f, 0.8f, 0.4f, modelAlpha);
        Gl(renderer).IsPremultipliedAlpha(premultipliedTexture);
        CubismMatrix44 matrix;
        matrix.LoadIdentity();
        Gl(renderer).SetMvpMatrix(&matrix);
        std::array<GLuint, 3> textures{Texture(64), Texture(128), Texture(static_cast<uchar>(textureAlpha))};
        const auto releaseTextures = qScopeGuard([&] { m_gl.glDeleteTextures(3, textures.data()); });
        std::array<uchar, 4> texel{128, 64, 192, static_cast<uchar>(textureAlpha)};
        if (premultipliedTexture) for (int channel = 0; channel < 3; ++channel)
            texel[channel] = static_cast<uchar>(qRound(texel[channel] * textureAlpha / 255.0));
        m_gl.glBindTexture(GL_TEXTURE_2D, textures[2]);
        m_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
        for (int index = 0; index < 3; ++index) Gl(renderer).BindTexture(index, textures[index]);
        QOpenGLFramebufferObject framebuffer(64, 64);
        QVERIFY(framebuffer.isValid());
        QVERIFY(framebuffer.bind());
        m_gl.glViewport(0, 0, 64, 64);
        m_gl.glDisable(GL_SCISSOR_TEST);
        m_gl.glDisable(GL_DEPTH_TEST);
        m_gl.glDisable(GL_STENCIL_TEST);
        m_gl.glDisable(GL_DITHER);
        m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_gl.glClearColor(0, 0, 0, 0);
        m_gl.glClear(GL_COLOR_BUFFER_BIT);
        allowDraw = true;
        isolatedLayer = true;
        QVERIFY(Gl(renderer)._rendererProfile.Save(&m_context));
        auto* clipping = mask ? (*Gl(renderer)._clippingManager->GetClippingContextListForDraw())[2] : nullptr;
        if (clipping) QVERIFY(Gl(renderer)._clippingManager->SetupClippingContext(*model, &Gl(renderer),
                                                   Gl(renderer)._rendererProfile.Viewport().data()));
        Gl(renderer).PreDraw();
        Gl(renderer).SetClippingContextBufferForDraw(clipping);
        const float overrideOpacity = opacityCase == 1 ? 0.25f : -1;
        const bool isAll = opacityCase == 2;
        LAppDelegate::GetInstance()->m_IsPrintALl = isAll;
        QVERIFY(Gl(renderer).DrawMeshOpenGL(*model, 2, isAll, overrideOpacity));
        std::array<uchar, 4> actual{};
        m_gl.glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        const double unionCoverage = 1 - (1 - 64.0 / 255) * (1 - 128.0 / 255);
        const double coverage = mask == 0 ? 1 : mask == 1 ? unionCoverage : 1 - unionCoverage;
        const double pixelAlpha = textureAlpha / 255.0 * coverage;
        std::array<double, 3> color{};
        const std::array<double, 3> multiply{0.8, 0.6, 0.4}, screen{0.1, 0.2, 0.3}, tint{0.6, 0.8, 0.4};
        for (int channel = 0; channel < 3; ++channel) {
            const double textureColor = premultipliedTexture ? double(texel[channel]) / textureAlpha : double(texel[channel]) / 255;
            const double multiplied = textureColor * multiply[channel];
            color[channel] = (multiplied + screen[channel] - multiplied * screen[channel]) * tint[channel];
            QVERIFY(qAbs(int(actual[channel]) - qRound(color[channel] * pixelAlpha * 255)) <= 3);
        }
        QVERIFY(qAbs(int(actual[3]) - qRound(pixelAlpha * 255)) <= 2);
        capturedFrame = framebuffer.toImage();
        allowLayerCapture = true;
        QVERIFY(Gl(renderer).WritePsdLayer(2, "Target", overrideOpacity, clipping != nullptr));
        const double effectiveOpacity = opacityCase == 0 ? 0.5 : opacityCase == 1 ? 0.25 : opacityCase == 2 ? 1 : 0;
        QCOMPARE(capturedOpacity, static_cast<uint8_t>(effectiveOpacity * modelAlpha * 255));
        QCOMPARE(capturedBlendMode, static_cast<CubismRenderer::CubismBlendMode>(blend));
        QCOMPARE(capturedBlendKey, 0x12345600u + static_cast<uint32_t>(blend));
        QVERIFY(capturedPremultiplied);
        QVERIFY(!capturedClipping);
        QCOMPARE(capturedLayer, capturedFrame);
        QVERIFY(Gl(renderer)._rendererProfile.Restore());

        isolatedLayer = false;
        QVERIFY(framebuffer.bind());
        m_gl.glViewport(0, 0, 64, 64);
        m_gl.glClearColor(26.0f / 255, 51.0f / 255, 77.0f / 255, 102.0f / 255);
        m_gl.glClear(GL_COLOR_BUFFER_BIT);
        Gl(renderer).PreDraw();
        Gl(renderer).SetClippingContextBufferForDraw(clipping);
        QVERIFY(Gl(renderer).DrawMeshOpenGL(*model, 2, isAll, overrideOpacity));
        m_gl.glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        const double sourceAlpha = pixelAlpha * effectiveOpacity * modelAlpha;
        const std::array<double, 3> background{26.0 / 255, 51.0 / 255, 77.0 / 255};
        for (int channel = 0; channel < 3; ++channel) {
            const double sourceColor = color[channel] * sourceAlpha;
            const double expected = blend == 0 ? sourceColor + background[channel] * (1 - sourceAlpha) :
                blend == 1 ? sourceColor + background[channel] : sourceColor * background[channel] + background[channel] * (1 - sourceAlpha);
            QVERIFY(qAbs(int(actual[channel]) - qRound(expected * 255)) <= 3);
        }
        const double expectedAlpha = blend == 0 ? sourceAlpha + 102.0 / 255 * (1 - sourceAlpha) : 102.0 / 255;
        QVERIFY(qAbs(int(actual[3]) - qRound(expectedAlpha * 255)) <= 2);
        if (modelAlpha == 0) {
            m_gl.glClearColor(0, 0, 0, 0);
            m_gl.glClear(GL_COLOR_BUFFER_BIT);
            Gl(renderer).SetClippingContextBufferForDraw(clipping);
            QVERIFY(Gl(renderer).DrawMeshOpenGL(*model, 2, isAll, overrideOpacity));
            m_gl.glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
            QVERIFY((actual == std::array<uchar, 4>{0, 0, 0, 0}));
            QVERIFY(qAlpha(capturedLayer.pixel(32, 32)) > 0);
            QVERIFY(qRed(capturedLayer.pixel(32, 32)) > 0);
            QCOMPARE(capturedOpacity, uint8_t(0));
        }
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        LAppDelegate::GetInstance()->m_IsPrintALl = false;
        allowDraw = allowLayerCapture = false;
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

    void changedVertexCountsAndUvsAreUploadedBeforePublishing_data()
    {
        QTest::addColumn<int>("count");QTest::newRow("remove-vertex")<<3;QTest::newRow("add-vertex")<<5;
    }
    void changedVertexCountsAndUvsAreUploadedBeforePublishing()
    {
        QFETCH(int,count);GetterState oldState,newState;
        std::array<Core::csmVector2,5> positions{{{-.8f,-.7f},{.8f,-.7f},{.8f,.7f},{-.8f,.7f},{0,.2f}}};
        std::array<Core::csmVector2,5> uvs{{{.2f,.3f},{.8f,.3f},{.8f,.7f},{.2f,.7f},{.5f,.5f}}};
        newState.vertexCounts[0]=count;newState.positionPointers[0]=positions.data();newState.uvPointers[0]=uvs.data();
        newState.indexCounts[0]=count==3?3:6;newState.indices[0]={0,1,2,0,2,4};
        auto previousModel=Model(oldState),nextModel=Model(newState);auto previous=Renderer(),candidate=Renderer();
        QVERIFY(nextModel->CopyRuntimeStateFrom(*previousModel,CubismModelUpdatePolicy::AllowDrawableGeometryEdits));
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));QVERIFY(Gl(previous).Initialize(previousModel.get()));
        QVERIFY(Gl(previous).UploadDrawableGeometry(*previousModel,0,previousModel->GetDrawableVertices(0),Gl(previous)._meshBuffers[0],nullptr));
        const auto oldVao=Gl(previous)._meshBuffers[0]->GetVertexArray();const auto glState=State();QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(nextModel.get(),Gl(previous),&error),qPrintable(error));QCOMPARE(State(),glState);
        const auto vao=Gl(candidate)._meshBuffers[0]->GetVertexArray();QVERIFY(vao!=oldVao);QVERIFY(m_gl.glIsVertexArray(oldVao));
        m_gl.glBindVertexArray(vao);GLint buffer=0;m_gl.glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buffer);m_gl.glBindBuffer(GL_ARRAY_BUFFER,buffer);
        std::array<float,20> actual{};m_gl.glGetBufferSubData(GL_ARRAY_BUFFER,0,count*4*sizeof(float),actual.data());
        for(int i=0;i<count;++i){QCOMPARE(actual[i*4],positions[i].X);QCOMPARE(actual[i*4+1],positions[i].Y);QCOMPARE(actual[i*4+2],uvs[i].X);QCOMPARE(actual[i*4+3],uvs[i].Y);}
        std::array<quint16,6> indices{};m_gl.glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,newState.indexCounts[0]*sizeof(quint16),indices.data());
        for(int i=0;i<newState.indexCounts[0];++i)QCOMPARE(indices[i],newState.indices[0][i]);
        m_gl.glBindVertexArray(glState[0]);m_gl.glBindBuffer(GL_ARRAY_BUFFER,glState[1]);candidate.reset();QVERIFY(!m_gl.glIsVertexArray(vao));QVERIFY(m_gl.glIsVertexArray(oldVao));
        QCOMPARE(m_gl.glGetError(),GLenum(GL_NO_ERROR));
    }

    void emptyDrawableArraysPrepareWithoutTouchingPreviousResources()
    {
        GetterState oldState,newState;newState.vertexCounts.fill(0);newState.indexCounts.fill(0);
        newState.nullGetters.insert("csmGetDrawableVertexPositions");newState.nullGetters.insert("csmGetDrawableVertexUvs");newState.nullGetters.insert("csmGetDrawableIndices");
        auto previousModel=Model(oldState),nextModel=Model(newState);auto previous=Renderer(),candidate=Renderer();
        QVERIFY(nextModel->CopyRuntimeStateFrom(*previousModel,CubismModelUpdatePolicy::AllowDrawableGeometryEdits));nextModel->InitMesh();
        QCOMPARE(nextModel->GetMeshMap()->GetMapMesh().size(),nextModel->GetDrawableCount());
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));QVERIFY(Gl(previous).Initialize(previousModel.get()));
        QVERIFY(Gl(previous).UploadDrawableGeometry(*previousModel,0,previousModel->GetDrawableVertices(0),Gl(previous)._meshBuffers[0],nullptr));
        const auto vao=Gl(previous)._meshBuffers[0]->GetVertexArray();const auto mask=Gl(previous).GetMaskBuffer(0)->GetRenderTexture();const auto state=State();QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(nextModel.get(),Gl(previous),&error),qPrintable(error));QCOMPARE(State(),state);
        for(const auto& buffer:Gl(candidate)._meshBuffers)QVERIFY(!buffer);
        QVERIFY(m_gl.glIsVertexArray(vao));QVERIFY(m_gl.glIsFramebuffer(mask));candidate.reset();QVERIFY(m_gl.glIsVertexArray(vao));QVERIFY(m_gl.glIsFramebuffer(mask));
        QCOMPARE(previousModel->GetDrawableVertexCount(0),4);QCOMPARE(m_gl.glGetError(),GLenum(GL_NO_ERROR));
    }

    void geometryFailuresPreservePreviousResources_data()
    {
        QTest::addColumn<QString>("failure");
        for (const char* name : {"nan-position", "null-position", "null-uv", "null-indices", "bad-index",
                                 "incomplete-triangle", "negative-vertices", "empty-with-indices", "bad-mask",
                                 "null-mask", "negative-mask", "invalid-texture-binding",
                                 "null-vertex-counts", "null-position-array", "prior-gl-error"})
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
        else if (failure == "negative-mask") newState.masks[1][0] = -1;
        else if (failure == "invalid-texture-binding") {
            QVERIFY(!m_gl.glIsTexture(0x7ffffffe));
            Gl(previous).BindTexture(9, 0x7ffffffe);
        }
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

    void rendererPreservesExistingStructurallySafeMaskLists_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("self") << 0;
        QTest::newRow("duplicate") << 1;
        QTest::newRow("duplicates-beyond-drawable-count") << 2;
    }

    void rendererPreservesExistingStructurallySafeMaskLists()
    {
        QFETCH(int, kind);
        GetterState oldState, newState;
        const std::array<int, 3> repeated{0, 0, 0};
        const int count = kind + 1;
        oldState.maskCounts[1] = newState.maskCounts[1] = count;
        oldState.masks[1] = newState.masks[1] = kind == 0 ? std::array<int, 2>{1, 0} : std::array<int, 2>{0, 0};
        if (kind == 2) oldState.maskPointers[1] = newState.maskPointers[1] = repeated.data();
        auto oldModel = Model(oldState), newModel = Model(newState);
        QVERIFY(oldModel->HasValidClippingDefinitions());
        QVERIFY(newModel->HasValidClippingDefinitions());
        if (kind == 2) QVERIFY(!newModel->IsCompatibleForKeyformUpdate(*oldModel));
        else QVERIFY(newModel->IsCompatibleForKeyformUpdate(*oldModel));
        auto previous = Renderer(), candidate = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));
        QVERIFY(Gl(previous).Initialize(oldModel.get()));
        const auto oldBuffer = Gl(previous).GetMaskBuffer(0)->GetRenderTexture();
        QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous), &error), qPrintable(error));
        auto* clipping = (*Gl(candidate)._clippingManager->GetClippingContextListForDraw())[1];
        QVERIFY(clipping);
        QCOMPARE(clipping->_clippingIdCount, count);
        for (int index = 0; index < count; ++index) QCOMPARE(clipping->_clippingIdList[index], kind == 0 ? 1 : 0);
        QVERIFY(m_gl.glIsFramebuffer(oldBuffer));
        QCOMPARE(Gl(previous).GetModel(), oldModel.get());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
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

    void unmaskedRendererStoresValidatedSizesForFutureMasks()
    {
        GetterState oldState, newState;
        oldState.maskCounts.fill(0);
        auto oldModel = Model(oldState), newModel = Model(newState);
        auto previous = Renderer(), candidate = Renderer();
        QVERIFY(Gl(previous).setGlOwningContext(&m_context));
        QVERIFY(Gl(previous).Initialize(oldModel.get(), 2));
        QCOMPARE(Gl(previous).GetRenderTextureCount(), 0);
        const auto before = State();
        Gl(previous).SetClippingMaskBufferSize(64, 96);
        QCOMPARE(State(), before);
        QCOMPARE(Gl(previous).GetRenderTextureCount(), 0);
        QVERIFY(!Gl(previous).GetMaskBuffer(0));
        QCOMPARE(Gl(previous)._requestedMaskBufferSize.X, 64.0f);
        QCOMPARE(Gl(previous)._requestedMaskBufferSize.Y, 96.0f);
        GLint maximum = 0;
        m_gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        for (const auto& size : {CubismVector2(0, 32), CubismVector2(32, -1), CubismVector2(nan, 32),
             CubismVector2(32, infinity), CubismVector2(float(maximum) + 1, 32),
             CubismVector2(float(std::numeric_limits<int>::max()), 32)}) {
            Gl(previous).SetClippingMaskBufferSize(size.X, size.Y);
            QCOMPARE(Gl(previous)._requestedMaskBufferSize.X, 64.0f);
            QCOMPARE(Gl(previous)._requestedMaskBufferSize.Y, 96.0f);
        }
        m_gl.glEnable(0xffffffff);
        Gl(previous).SetClippingMaskBufferSize(128, 128);
        QCOMPARE(Gl(previous)._requestedMaskBufferSize.X, 64.0f);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        m_context.doneCurrent();
        Gl(previous).SetClippingMaskBufferSize(128, 128);
        QCOMPARE(Gl(previous)._requestedMaskBufferSize.Y, 96.0f);
        QVERIFY(m_context.makeCurrent(&m_surface));
        QString error;
        QVERIFY2(Gl(candidate).PrepareModelReplacement(newModel.get(), Gl(previous), &error), qPrintable(error));
        QCOMPARE(Gl(candidate).GetRenderTextureCount(), 2);
        QCOMPARE(Gl(candidate).GetMaskBuffer(0)->GetBufferWidth(), 64);
        QCOMPARE(Gl(candidate).GetMaskBuffer(0)->GetBufferHeight(), 96);
        QCOMPARE(Gl(previous).GetRenderTextureCount(), 0);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void switchingMaskPrecisionInvalidatesThePreviousLayout_data()
    {
        QTest::addColumn<bool>("inverted");
        QTest::newRow("mask") << false;
        QTest::newRow("inverted") << true;
    }

    void switchingMaskPrecisionInvalidatesThePreviousLayout()
    {
        QFETCH(bool, inverted);
        GetterState state;
        state.drawableCount = 3;
        state.maskCounts = {0, 0, 2};
        state.flags.fill(Core::csmIsDoubleSided);
        if (inverted) state.flags[2] |= Core::csmIsInvertedMask;
        state.positions[1] = state.positions[0];
        state.dynamicFlags.fill(Core::csmIsVisible | Core::csmVertexPositionsDidChange);
        auto model = Model(state);
        auto renderer = Renderer();
        auto& gl = Gl(renderer);
        QVERIFY(gl.setGlOwningContext(&m_context));
        QVERIFY(gl.Initialize(model.get()));
        gl.SetClippingMaskBufferSize(32, 48);
        CubismMatrix44 matrix;
        matrix.LoadIdentity();
        gl.SetMvpMatrix(&matrix);
        const std::array<GLuint, 3> textures{Texture(64), Texture(128), Texture(255)};
        const auto deleteTextures = qScopeGuard([&] { m_gl.glDeleteTextures(3, textures.data()); });
        for (int index = 0; index < 3; ++index) gl.BindTexture(index, textures[index]);
        QOpenGLFramebufferObject framebuffer(64, 64);
        QVERIFY(framebuffer.isValid());
        allowDraw = true;
        auto* delegate = LAppDelegate::GetInstance();
        delegate->m_hiddenDrawables = {"MeshA", "MeshB"};
        const auto restoreDelegate = qScopeGuard([&] { delegate->m_hiddenDrawables.clear(); allowDraw = false; });
        const auto draw = [&]() {
            if (!framebuffer.bind()) return -1;
            m_gl.glViewport(0, 0, 64, 64);
            m_gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            m_gl.glDisable(GL_SCISSOR_TEST);
            m_gl.glDisable(GL_DEPTH_TEST);
            m_gl.glDisable(GL_STENCIL_TEST);
            m_gl.glClearColor(0, 0, 0, 0);
            m_gl.glClear(GL_COLOR_BUFFER_BIT);
            if (!gl.DrawModel()) return -1;
            std::array<uchar, 4> pixel{};
            m_gl.glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            return int(pixel[3]);
        };
        const double coverage = 1 - (1 - 64.0 / 255) * (1 - 128.0 / 255);
        const int expected = qRound((inverted ? 1 - coverage : coverage) * 255);
        QVERIFY(qAbs(draw() - expected) <= 2);
        QVERIFY(!gl._maskBuffersNeedRedraw);
        gl.UseHighPrecisionMask(false);
        QVERIFY(!gl._maskBuffersNeedRedraw);
        gl.UseHighPrecisionMask(true);
        QVERIFY(gl._maskBuffersNeedRedraw);
        QVERIFY(qAbs(draw() - expected) <= 2);
        QVERIFY(!gl._maskBuffersNeedRedraw);
        gl.UseHighPrecisionMask(true);
        QVERIFY(!gl._maskBuffersNeedRedraw);
        state.dynamicFlags[0] = state.dynamicFlags[1] = Core::csmIsVisible;
        state.positionPointers[0] = state.positionPointers[1] = nullptr;
        gl.UseHighPrecisionMask(false);
        QVERIFY(gl._maskBuffersNeedRedraw);
        QCOMPARE(draw(), inverted ? 255 : 0);
        QVERIFY(gl._maskBuffersNeedRedraw);
        QVERIFY(!gl._maskTopologyUpdated);
        state.positionPointers[0] = state.positions[0].data();
        state.positionPointers[1] = state.positions[1].data();
        state.dynamicFlags[0] = state.dynamicFlags[1] = Core::csmIsVisible | Core::csmVertexPositionsDidChange;
        QVERIFY(qAbs(draw() - expected) <= 2);
        QVERIFY(!gl._maskBuffersNeedRedraw);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
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
