#include "opengl/cpsdfiletask.h"
#include "opengl/cpsdrendersession.h"
#include "std/cexportcontrol.h"
#include "cubism_model_fixture.h"
#include "psd_binary_reader.h"
#include <QtTest>
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLFramebufferObject>
#include <QTemporaryDir>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;
using CubismModelFixture::GetterState;
namespace Core = Live2D::Cubism::Core;

namespace {
std::unordered_map<void*, std::size_t> allocations;
std::unordered_map<void*, std::unique_ptr<GetterState>> coreStates;
std::unique_ptr<CubismIdManager> ids;
void* allocate(std::size_t size, std::size_t alignment = 0)
{
    void* pointer = alignment ? ::operator new(size, std::align_val_t(alignment), std::nothrow) : std::malloc(size);
    if (!pointer) throw std::bad_alloc();
    allocations.emplace(pointer, alignment);
    return pointer;
}
void release(void* pointer)
{
    if (!pointer) return;
    const auto found = allocations.find(pointer);
    if (found == allocations.end()) qFatal("Unknown SDK allocation in cooperative PSD test");
    const auto alignment = found->second;
    allocations.erase(found);
    if (alignment) ::operator delete(pointer, std::align_val_t(alignment));
    else std::free(pointer);
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool seed(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("original-project") == 16;
}
const PsdBinary::Layer* layer(const PsdBinary::Document& document, const QString& name)
{
    for (const auto& entry : document.layers) if (entry.name == name) return &entry;
    return nullptr;
}
quint8 pixel(const PsdBinary::Layer& layer, int channel, int x, int y)
{
    return static_cast<quint8>(layer.Plane(channel).at(y * layer.bounds.width() + x));
}
[[noreturn]] void unexpectedRuntime() { qFatal("Stepped PSD rendering must not access runtime-global or legacy frame paths"); }
}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size); }
void operator delete(void* pointer, CubismAllocationTag) { release(pointer); }
namespace Live2D { namespace Cubism { namespace Framework {
namespace Constant {
const csmInt32 VertexOffset = 0;
const csmInt32 VertexStep = 2;
}
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size); }
void CubismFramework::Deallocate(void* pointer) { release(pointer); }
void CubismFramework::DeallocateAligned(void* pointer)
{
    if (!pointer) return;
    if (coreStates.erase(pointer) != 1) qFatal("Unknown Core fixture in cooperative PSD test");
    release(pointer);
}
CubismIdManager* CubismFramework::GetIdManager() { return ids.get(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
}}}
LAppDelegate* LAppDelegate::GetInstance() { unexpectedRuntime(); }
QOpenGLContext* LAppDelegate::getGlOwningContext() { unexpectedRuntime(); }
const QWidget* LAppDelegate::GetWindow() { unexpectedRuntime(); }
bool LAppDelegate::IsOffScreen() { unexpectedRuntime(); }
void LAppDelegate::SetIsOffset(bool) { unexpectedRuntime(); }
CPsdWriteList* LAppDelegate::GetCPsdWriteList() { unexpectedRuntime(); }

class PsdCooperativeGlTests : public QObject
{
    Q_OBJECT
    using ModelOwner = std::unique_ptr<CubismModel, void (*)(CubismModel*)>;
    using RendererOwner = std::unique_ptr<CubismRenderer_OpenGLES2, void (*)(CubismRenderer_OpenGLES2*)>;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    std::vector<ModelOwner> m_models;
    std::vector<RendererOwner> m_renderers;
    QVector<GLuint> m_textures;

