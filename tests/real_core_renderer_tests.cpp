#include "framework/CubismFramework.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "live2dstruct/ccanvasgeometry.h"
#include "opengl/cpsdwritelist.h"
#include "psd/cpsdlist.h"
#include "psd_binary_reader.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;
using live2dstruct::CCanvasGeometry;
namespace Core = Live2D::Cubism::Core;

namespace {
std::unique_ptr<CubismIdManager> ids;

void deleteRenderer(CubismRenderer_OpenGLES2* renderer)
{
    CubismRenderer::Delete(renderer);
}

void* allocate(std::size_t size, std::size_t alignment = 0)
{
    if (alignment) return ::operator new(size, std::align_val_t(alignment));
    return std::malloc(size);
}

void release(void* pointer, std::size_t alignment = 0)
{
    if (alignment) ::operator delete(pointer, std::align_val_t(alignment));
    else std::free(pointer);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString digest(const QImage& image)
{
    const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
    return QString::fromLatin1(QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(normalized.constBits()), normalized.sizeInBytes()),
        QCryptographicHash::Sha256).toHex());
}

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

class CoreScene
{
public:
    bool load(const QString& path, QString* error)
    {
        source = readFile(path);
        if (source.isEmpty()) return fail(error, QStringLiteral("无法读取 MOC3: %1").arg(path));
        mocBacking.assign(source.size() + Core::csmAlignofMoc + 128, 0xa5);
        const auto raw = reinterpret_cast<std::uintptr_t>(mocBacking.data());
        moc = reinterpret_cast<unsigned char*>((raw + Core::csmAlignofMoc - 1) & ~(Core::csmAlignofMoc - 1));
        std::memcpy(moc, source.constData(), size_t(source.size()));
        if (!Core::csmHasMocConsistency(moc, unsigned(source.size())))
            return fail(error, QStringLiteral("Core 拒绝 MOC3 一致性校验"));
        std::memcpy(moc, source.constData(), size_t(source.size()));
        auto* revived = Core::csmReviveMocInPlace(moc, unsigned(source.size()));
        if (!revived) return fail(error, QStringLiteral("Core 无法 revive MOC3"));
        const unsigned modelSize = Core::csmGetSizeofModel(revived);
        if (!modelSize || modelSize > 512u * 1024u * 1024u)
            return fail(error, QStringLiteral("Core 返回无效模型大小"));
        model = static_cast<Core::csmModel*>(allocate(modelSize, Core::csmAlignofModel));
        model = Core::csmInitializeModelInPlace(revived, model, modelSize);
        if (!model) return fail(error, QStringLiteral("Core 无法初始化模型"));
        if (error) error->clear();
        return true;
    }

    Core::csmModel* value() const { return model; }
    const QByteArray& bytes() const { return source; }
    void disown() { model = nullptr; }

    ~CoreScene()
    {
        if (model) release(model, Core::csmAlignofModel);
    }

private:
    static bool fail(QString* error, const QString& message)
    {
        if (error) *error = message;
        return false;
    }

    QByteArray source;
    std::vector<unsigned char> mocBacking;
    unsigned char* moc = nullptr;
    Core::csmModel* model = nullptr;
};

struct RenderFixture
{
    QString mocPath;
    QString texturePath;
    QSize size;
    std::array<float, 16> matrix{};
    CoreScene source;
    CubismModel* model = nullptr;
    std::unique_ptr<CubismRenderer_OpenGLES2, void (*)(CubismRenderer_OpenGLES2*)> renderer{nullptr, &deleteRenderer};
    GLuint texture = 0;
};