    std::pair<CubismModel*, CubismRenderer_OpenGLES2*> scene(bool masked = false, bool high = false,
                                                          bool available = true, bool inverted = false,
                                                          const QVector<int>& maskList = {})
    {
        auto state = std::make_unique<GetterState>();
        state->maskCounts = {{0, masked ? 1 : 0, 0}};
        state->flags[0] = Core::csmIsDoubleSided;
        state->flags[1] = Core::csmIsDoubleSided | (inverted ? Core::csmIsInvertedMask : 0);
        state->dynamicFlags[0] = Core::csmIsVisible | (available ? Core::csmVertexPositionsDidChange : 0);
        state->dynamicFlags[1] = Core::csmIsVisible | Core::csmVertexPositionsDidChange;
        if (!maskList.isEmpty()) {
            state->drawableCount = 3;
            state->maskCounts = {0, int(maskList.size()), 0};
            state->maskPointers[1] = maskList.constData();
            state->flags[2] = Core::csmIsDoubleSided;
            state->dynamicFlags[2] = Core::csmIsVisible | Core::csmVertexPositionsDidChange;
        }
        if (masked) {
            for (auto& vertex : state->positions[0]) {
                vertex.X *= 0.5f;
                vertex.Y *= 0.5f;
            }
        }
        for (int index = 0; index < state->drawableCount; ++index) {
            state->multiply[index] = {1, 1, 1, 1};
            state->screen[index] = {0, 0, 0, 0};
        }
        void* handle = allocate(sizeof(CubismModelFixture::ModelHandle), Core::csmAlignofModel);
        auto* core = CubismModelFixture::AttachModel(handle, state.get());
        coreStates.emplace(handle, std::move(state));
        ModelOwner model(new CubismModel(core), [](CubismModel* item) { delete item; });
        model->Initialize();
        RendererOwner renderer(static_cast<CubismRenderer_OpenGLES2*>(CubismRenderer::Create()),
                               [](CubismRenderer_OpenGLES2* item) { CubismRenderer::Delete(item); });
        if (!renderer->setGlOwningContext(m_context.get()) || !renderer->Initialize(model.get())) qFatal("Renderer setup failed");
        renderer->UseHighPrecisionMask(high);
        const std::array<std::array<quint8, 4>, 3> colors{{
            {{255, 0, 0, quint8(maskList.isEmpty() ? (masked ? 128 : 255) : 64)}},
            {{0, 0, 255, quint8(maskList.isEmpty() ? 128 : 255)}}, {{255, 255, 255, 128}}}};
        for (int index = 0; index < model->GetDrawableCount(); ++index) {
            GLuint texture = 0;
            m_gl->glGenTextures(1, &texture);
            m_gl->glBindTexture(GL_TEXTURE_2D, texture);
            m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colors[index].data());
            m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            renderer->BindTexture(index, texture);
            m_textures.append(texture);
        }
        const auto result = std::make_pair(model.get(), renderer.get());
        m_models.push_back(std::move(model));
        m_renderers.push_back(std::move(renderer));
        return result;
    }

    std::unique_ptr<CPsdRenderSession> session(CubismModel* model, CubismRenderer_OpenGLES2* renderer,
                                              CExportControl& control, QSize size = QSize(64, 64), bool twoLayers = true)
    {
        QVector<CPsdRenderSession::Entry> entries;
        entries.append({CPsdExportItem::Type::Layer, 0, QStringLiteral("红色😀"), -1});
        if (twoLayers) entries.append({CPsdExportItem::Type::Layer, 1, QStringLiteral("蓝色"), -1});
        CubismMatrix44 matrix;
        const QPointer<QOpenGLContext> context = m_context.get();
        const QPointer<QOffscreenSurface> surface = m_surface.get();
        return std::make_unique<CPsdRenderSession>(m_surface.get(), m_context.get(), model, renderer, size, matrix,
            std::move(entries), [context, surface] { return context && surface && context->makeCurrent(surface); },
            [](QString*) { return true; }, &control);
    }