bool matrixFor(Core::csmModel* model, const QSize& size, std::array<float, 16>& matrix, QString* error)
{
    Core::csmVector2 canvas{}, origin{};
    float pixelsPerUnit = 0;
    Core::csmReadCanvasInfo(model, &canvas, &origin, &pixelsPerUnit);
    if (!std::isfinite(canvas.X) || !std::isfinite(canvas.Y) || !std::isfinite(origin.X) ||
        !std::isfinite(origin.Y) || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0f ||
        size.isEmpty()) {
        if (error) *error = QStringLiteral("MOC3 canvas_info 无效");
        return false;
    }
    const std::array<double, 4> values{
        2.0 * double(pixelsPerUnit) / size.width(),
        2.0 * double(pixelsPerUnit) / size.height(),
        2.0 * double(origin.X) / size.width() - 1.0,
        1.0 - 2.0 * double(origin.Y) / size.height()};
    matrix.fill(0.0f);
    matrix[0] = float(values[0]);
    matrix[5] = float(values[1]);
    matrix[10] = 1.0f;
    matrix[12] = float(values[2]);
    matrix[13] = float(values[3]);
    matrix[15] = 1.0f;
    for (float value : matrix)
        if (!std::isfinite(value)) {
            if (error) *error = QStringLiteral("MOC3 canvas_info 产生非有限 MVP");
            return false;
        }
    return true;
}

bool makeTexture(QOpenGLFunctions_3_3_Core& gl, const QString& path, GLuint& output, QString* error)
{
    QImage image(path);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("无法读取模型纹理: %1").arg(path);
        return false;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    gl.glGenTextures(1, &output);
    gl.glBindTexture(GL_TEXTURE_2D, output);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, image.constBits());
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (gl.glGetError() != GL_NO_ERROR) {
        gl.glDeleteTextures(1, &output);
        output = 0;
        if (error) *error = QStringLiteral("模型纹理上传失败");
        return false;
    }
    return true;
}

QString inferredTexture(const QString& mocPath)
{
    const QFileInfo info(mocPath);
    const QDir directory = info.dir();
    const QString stem = info.completeBaseName();
    const QStringList candidates{
        directory.filePath(stem + QStringLiteral(".4096/texture_00.png")),
        directory.filePath(stem + QStringLiteral(".2048/texture_00.png")),
        directory.filePath(QStringLiteral("texture_00.png")),
        directory.filePath(QStringLiteral("../") + stem + QStringLiteral(".4096/texture_00.png")),
        directory.filePath(QStringLiteral("../") + stem + QStringLiteral(".2048/texture_00.png"))};
    for (const QString& candidate : candidates)
        if (QFileInfo::exists(candidate)) return candidate;
    return {};
}

QByteArray readLayerPixels(const PsdBinary::Layer& layer)
{
    QByteArray result;
    result.reserve(layer.bounds.width() * layer.bounds.height() * 4);
    for (int y = 0; y < layer.bounds.height(); ++y)
        for (int x = 0; x < layer.bounds.width(); ++x) {
            const int index = y * layer.bounds.width() + x;
            result.append(layer.Plane(0).at(index));
            result.append(layer.Plane(1).at(index));
            result.append(layer.Plane(2).at(index));
            result.append(layer.Plane(-1).at(index));
        }
    return result;
}
}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size); }
void operator delete(void* pointer, CubismAllocationTag) noexcept { release(pointer); }
void* operator new(csmSizeType size, csmUint32 alignment, CubismAllocationAlignedTag) { return allocate(size, alignment); }
void operator delete(void* pointer, CubismAllocationAlignedTag) noexcept { release(pointer, Core::csmAlignofModel); }

namespace Live2D { namespace Cubism { namespace Framework {
namespace Constant { const csmInt32 VertexOffset = 0; const csmInt32 VertexStep = 2; }
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size); }
void CubismFramework::Deallocate(void* pointer) { release(pointer); }
void* CubismFramework::AllocateAligned(csmSizeType size, csmUint32 alignment) { return allocate(size, alignment); }
void CubismFramework::DeallocateAligned(void* pointer) { release(pointer, Core::csmAlignofModel); }
CubismIdManager* CubismFramework::GetIdManager() { return ids.get(); }
void CubismFramework::CoreLogFunction(const csmChar*) {}
CubismFramework::Option::LogLevel CubismFramework::GetLoggingLevel() { return CubismFramework::Option::LogLevel_Off; }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
}}}

LAppDelegate* LAppDelegate::GetInstance() { return nullptr; }
QOpenGLContext* LAppDelegate::getGlOwningContext() { return QOpenGLContext::currentContext(); }
const QWidget* LAppDelegate::GetWindow() { return nullptr; }
bool LAppDelegate::IsOffScreen() { return false; }
void LAppDelegate::SetIsOffset(bool) {}
CPsdWriteList* LAppDelegate::GetCPsdWriteList() { return nullptr; }

class RealCoreRendererTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOpenGLContext> context;
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QTemporaryDir> psdDirectory;
    QOpenGLFunctions_3_3_Core* gl = nullptr;

    QString modelPath() const { return qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC"); }
    QString texturePath(const QString& mocPath) const
    {
        const QString configured = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_TEXTURE");
        return configured.isEmpty() ? inferredTexture(mocPath) : configured;
    }

    void destroy(RenderFixture& fixture)
    {
        fixture.renderer.reset();
        if (fixture.texture) {
            gl->glDeleteTextures(1, &fixture.texture);
            fixture.texture = 0;
        }
        if (fixture.model) {
            fixture.source.disown();
            delete fixture.model;
            fixture.model = nullptr;
        }
    }

    bool prepare(RenderFixture& fixture, const QSize& output, QString* error)
    {
        fixture.mocPath = modelPath();
        fixture.texturePath = texturePath(fixture.mocPath);
        if (fixture.mocPath.isEmpty() || fixture.texturePath.isEmpty()) return false;
        if (!fixture.source.load(fixture.mocPath, error)) return false;
        fixture.size = output;
        if (!matrixFor(fixture.source.value(), output, fixture.matrix, error)) return false;
        fixture.model = new CubismModel(fixture.source.value());
        fixture.model->Initialize();
        fixture.model->InitMesh();
        fixture.renderer.reset(static_cast<CubismRenderer_OpenGLES2*>(CubismRenderer::Create()));
        if (!fixture.renderer || !fixture.renderer->setGlOwningContext(context.get()) ||
            !fixture.renderer->Initialize(fixture.model)) {
            if (error) *error = QStringLiteral("当前 renderer 初始化失败");
            return false;
        }
        if (!makeTexture(*gl, fixture.texturePath, fixture.texture, error)) return false;
        int maxTextureIndex = -1;
        const auto* textureIndices = Core::csmGetDrawableTextureIndices(fixture.source.value());
        for (int index = 0; index < fixture.model->GetDrawableCount(); ++index)
            maxTextureIndex = std::max(maxTextureIndex, textureIndices[index]);
        if (maxTextureIndex < 0 || maxTextureIndex > 64) {
            if (error) *error = QStringLiteral("模型纹理索引无效");
            return false;
        }
        for (int index = 0; index <= maxTextureIndex; ++index)
            fixture.renderer->BindTexture(index, fixture.texture);
        return true;
    }

    QImage renderFrame(RenderFixture& fixture, QString* error)
    {
        QOpenGLFramebufferObject target(fixture.size, QOpenGLFramebufferObject::CombinedDepthStencil);
        if (!target.isValid() || !target.bind()) {
            if (error) *error = QStringLiteral("无法创建模型帧 FBO");
            return {};
        }
        const auto restore = qScopeGuard([&] { target.release(); });
        gl->glViewport(0, 0, fixture.size.width(), fixture.size.height());
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glClearColor(0, 0, 0, 0);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        CubismMatrix44 matrix;
        matrix.SetMatrix(fixture.matrix.data());
        fixture.renderer->SetMvpMatrix(&matrix);
        CubismRenderer_OpenGLES2::SnapshotScene scene;
        if (!fixture.renderer->DrawSnapshotModel(scene, error)) return {};
        return target.toImage().convertToFormat(QImage::Format_RGBA8888);
    }

    bool exportPsd(RenderFixture& fixture, QString& path, QString* error)
    {
        psdDirectory = std::make_unique<QTemporaryDir>();
        if (!psdDirectory->isValid()) return false;
        path = psdDirectory->filePath(QStringLiteral("ren-render.psd"));
        CPsdList writer;
        if (!writer.SetPsd(path, fixture.size.width(), fixture.size.height())) return false;
        QOpenGLFramebufferObject target(fixture.size, QOpenGLFramebufferObject::CombinedDepthStencil);
        if (!target.isValid() || !target.bind()) return false;
        const auto restore = qScopeGuard([&] { target.release(); });
        CubismMatrix44 matrix;
        matrix.SetMatrix(fixture.matrix.data());
        if (!fixture.renderer->BeginPsdExport(true, nullptr, error)) return false;
        auto endExport = qScopeGuard([&] { fixture.renderer->EndPsdExport(false); });
        for (int index = 0; index < fixture.model->GetDrawableCount(); ++index) {
            QImage image;
            QRect rect;
            bool ready = false;
            for (int step = 0; step < 4096 && !ready; ++step)
                if (!fixture.renderer->DrawPsdStep(index, -1.0f, matrix, fixture.size, image, rect, ready, error)) return false;
            if (!ready) {
                if (error) *error = QStringLiteral("PSD 图层 %1 未在步进上限内完成").arg(index);
                return false;
            }
            const QString name = QString::fromUtf8(fixture.model->GetDrawableId(index)->GetString().GetRawString());
            if (rect.isEmpty()) {
                if (!writer.AddEmptyLayer(name)) return false;
            } else if (!writer.AddLayer(name, image, rect, 255,
                                        CPsdWriteList::RawBlendModeToPsd(fixture.model->GetDrawableBlendModeValue(index)),
                                        false, true, quint32(index + 1))) return false;
        }
        if (!writer.CreatePsd()) return false;
        endExport.dismiss();
        fixture.renderer->EndPsdExport(true);
        return true;
    }

private slots:
    void initTestCase()
    {
        ids = std::make_unique<CubismIdManager>();
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(0);
        context = std::make_unique<QOpenGLContext>();
        context->setFormat(format);
        if (!context->create() || !context->isValid()) QSKIP("当前环境没有 OpenGL 3.3 Core");
        surface = std::make_unique<QOffscreenSurface>();
        surface->setFormat(context->format());
        surface->create();
        if (!surface->isValid() || !context->makeCurrent(surface.get())) QSKIP("无法建立 OpenGL 离屏上下文");
        gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context.get());
        if (!gl || !gl->initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 函数不可用");
    }

    void renCore6FrameAndPsdParity()
    {
        const QString mocPath = modelPath();
        const QString texture = texturePath(mocPath);
        if (mocPath.isEmpty() || texture.isEmpty())
            QSKIP("设置 LIVE2D_REAL_CORE_EXTRA_MOC 与 LIVE2D_REAL_CORE_EXTRA_TEXTURE 后运行官方 Ren fixture");
        RenderFixture fixture;
        const auto cleanup = qScopeGuard([&] { destroy(fixture); });
        QString error;
        QVERIFY2(prepare(fixture, QSize(256, 256), &error), qPrintable(error));
        const QImage first = renderFrame(fixture, &error);
        QVERIFY2(!first.isNull(), qPrintable(error));
        const QString firstHash = digest(first);
        const QImage second = renderFrame(fixture, &error);
        QVERIFY2(!second.isNull(), qPrintable(error));
        const QString secondHash = digest(second);
        QCOMPARE(firstHash, secondHash);
        qInfo().noquote() << "Ren frame SHA-256:" << firstHash
                          << "drawables:" << fixture.model->GetDrawableCount()
                          << "offscreens:" << fixture.model->GetOffscreenCount();

        QString psdPath;
        QVERIFY2(exportPsd(fixture, psdPath, &error), qPrintable(error));
        const QByteArray psdBytes = readFile(psdPath);
        QVERIFY(!psdBytes.isEmpty());
        PsdBinary::Document document;
        QVERIFY2(PsdBinary::Parse(psdBytes, document, &error), qPrintable(error));
        QCOMPARE(document.canvas, fixture.size);
        QCOMPARE(document.layers.size(), fixture.model->GetDrawableCount());
        QByteArray layerDigest;
        for (int index = 0; index < document.layers.size(); ++index) {
            const auto& layer = document.layers[index];
            QCOMPARE(layer.name, QString::fromUtf8(fixture.model->GetDrawableId(index)->GetString().GetRawString()));
            layerDigest += digest(readLayerPixels(layer)).toUtf8();
        }
        qInfo().noquote() << "Ren PSD SHA-256:" << digest(psdBytes)
                          << "layer-pixels SHA-256:" << digest(layerDigest);
        QVERIFY(!document.layers.isEmpty());
    }

    void cleanupTestCase()
    {
        if (context && surface && context->makeCurrent(surface.get())) {
            CubismRenderer::StaticRelease();
            context->doneCurrent();
        }
        ids.reset();
    }
};

QTEST_MAIN(RealCoreRendererTests)
#include "real_core_renderer_tests.moc"