private slots:
    void emptyWriterPreservesCancellation_data()
    {
        QTest::addColumn<QString>("operation"); QTest::addColumn<bool>("settled");
        for (const auto* action : {"write", "layer", "empty", "start", "end"})
            for (bool settled : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1(action) + (settled ? "-settled" : "-pending")))
                    << QString::fromLatin1(action) << settled;
    }

    void emptyWriterPreservesCancellation()
    {
        QFETCH(QString, operation); QFETCH(bool, settled);
        CExportControl control; CPsdWriteList writer; writer.SetControl(&control); control.Cancel();
        if (settled) QVERIFY(!control.Checkpoint());
        bool accepted = true; CPsdPreviewKind preview = CPsdPreviewKind::Composite;
        if (operation == "write") { accepted = writer.WritePsd(&preview); QCOMPARE(preview, CPsdPreviewKind::Unknown); }
        if (operation == "layer") accepted = writer.AddLayer("layer", QRect(), QImage());
        if (operation == "empty") accepted = writer.AddEmptyLayer("empty");
        if (operation == "start") accepted = writer.AddGroupStart("group");
        if (operation == "end") accepted = writer.AddGroupEnd();
        QVERIFY(!accepted); QVERIFY(!control.Checkpoint()); QVERIFY(control.WasCanceled());
        QVERIFY(!control.IsCommitted()); QCOMPARE(writer.Control(), &control);
    }

    void writerWrapperRejectsCallbackReentry_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("cancelFirst");
        for (const auto* action : {"create", "write", "layer", "empty", "start", "end", "control", "close", "sort", "moc", "cid", "clear"})
            for (bool cancel : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1(action) + (cancel ? "-cancel" : "-failure")))
                    << QString::fromLatin1(action) << cancel;
    }

    void writerWrapperRejectsCallbackReentry()
    {
        QFETCH(QString, operation);
        QFETCH(bool, cancelFirst);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("protected.psd");
        const auto replacement = directory.filePath("replacement.psd");
        QVERIFY(seed(path));
        CPsdWriteList writer;
        QVERIFY(writer.CreatePsd(path, 2, 137));
        QImage image(2, 137, QImage::Format_ARGB32);
        image.fill(qRgba(10, 20, 30, 255));
        QVERIFY(writer.AddLayer("original", QRect(0, 0, 2, 137), image));
        CExportControl control;
        writer.SetControl(&control);
        bool called = false, rejected = true;
        CPsdPreviewKind nestedPreview = CPsdPreviewKind::Composite;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (called || progress.detail != QStringLiteral("合成透明预览")) return true;
            called = true;
            if (cancelFirst) control.Cancel();
            if (operation == "create") rejected = !writer.CreatePsd(replacement, 2, 2);
            if (operation == "write") rejected = !writer.WritePsd(&nestedPreview);
            if (operation == "layer") rejected = !writer.AddLayer("nested", QRect(0, 0, 2, 137), image);
            if (operation == "empty") rejected = !writer.AddEmptyLayer("nested");
            if (operation == "start") rejected = !writer.AddGroupStart("nested");
            if (operation == "end") rejected = !writer.AddGroupEnd();
            if (operation == "control") writer.SetControl(nullptr);
            if (operation == "close") writer.ClosePsd();
            if (operation == "sort") rejected = writer.GetSortDrawableIndexList({}, nullptr).isEmpty();
            if (operation == "moc") writer.SetMocName("nested.moc3");
            if (operation == "cid") writer.SetCidName("nested.cdi3.json");
            if (operation == "clear") writer.ClaerArtToName();
            return true;
        }));
        CPsdPreviewKind preview = CPsdPreviewKind::Composite;
        QVERIFY(!writer.WritePsd(&preview));
        QVERIFY(called);
        QVERIFY(rejected);
        QCOMPARE(writer.Control(), &control);
        QCOMPARE(preview, CPsdPreviewKind::Unknown);
        QCOMPARE(nestedPreview, CPsdPreviewKind::Composite);
        QVERIFY(!control.IsCommitted());
        QCOMPARE(control.WasCanceled(), cancelFirst);
        QCOMPARE(bytes(path), QByteArray("original-project"));
        QVERIFY(!QFileInfo::exists(replacement));
        writer.SetControl(nullptr);
        QVERIFY(writer.CreatePsd(path, 2, 2));
        QVERIFY(writer.AddEmptyLayer("retry"));
        QVERIFY(writer.WritePsd(&preview));
        QCOMPARE(preview, CPsdPreviewKind::Composite);
    }

    void writerWrapperFreezesBorrowedGroupNameBeforeCallbacks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("group.psd");
        CPsdWriteList writer;
        QVERIFY(writer.CreatePsd(path, 2, 2));
        CExportControl control;
        QString name = QStringLiteral("original folder");
        const QString borrowed = QString::fromRawData(name.data(), name.size());
        bool changed = false;
        writer.SetControl(&control);
        QVERIFY(control.SetHandler([&](const CExportControl::Progress&, QString*) {
            if (!changed) { name.fill('x'); changed = true; }
            return true;
        }));
        QVERIFY(writer.AddGroupStart(borrowed));
        QVERIFY(changed);
        QVERIFY(writer.AddEmptyLayer("empty"));
        QVERIFY(writer.AddGroupEnd());
        QVERIFY(writer.WritePsd());
        PsdBinary::Document parsed;
        QString error;
        QVERIFY2(PsdBinary::Parse(bytes(path), parsed, &error), qPrintable(error));
        QCOMPARE(parsed.layers.last().name, QStringLiteral("original folder"));
    }

    void emptyGeometryWritesIdentifiedLayers_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("coincident") << 0;
        QTest::newRow("outside") << 1;
        QTest::newRow("no-triangles") << 2;
        QTest::newRow("no-vertices") << 3;
    }

    void emptyGeometryWritesIdentifiedLayers()
    {
        QFETCH(int, kind);
        const auto [model, renderer] = scene();
        auto& state = *coreStates.at(model->GetModel());
        if (kind == 0) for (auto& point : state.positions[0]) point = {0.25f, 0.25f};
        if (kind == 1) for (auto& point : state.positions[0]) { point.X += 4; point.Y += 4; }
        if (kind >= 2) state.indexCounts[0] = 0;
        if (kind == 3) state.vertexCounts[0] = 0;
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("empty.psd");
        CExportControl control; QString error;
        auto render = session(model, renderer, control);
        QVERIFY2(render->Begin(true, &error), qPrintable(error));
        QVector<CPsdExportItem> accepted;
        QVERIFY2(CPsdFileTask::Execute(path, QSize(64, 64), false,
            [&](CPsdExportItem& item, QString* detail) { return render->Next(item, detail); }, &control, &error,
            [&](const CPsdExportItem& item, QString*) { accepted.append(item); return true; }), qPrintable(error));
        QVERIFY2(render->Finish(true, &error), qPrintable(error));
        QVERIFY(control.IsCommitted()); QCOMPARE(accepted.size(), 2);
        QCOMPARE(accepted[0].type, CPsdExportItem::Type::EmptyLayer);
        QCOMPARE(accepted[0].runtimeIndex, 0); QCOMPARE(accepted[0].psdLayerId, quint32(1));
        using Reason = CPsdExportCoverage::Reason;
        QCOMPARE(accepted[0].emptyReason, kind == 0 ? Reason::DegenerateBounds : kind == 1 ? Reason::OutsideCanvas :
                                          kind == 2 ? Reason::NoTriangles : Reason::NoVertices);
        PsdBinary::Document decoded;
        QVERIFY2(PsdBinary::Parse(bytes(path), decoded, &error), qPrintable(error));
        const auto* empty = layer(decoded, QStringLiteral("红色😀")); QVERIFY(empty);
        QCOMPARE(empty->bounds, QRect()); QCOMPARE(empty->channels.size(), 0);
        QVERIFY(empty->layerId); QCOMPARE(*empty->layerId, quint32(1));
        const auto* raster = layer(decoded, QStringLiteral("蓝色")); QVERIFY(raster); QVERIFY(!raster->bounds.isEmpty());
    }

    void malformedEmptyGeometryDoesNotReplaceTarget_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("empty-with-indices") << 0;
        QTest::newRow("collapsed-bad-index") << 1;
        QTest::newRow("nonfinite-matrix") << 2;
        QTest::newRow("nonfinite-without-triangles") << 3;
        QTest::newRow("nonfinite-opacity") << 4;
        QTest::newRow("nonfinite-model-opacity") << 5;
    }

    void malformedEmptyGeometryDoesNotReplaceTarget()
    {
        QFETCH(int, kind);
        const auto [model, renderer] = scene();
        auto& state = *coreStates.at(model->GetModel());
        for (auto& point : state.positions[0]) point = {0.25f, 0.25f};
        if (kind == 0) state.vertexCounts[0] = 0;
        if (kind == 1) state.indices[0][0] = 65535;
        if (kind == 3) { state.indexCounts[0] = 0; state.positions[0][0].X = std::numeric_limits<float>::quiet_NaN(); }
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto path = temporary.filePath("bad.psd");
        QVERIFY(seed(path)); CExportControl control; QString error;
        auto render = session(model, renderer, control);
        if (kind == 2) render->m_matrix.GetArray()[0] = std::numeric_limits<float>::quiet_NaN();
        if (kind == 4) render->m_entries[0].opacityOverride = std::numeric_limits<float>::quiet_NaN();
        if (kind == 5) model->SetModelOpacity(std::numeric_limits<float>::quiet_NaN());
        const bool began = render->Begin(true, &error);
        if (kind == 2) QVERIFY(!began);
        else {
            QVERIFY2(began, qPrintable(error));
            QVERIFY(!CPsdFileTask::Execute(path, QSize(64, 64), false,
                [&](CPsdExportItem& item, QString* detail) { return render->Next(item, detail); }, &control, &error));
            QVERIFY(render->Finish(false));
        }
        QVERIFY(!error.isEmpty()); QCOMPARE(bytes(path), QByteArray("original-project")); QVERIFY(!control.IsCommitted());
    }

    void unspecifiedEmptyRasterFailsWithoutReplacingTarget()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const auto path = temporary.filePath("bad.psd");
        QVERIFY(seed(path)); CExportControl control; QString error; int index = 0;
        QVERIFY(!CPsdFileTask::Execute(path, QSize(64, 64), false, [&](CPsdExportItem& item, QString*) {
            item.type = index++ == 0 ? CPsdExportItem::Type::Layer : CPsdExportItem::Type::Done;
            item.name = QStringLiteral("invalid"); return true;
        }, &control, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(bytes(path), QByteArray("original-project")); QVERIFY(!control.IsCommitted());
    }

    void repeatedSourcesKeepTheirCoverageInSteppedPsd_data()
    {
        QTest::addColumn<bool>("high");
        QTest::addColumn<bool>("inverted");
        for (bool high : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(high).arg(inverted))) << high << inverted;
    }

    void repeatedSourcesKeepTheirCoverageInSteppedPsd()
    {
        QFETCH(bool, high);
        QFETCH(bool, inverted);
        QVector<int> alphas;
        for (const QVector<int>& masks : {QVector<int>{0, 0, 2}, QVector<int>{0, 2, 2}}) {
            const auto [model, renderer] = scene(true, high, true, inverted, masks);
            QOpenGLFramebufferObject framebuffer(64, 64);
            QVERIFY(framebuffer.isValid() && framebuffer.bind());
            m_gl->glViewport(0, 0, 64, 64);
            m_gl->glDisable(GL_DITHER);
            QString error;
            QVERIFY2(renderer->BeginPsdExport(true, nullptr, &error), qPrintable(error));
            CubismMatrix44 matrix;
            QImage image;
            QRect bounds;
            bool ready = false;
            int steps = 0;
            while (!ready && steps++ < 100)
                QVERIFY2(renderer->DrawPsdStep(1, 1, matrix, QSize(64, 64), image, bounds, ready, &error), qPrintable(error));
            QVERIFY(ready && !image.isNull());
            renderer->EndPsdExport(true);
            const auto point = QPoint(32, 32) - bounds.topLeft();
            QVERIFY(image.rect().contains(point));
            const int actual = qAlpha(image.pixel(point));
            double transmission = 1;
            for (int source : masks) transmission *= 1.0 - (source == 0 ? 64.0 : 128.0) / 255;
            const int expected = qRound(255 * (inverted ? transmission : 1 - transmission));
            QVERIFY2(std::abs(actual - expected) <= 1, qPrintable(QString("actual %1 expected %2").arg(actual).arg(expected)));
            alphas.append(actual);
            QCOMPARE(model->GetDrawableMaskCounts()[1], 3);
            for (int index = 0; index < masks.size(); ++index) QCOMPARE(model->GetDrawableMasks()[1][index], masks[index]);
        }
        QVERIFY(std::abs(alphas[0] - alphas[1]) >= 20);
    }

    void initTestCase() { ids = std::make_unique<CubismIdManager>(); }
    void init()
    {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(format);
        QVERIFY(m_context->create());
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(m_context->format());
        m_surface->create();
        QVERIFY(m_surface->isValid());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        m_gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_context.get());
        QVERIFY(m_gl && m_gl->initializeOpenGLFunctions());
    }
    void cleanup()
    {
        if (m_context) QVERIFY(m_context->makeCurrent(m_surface.get()));
        m_renderers.clear();
        m_models.clear();
        if (m_context && !m_textures.isEmpty()) m_gl->glDeleteTextures(m_textures.size(), m_textures.constData());
        m_textures.clear();
        CubismShader_OpenGLES2::DeleteInstance();
        QVERIFY(coreStates.empty());
        if (m_context) m_context->doneCurrent();
        m_context.reset();
        m_surface.reset();
        m_gl = nullptr;
    }
    void cleanupTestCase() { ids.reset(); QVERIFY(allocations.empty()); }

    void realGpuLayersUseWorkerAndRoundTripPsdWithBackpressure()
    {
        const auto [model, renderer] = scene();
        CExportControl control;
        auto render = session(model, renderer, control);
        QString error;
        QVERIFY2(render->Begin(false, &error), qPrintable(error));
        std::atomic<bool> firstEncoded{false}, encodedOffGui{true};
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Encoding) {
                if (QThread::currentThread() == qApp->thread()) encodedOffGui = false;
                if (progress.detail == QStringLiteral("红色😀") && progress.current == progress.total) firstEncoded = true;
            }
            return true;
        }));
        int layers = 0;
        bool correctThreads = true, backpressure = true;
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("result.psd"));
        m_context->doneCurrent();
        QVERIFY2(CPsdFileTask::Execute(path, QSize(64, 64), true, [&](CPsdExportItem& item, QString* detail) {
            correctThreads = correctThreads && QThread::currentThread() == qApp->thread();
            if (!render->Next(item, detail)) return false;
            if (item.type == CPsdExportItem::Type::Layer) {
                if (layers) backpressure = backpressure && firstEncoded.load();
                ++layers;
            }
            return true;
        }, &control, &error), qPrintable(error));
        QVERIFY(render->Finish(true, &error));
        QVERIFY(correctThreads && encodedOffGui && backpressure);
        QCOMPARE(layers, 2);
        QVERIFY(control.IsCommitted());
        PsdBinary::Document document;
        QVERIFY2(PsdBinary::Parse(bytes(path), document, &error), qPrintable(error));
        QCOMPARE(document.canvas, QSize(64, 64));
        const auto* red = layer(document, QStringLiteral("红色😀"));
        const auto* blue = layer(document, QStringLiteral("蓝色"));
        QVERIFY(red && blue);
        QCOMPARE(red->bounds, QRect(0, 0, 64, 64));
        QCOMPARE(pixel(*red, 0, 32, 32), quint8(255));
        QCOMPARE(pixel(*red, -1, 32, 32), quint8(255));
        QCOMPARE(pixel(*blue, 2, 32, 32), quint8(255));
        QCOMPARE(pixel(*blue, -1, 32, 32), quint8(128));
        QCOMPARE(red->opacity, quint8(229));
        QCOMPARE(blue->opacity, quint8(204));
    }

    void masksAdvanceSeparatelyAndKeepAvailabilityContract_data()
    {
        QTest::addColumn<bool>("high"); QTest::addColumn<bool>("available"); QTest::addColumn<bool>("inverted");
        for (bool high : {false, true}) for (bool available : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(high).arg(available).arg(inverted))) << high << available << inverted;
    }
    void masksAdvanceSeparatelyAndKeepAvailabilityContract()
    {
        QFETCH(bool, high); QFETCH(bool, available); QFETCH(bool, inverted);
        const auto [model, renderer] = scene(true, high, available, inverted);
        CExportControl control;
        auto render = session(model, renderer, control);
        QString error;
        QVERIFY(render->Begin(true, &error));
        int progressSteps = 0;
        QTemporaryDir temporary;
        const QString path = temporary.filePath("masked.psd");
        QVERIFY2(CPsdFileTask::Execute(path, QSize(64, 64), false, [&](CPsdExportItem& item, QString* detail) {
            const bool ok = render->Next(item, detail);
            if (ok && item.type == CPsdExportItem::Type::Progress) ++progressSteps;
            return ok;
        }, &control, &error), qPrintable(error));
        QVERIFY(render->Finish(true));
        QVERIFY(progressSteps >= 6);
        PsdBinary::Document document;
        QVERIFY2(PsdBinary::Parse(bytes(path), document, &error), qPrintable(error));
        const auto* blue = layer(document, QStringLiteral("蓝色"));
        QVERIFY(blue);
        QCOMPARE(pixel(*blue, -1, 32, 32), quint8(available ? 64 : inverted ? 128 : 0));
        QCOMPARE(pixel(*blue, -1, 1, 1), quint8(inverted ? 128 : 0));
        QCOMPARE(blue->clipping, quint8(0));
    }

    void replacementAlphaReplaysUnchangedVerticesIntoPsd_data()
    {
        QTest::addColumn<bool>("high"); QTest::addColumn<bool>("inverted");
        for (bool high : {false, true}) for (bool inverted : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(high).arg(inverted))) << high << inverted;
    }
    void replacementAlphaReplaysUnchangedVerticesIntoPsd()
    {
        QFETCH(bool, high); QFETCH(bool, inverted);
        const auto [model, renderer] = scene(true, high, false, inverted);
        GLuint replacement = 0; m_gl->glGenTextures(1,&replacement); m_textures.append(replacement);
        m_gl->glBindTexture(GL_TEXTURE_2D,replacement); const std::array<quint8,4> rgba{255,0,0,64};
        m_gl->glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
        m_gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); m_gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        renderer->BindTexture(0,replacement);
        for (int repeat = 0; repeat < 2; ++repeat) {
            CExportControl control; auto render = session(model,renderer,control); QString error; QVERIFY(render->Begin(true,&error));
            QTemporaryDir temporary; const QString path = temporary.filePath("replaced.psd");
            QVERIFY2(CPsdFileTask::Execute(path,QSize(64,64),false,[&](CPsdExportItem& item,QString* detail) { return render->Next(item,detail); },&control,&error),qPrintable(error));
            QVERIFY(render->Finish(true)); PsdBinary::Document doc; QVERIFY2(PsdBinary::Parse(bytes(path),doc,&error),qPrintable(error));
            const auto* blue = layer(doc,QStringLiteral("蓝色")); QVERIFY(blue);
            QVERIFY(qAbs(int(pixel(*blue,-1,32,32)) - (inverted ? 96 : 32)) <= 1);
            QCOMPARE(pixel(*blue,-1,1,1),quint8(inverted ? 128 : 0)); QCOMPARE(blue->clipping,quint8(0));
        }
    }

    void cancellationBetweenLayersPreservesOldOutput()
    {
        const auto [model, renderer] = scene();
        CExportControl control;
        auto render = session(model, renderer, control);
        QVERIFY(render->Begin(false));
        QTemporaryDir temporary;
        const QString path = temporary.filePath("existing.psd");
        QVERIFY(seed(path));
        bool firstEncoded = false;
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Encoding && progress.detail == QStringLiteral("红色😀") &&
                progress.current == progress.total) { firstEncoded = true; control.Cancel(); }
            return true;
        }));
        int layers = 0;
        QString error;
        QVERIFY(!CPsdFileTask::Execute(path, QSize(64, 64), false, [&](CPsdExportItem& item, QString* detail) {
            const bool ok = render->Next(item, detail);
            if (ok && item.type == CPsdExportItem::Type::Layer) ++layers;
            return ok;
        }, &control, &error));
        QVERIFY(render->Finish(false));
        QVERIFY(firstEncoded && control.WasCanceled());
        QCOMPARE(layers, 1);
        QCOMPARE(bytes(path), QByteArray("original-project"));
        QCOMPARE(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden), QStringList({"existing.psd"}));
    }

    void guiCancelInterruptsOneLargeLayerCompression()
    {
        const auto [model, renderer] = scene();
        CExportControl control;
        const QSize canvas(2048, 1024);
        auto render = session(model, renderer, control, canvas, false);
        QVERIFY(render->Begin(false));
        std::mutex mutex;
        std::condition_variable arrived;
        bool guiResponded = false;
        std::atomic<bool> requested{false};
        QVERIFY(control.SetHandler([&](const CExportControl::Progress& progress, QString* detail) {
            if (progress.phase != CExportControl::Phase::Encoding || progress.current <= canvas.height() || requested.exchange(true)) return true;
            QMetaObject::invokeMethod(this, [&] {
                control.Cancel();
                { std::lock_guard<std::mutex> lock(mutex); guiResponded = true; }
                arrived.notify_one();
            }, Qt::QueuedConnection);
            std::unique_lock<std::mutex> lock(mutex);
            if (!arrived.wait_for(lock, std::chrono::seconds(3), [&] { return guiResponded; })) {
                *detail = QStringLiteral("界面未能在后台压缩时响应取消");
                return false;
            }
            return true;
        }));
        QTemporaryDir temporary;
        const QString path = temporary.filePath("existing.psd");
        QVERIFY(seed(path));
        QString error;
        QVERIFY(!CPsdFileTask::Execute(path, canvas, false,
            [&](CPsdExportItem& item, QString* detail) { return render->Next(item, detail); }, &control, &error));
        QVERIFY(render->Finish(false));
        QVERIFY(requested && guiResponded && control.WasCanceled());
        QCOMPARE(bytes(path), QByteArray("original-project"));
    }

    void foreignContextIsRestoredBetweenEveryGpuStep()
    {
        const auto [model, renderer] = scene(true, true);
        CExportControl control;
        auto render = session(model, renderer, control);
        QVERIFY(render->Begin(false));
        QOpenGLContext foreign;
        foreign.setFormat(m_context->format());
        QVERIFY(foreign.create());
        QOffscreenSurface foreignSurface;
        foreignSurface.setFormat(foreign.format()); foreignSurface.create();
        QVERIFY(foreign.makeCurrent(&foreignSurface));
        QTemporaryDir temporary;
        QString error;
        bool preserved = true;
        QVERIFY2(CPsdFileTask::Execute(temporary.filePath("result.psd"), QSize(64, 64), false,
            [&](CPsdExportItem& item, QString* detail) {
                const bool ok = render->Next(item, detail);
                preserved = preserved && QOpenGLContext::currentContext() == &foreign;
                return ok;
            }, &control, &error), qPrintable(error));
        QVERIFY(render->Finish(true));
        QVERIFY(preserved);
        QCOMPARE(QOpenGLContext::currentContext(), &foreign);
        QVERIFY(m_context->makeCurrent(m_surface.get()));
    }

    void gpuStepsRestoreCallerFramebufferAndBindingState()
    {
        const auto [model, renderer] = scene(true, true);
        CExportControl control;
        auto render = session(model, renderer, control);
        QVERIFY(render->Begin(false));
        COffscreenBuffer read, draw;
        QVERIFY(read.setGlOwningContext(m_context.get()));
        QVERIFY(draw.setGlOwningContext(m_context.get()));
        QVERIFY(read.CreateOffscreenSurface(16, 16));
        QVERIFY(draw.CreateOffscreenSurface(16, 16));
        GLuint vao = 0, buffer = 0;
        m_gl->glGenVertexArrays(1, &vao);
        m_gl->glGenBuffers(1, &buffer);
        m_gl->glBindVertexArray(vao);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, buffer);
        m_gl->glBufferData(GL_ARRAY_BUFFER, 32, nullptr, GL_STATIC_DRAW);
        m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
        m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, read.GetRenderTexture());
        m_gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.GetRenderTexture());
        m_gl->glViewport(1, 2, 11, 13);
        m_gl->glEnable(GL_SCISSOR_TEST);
        m_gl->glScissor(3, 4, 5, 6);
        m_gl->glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_FALSE);
        m_gl->glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        m_gl->glActiveTexture(GL_TEXTURE3);
        bool preserved = true;
        int steps = 0;
        QTemporaryDir temporary;
        QString error;
        QVERIFY2(CPsdFileTask::Execute(temporary.filePath("state.psd"), QSize(64, 64), false,
            [&](CPsdExportItem& item, QString* detail) {
                if (!render->Next(item, detail)) return false;
                ++steps;
                GLint value = 0, viewport[4]{};
                m_gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &value);
                preserved = preserved && value == GLint(read.GetRenderTexture());
                m_gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value);
                preserved = preserved && value == GLint(draw.GetRenderTexture());
                m_gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &value);
                preserved = preserved && value == GLint(vao);
                m_gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &value);
                preserved = preserved && value == GLint(buffer);
                m_gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value);
                preserved = preserved && value == GLint(buffer);
                m_gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
                preserved = preserved && value == GL_TEXTURE3;
                m_gl->glGetIntegerv(GL_VIEWPORT, viewport);
                preserved = preserved && viewport[0] == 1 && viewport[1] == 2 && viewport[2] == 11 && viewport[3] == 13;
                GLboolean mask[4]{};
                m_gl->glGetBooleanv(GL_COLOR_WRITEMASK, mask);
                preserved = preserved && !mask[0] && mask[1] && mask[2] && !mask[3] && m_gl->glIsEnabled(GL_SCISSOR_TEST);
                GLfloat clear[4]{};
                m_gl->glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
                preserved = preserved && clear[0] == 0.25f && clear[1] == 0.5f && clear[2] == 0.75f && clear[3] == 1.0f;
                return true;
            }, &control, &error), qPrintable(error));
        QVERIFY(render->Finish(true));
        QVERIFY(preserved);
        QVERIFY(steps > 4);
        m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_gl->glBindVertexArray(0);
        m_gl->glDeleteBuffers(1, &buffer);
        m_gl->glDeleteVertexArrays(1, &vao);
    }

    void invalidBindingsAndLostContextsFailWithoutWriting_data()
    {
        QTest::addColumn<bool>("canceled");
        QTest::newRow("context-lost") << false;
        QTest::newRow("cancel-then-context-lost") << true;
    }

    void invalidBindingsAndLostContextsFailWithoutWriting()
    {
        QFETCH(bool, canceled);
        const auto [model, renderer] = scene(true, true);
        const auto other = scene();
        CExportControl missingControl;
        auto missing = session(nullptr, nullptr, missingControl);
        QString error;
        QVERIFY(!missing->Begin(false, &error));
        QVERIFY(!error.isEmpty());
        CExportControl mismatchControl;
        auto mismatch = session(other.first, renderer, mismatchControl);
        QVERIFY(!mismatch->Begin(false, &error));
        CExportControl control;
        auto render = session(model, renderer, control);
        QVERIFY(render->Begin(false));
        if (canceled) control.Cancel();
        m_context->doneCurrent();
        m_context.reset();
        QTemporaryDir temporary;
        const QString path = temporary.filePath("existing.psd");
        QVERIFY(seed(path));
        QVERIFY(!CPsdFileTask::Execute(path, QSize(64, 64), false,
            [&](CPsdExportItem& item, QString* detail) { return render->Next(item, detail); }, &control, &error));
        QCOMPARE(control.WasCanceled(), canceled);
        QVERIFY(!render->Finish(false, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!control.WasCanceled());
        QVERIFY(!control.IsCommitted());
        QCOMPARE(bytes(path), QByteArray("original-project"));
    }

    void cleanupFailureAfterCommitPreservesCompletedFile()
    {
        const auto [model, renderer] = scene();
        CExportControl control;
        auto render = session(model, renderer, control, QSize(64, 64), false);
        QVERIFY(render->Begin(false));
        QTemporaryDir temporary;
        const QString path = temporary.filePath("completed.psd");
        QVERIFY(seed(path));
        QString error;
        QVERIFY2(CPsdFileTask::Execute(path, QSize(64, 64), false,
            [&](CPsdExportItem& item, QString* detail) { return render->Next(item, detail); }, &control, &error), qPrintable(error));
        QVERIFY(control.IsCommitted());
        const QByteArray completed = bytes(path);
        PsdBinary::Document document;
        QVERIFY2(PsdBinary::Parse(completed, document, &error), qPrintable(error));
        QCOMPARE(document.layers.size(), 1);
        m_context->doneCurrent();
        m_context.reset();
        QVERIFY(!render->Finish(true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(control.IsCommitted());
        QVERIFY(!control.WasCanceled());
        QCOMPARE(bytes(path), completed);
    }

    void pendingCleanupCanRetryWithoutExpiredControl_data()
    {
        QTest::addColumn<bool>("rendererAlreadyEnded");
        QTest::newRow("renderer-and-buffer-pending") << false;
        QTest::newRow("only-buffer-pending") << true;
    }

    void pendingCleanupCanRetryWithoutExpiredControl()
    {
        QFETCH(bool, rendererAlreadyEnded);
        const auto [model, renderer] = scene();
        auto control = std::make_unique<CExportControl>();
        bool activateAllowed = true;
        CubismMatrix44 matrix;
        QVector<CPsdRenderSession::Entry> entries{{CPsdExportItem::Type::Layer, 0, "layer", -1}};
        CPsdRenderSession render(m_surface.get(), m_context.get(), model, renderer, QSize(64, 64), matrix, entries,
            [&] { return activateAllowed && m_context->makeCurrent(m_surface.get()); },
            [](QString*) { return true; }, control.get());
        QString error;
        QVERIFY2(render.Begin(false, &error), qPrintable(error));
        const GLuint framebuffer = render.m_buffer.GetRenderTexture();
        const GLuint texture = render.m_buffer.GetColorBuffer();
        QVERIFY(m_gl->glIsFramebuffer(framebuffer));
        QVERIFY(m_gl->glIsTexture(texture));
        m_context->doneCurrent();
        activateAllowed = false;
        QVERIFY(!render.Finish(false, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(renderer->HasPsdExport());
        render.DetachControl();
        control.reset();
        activateAllowed = true;
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(m_gl->glIsFramebuffer(framebuffer));
        QVERIFY(m_gl->glIsTexture(texture));
        if (rendererAlreadyEnded) renderer->EndPsdExport(false);
        else {
            CPsdExportItem item;
            QVERIFY2(render.Next(item, &error), qPrintable(error));
            QCOMPARE(item.type, CPsdExportItem::Type::Layer);
            QVERIFY(!item.image.isNull());
        }
        QVERIFY2(render.Finish(false, &error), qPrintable(error));
        QVERIFY(!renderer->HasPsdExport());
        QVERIFY(!m_gl->glIsFramebuffer(framebuffer));
        QVERIFY(!m_gl->glIsTexture(texture));
        QVERIFY(render.Finish(false, &error));
        CExportControl nextControl;
        auto next = session(model, renderer, nextControl);
        QVERIFY(next->Begin(false, &error));
        QVERIFY(next->Finish(false, &error));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    PsdCooperativeGlTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "psd_cooperative_gl_tests.moc"
