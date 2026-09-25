#include "live2d/LAppTextureManager.hpp"
#include "live2d/LAppPal.hpp"

#include <QtTest>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace {
std::string pathBytes(const QString& path) { return path.toUtf8().toStdString(); }

QByteArray sourcePixels()
{
    return QByteArray::fromHex("ff800100c86432018040ff7fe6b45a800dc8f0fe2c4258ff");
}

bool savePng(const QString& path)
{
    const QByteArray pixels = sourcePixels();
    QImage image(3, 2, QImage::Format_RGBA8888);
    if (image.isNull() || image.bytesPerLine() != 12) return false;
    std::memcpy(image.bits(), pixels.constData(), pixels.size());
    return image.save(path, "PNG");
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray expectedPixels()
{
    QByteArray pixels = sourcePixels();
#ifdef PREMULTIPLIED_ALPHA_ENABLE
    for (int offset = 0; offset < pixels.size(); offset += 4) {
        const unsigned alpha = static_cast<unsigned char>(pixels[offset + 3]);
        for (int channel = 0; channel < 3; ++channel) {
            const unsigned component = static_cast<unsigned char>(pixels[offset + channel]);
            pixels[offset + channel] = static_cast<char>(component * (alpha + 1) / 256);
        }
    }
#endif
    return pixels;
}

quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xffffffffu;
    for (char value : data) {
        crc ^= static_cast<quint8>(value);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

QByteArray resizedHeader(QByteArray png, quint32 width, quint32 height)
{
    if (png.size() < 33 || png.mid(12, 4) != "IHDR") return {};
    qToBigEndian<quint32>(width, reinterpret_cast<uchar*>(png.data() + 16));
    qToBigEndian<quint32>(height, reinterpret_cast<uchar*>(png.data() + 20));
    qToBigEndian<quint32>(crc32(png.mid(12, 17)), reinterpret_cast<uchar*>(png.data() + 29));
    return png;
}

struct UploadState
{
    GLint active = 0, binding = 0, buffer = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    bool operator==(const UploadState& other) const
    {
        return active == other.active && binding == other.binding && buffer == other.buffer &&
            alignment == other.alignment && rowLength == other.rowLength &&
            skipPixels == other.skipPixels && skipRows == other.skipRows;
    }
};

class TextureManagerProbe : public LAppTextureManager
{
public:
    QOpenGLContext* Context() const { return owningContext(); }
    bool Initialized() const { return isInitialized(); }
};
}

class TextureLoaderTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    QOpenGLFunctions_3_3_Core m_gl;

    UploadState uploadState()
    {
        UploadState state;
        m_gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active);
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.binding);
        m_gl.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state.buffer);
        m_gl.glGetIntegerv(GL_UNPACK_ALIGNMENT, &state.alignment);
        m_gl.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &state.rowLength);
        m_gl.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &state.skipPixels);
        m_gl.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &state.skipRows);
        return state;
    }

    QByteArray gpuPixels(GLuint texture)
    {
        GLint binding, packBuffer, alignment, rowLength, skipPixels, skipRows;
        m_gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        m_gl.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        m_gl.glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        m_gl.glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        m_gl.glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        m_gl.glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        const auto restore = qScopeGuard([&] {
            m_gl.glBindTexture(GL_TEXTURE_2D, binding);
            m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
            m_gl.glPixelStorei(GL_PACK_ALIGNMENT, alignment);
            m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
            m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
            m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        });
        m_gl.glBindTexture(GL_TEXTURE_2D, texture);
        m_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        m_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        m_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        GLint width = 0, height = 0;
        m_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        m_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        if (width != 3 || height != 2) return {};
        QByteArray pixels(24, '\0');
        m_gl.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
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
        if (actual.renderableType() != QSurfaceFormat::OpenGL || actual.profile() != QSurfaceFormat::CoreProfile ||
            actual.majorVersion() < 3 || (actual.majorVersion() == 3 && actual.minorVersion() < 3))
            QSKIP("The returned context does not provide OpenGL 3.3 Core.");
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(actual);
        m_surface->create();
        if (!m_surface->isValid() || !m_context->makeCurrent(m_surface.get()))
            QSKIP("Unable to make the offscreen surface current.");
        if (!m_gl.initializeOpenGLFunctions()) QSKIP("OpenGL 3.3 Core functions are unavailable.");
        qInfo() << "Texture loader GL:" << reinterpret_cast<const char*>(m_gl.glGetString(GL_VERSION))
                << reinterpret_cast<const char*>(m_gl.glGetString(GL_RENDERER));
#ifdef PREMULTIPLIED_ALPHA_ENABLE
        qInfo() << "Texture loader alpha: premultiplied";
#else
        qInfo() << "Texture loader alpha: straight";
#endif
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void init()
    {
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void validPngUploadsExactPixelsAndRestoresUnpackState()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("纹理 alpha.png"));
        QVERIFY(savePng(path));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        GLuint sentinelTexture = 0, sentinelBuffer = 0;
        m_gl.glGenTextures(1, &sentinelTexture);
        m_gl.glGenBuffers(1, &sentinelBuffer);
        const auto reset = qScopeGuard([&] {
            m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_gl.glDeleteBuffers(1, &sentinelBuffer);
            m_gl.glDeleteTextures(1, &sentinelTexture);
            m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            m_gl.glActiveTexture(GL_TEXTURE0);
        });
        m_gl.glActiveTexture(GL_TEXTURE3);
        m_gl.glBindTexture(GL_TEXTURE_2D, sentinelTexture);
        m_gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sentinelBuffer);
        m_gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
        m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 9);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        m_gl.glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        const UploadState before = uploadState();
        auto* info = manager.CreateTextureFromPngFile(pathBytes(path));
        QVERIFY(info);
        QCOMPARE(info->width, 3);
        QCOMPARE(info->height, 2);
        QCOMPARE(QString::fromStdString(info->fileName), path);
        QVERIFY(info->id != 0 && m_gl.glIsTexture(info->id));
        QVERIFY(uploadState() == before);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QCOMPARE(gpuPixels(info->id), expectedPixels());
        QVERIFY(uploadState() == before);
        m_gl.glBindTexture(GL_TEXTURE_2D, info->id);
        GLint mipWidth = 0, mipHeight = 0, minFilter = 0, magFilter = 0;
        m_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &mipWidth);
        m_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_HEIGHT, &mipHeight);
        m_gl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        m_gl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        m_gl.glBindTexture(GL_TEXTURE_2D, before.binding);
        QCOMPARE(mipWidth, 1);
        QCOMPARE(mipHeight, 1);
        QCOMPARE(minFilter, GLint(GL_LINEAR_MIPMAP_LINEAR));
        QCOMPARE(magFilter, GLint(GL_LINEAR));
        QCOMPARE(manager.GetTextureInfoByName(pathBytes(path)), info);
        QCOMPARE(manager.GetTextureInfoById(info->id), info);
        QCOMPARE(manager.CreateTextureFromPngFile(pathBytes(path)), info);
        QVERIFY(uploadState() == before);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void failedInputsNeverEnterCacheAndCanBeRetried()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString valid = temporary.filePath("valid.png");
        QVERIFY(savePng(valid));
        const QByteArray png = readFile(valid);
        QVERIFY(png.size() > 33);
        const QString missing = temporary.filePath("missing.png");
        const QString empty = temporary.filePath("empty.png");
        const QString corrupt = temporary.filePath("corrupt.png");
        const QString truncated = temporary.filePath("truncated.png");
        QVERIFY(writeFile(empty, {}));
        QVERIFY(writeFile(corrupt, "not a PNG"));
        QVERIFY(writeFile(truncated, png.left(33)));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        auto* retained = manager.CreateTextureFromPngFile(pathBytes(valid));
        QVERIFY(retained);
        manager.SetModelTextures({{0, valid}});
        const QMap<int, QString> mapping = manager.GetModelTex();
        const UploadState before = uploadState();
        for (const QString& path : {missing, empty, corrupt, truncated}) {
            QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(path)));
            QVERIFY(!manager.GetTextureInfoByName(pathBytes(path)));
            QCOMPARE(manager.GetTextureInfoByName(pathBytes(valid)), retained);
            QCOMPARE(manager.GetModelTex(), mapping);
            QVERIFY(uploadState() == before);
            QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        }
        QVERIFY(savePng(corrupt));
        auto* retried = manager.CreateTextureFromPngFile(pathBytes(corrupt));
        QVERIFY(retried && retried != retained);
        QCOMPARE(gpuPixels(retried->id), expectedPixels());
        QCOMPARE(manager.GetTextureInfoByName(pathBytes(valid)), retained);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        for (const QString& path : {missing, empty}) {
            Csm::csmSizeInt size = 123;
            Csm::csmByte* bytes = LAppPal::LoadFileAsBytes(pathBytes(path), &size);
            const auto release = qScopeGuard([&] { LAppPal::ReleaseBytes(bytes); });
            QVERIFY(!bytes);
            QCOMPARE(size, Csm::csmSizeInt(0));
        }
    }

    void oversizedHeadersAreRejectedWithoutAllocatingImages()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath("source.png");
        QVERIFY(savePng(source));
        const QByteArray png = readFile(source);
        GLint maximum = 0;
        m_gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        QVERIFY(maximum > 0 && maximum < std::numeric_limits<int>::max());
        const std::array<std::array<quint32, 2>, 5> sizes{{
            {static_cast<quint32>(maximum) + 1, 1}, {1, static_cast<quint32>(maximum) + 1},
            {46341, 46341}, {0xffffffffu, 2}, {0, 1}
        }};
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        for (const auto& size : sizes) {
            const QString path = temporary.filePath(QStringLiteral("%1-%2.png").arg(size[0]).arg(size[1]));
            const QByteArray encoded = resizedHeader(png, size[0], size[1]);
            QCOMPARE(encoded.size(), png.size());
            QVERIFY(writeFile(path, encoded));
            QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(path)));
            QVERIFY(!manager.GetTextureInfoByName(pathBytes(path)));
            QVERIFY(manager.GetModelTex().isEmpty());
            QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        }
    }

    void missingOrForeignContextPreservesExistingTextures()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("owned.png");
        const QString second = temporary.filePath("new.png");
        QVERIFY(savePng(path) && savePng(second));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        auto* retained = manager.CreateTextureFromPngFile(pathBytes(path));
        QVERIFY(retained);
        const GLuint ownedId = retained->id;
        manager.SetModelTextures({{0, path}});
        const auto restoreOwner = qScopeGuard([&] { m_context->makeCurrent(m_surface.get()); });
        m_context->doneCurrent();
        manager.setGlOwningContext(nullptr);
        manager.setGlOwningContext(m_context.get());
        QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(path)));
        QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(second)));
        manager.ReleaseTextures();
        QCOMPARE(manager.GetTextureInfoByName(pathBytes(path)), retained);
        QCOMPARE(manager.GetModelTex().value(0), path);
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(m_gl.glIsTexture(ownedId));
        {
            QOpenGLContext foreign;
            foreign.setFormat(m_context->format());
            if (!foreign.create() || !foreign.isValid()) QSKIP("The platform could not create a second independent GL context.");
            QOffscreenSurface surface;
            surface.setFormat(foreign.format());
            surface.create();
            if (!surface.isValid() || !foreign.makeCurrent(&surface)) QSKIP("The second offscreen context could not become current.");
            const auto detachForeign = qScopeGuard([&] { foreign.doneCurrent(); });
            QOpenGLFunctions_3_3_Core gl;
            QVERIFY(gl.initializeOpenGLFunctions());
            GLuint sentinel = 0;
            gl.glGenTextures(1, &sentinel);
            gl.glBindTexture(GL_TEXTURE_2D, sentinel);
            const auto releaseSentinel = qScopeGuard([&] { gl.glDeleteTextures(1, &sentinel); });
            QVERIFY(gl.glIsTexture(sentinel));
            manager.setGlOwningContext(&foreign);
            QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(path)));
            QVERIFY(!manager.CreateTextureFromPngFile(pathBytes(second)));
            manager.ReleaseTexture(ownedId);
            manager.ReleaseTexture(pathBytes(path));
            manager.ReleaseUnusedTextures({});
            manager.ReleaseTextures();
            QCOMPARE(manager.GetTextureInfoByName(pathBytes(path)), retained);
            QVERIFY(!manager.GetTextureInfoByName(pathBytes(second)));
            QCOMPARE(manager.GetModelTex().value(0), path);
            QVERIFY(gl.glIsTexture(sentinel));
            QCOMPARE(gl.glGetError(), GLenum(GL_NO_ERROR));
        }
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        QVERIFY(m_gl.glIsTexture(ownedId));
        QCOMPARE(gpuPixels(ownedId), expectedPixels());
        auto* created = manager.CreateTextureFromPngFile(pathBytes(second));
        QVERIFY(created);
        manager.ReleaseTextures();
        QVERIFY(!m_gl.glIsTexture(ownedId));
        QVERIFY(manager.GetModelTex().isEmpty());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void contextDestroyedBeforeManagerClearsQtOwnerAndPreservesForeignTextures()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("context-owned.png");
        QVERIFY(savePng(path));
        auto manager = std::make_unique<TextureManagerProbe>();
        auto owner = std::make_unique<QOpenGLContext>();
        owner->setFormat(m_context->format());
        QVERIFY(owner->create());
        QOffscreenSurface surface;
        surface.setFormat(owner->format());
        surface.create();
        QVERIFY(surface.isValid());
        manager->setGlOwningContext(owner.get());
        QVERIFY(!manager->Context());
        QVERIFY(!manager->Initialized());
        QVERIFY(owner->makeCurrent(&surface));
        manager->setGlOwningContext(owner.get());
        QCOMPARE(manager->Context(), owner.get());
        QVERIFY(manager->Initialized());
        QVERIFY(manager->CreateTextureFromPngFile(pathBytes(path)));
        manager->SetModelTextures({{0, path}});
        owner->doneCurrent();
        owner.reset();
        QVERIFY(!manager->Context());
        QVERIFY(!manager->Initialized());
        QVERIFY(m_context->makeCurrent(m_surface.get()));
        GLuint sentinel = 0;
        m_gl.glGenTextures(1, &sentinel);
        m_gl.glBindTexture(GL_TEXTURE_2D, sentinel);
        const auto cleanup = qScopeGuard([&] { m_gl.glDeleteTextures(1, &sentinel); });
        QVERIFY(m_gl.glIsTexture(sentinel));
        QVERIFY(!manager->CreateTextureFromPngFile(pathBytes(path)));
        manager->ReleaseTextures();
        QVERIFY(!manager->GetTextureInfoByName(pathBytes(path)));
        QVERIFY(manager->GetModelTex().isEmpty());
        manager.reset();
        QVERIFY(m_gl.glIsTexture(sentinel));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void allReleaseEntrypointsDeleteGpuObjectsAndMetadata()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString a = temporary.filePath("a.png"), b = temporary.filePath("b.png"), c = temporary.filePath("c.png");
        QVERIFY(savePng(a) && savePng(b) && savePng(c));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        const auto* first = manager.CreateTextureFromPngFile(pathBytes(a));
        const auto* second = manager.CreateTextureFromPngFile(pathBytes(b));
        const auto* third = manager.CreateTextureFromPngFile(pathBytes(c));
        QVERIFY(first && second && third);
        const GLuint firstId = first->id, secondId = second->id, thirdId = third->id;
        manager.SetModelTextures({{0, a}, {1, b}, {2, c}, {3, a}});
        manager.ReleaseTexture(firstId);
        QVERIFY(!m_gl.glIsTexture(firstId));
        QVERIFY(!manager.GetTextureInfoById(firstId));
        QVERIFY(!manager.GetTextureInfoByName(pathBytes(a)));
        QVERIFY(!manager.GetModelTex().contains(0) && !manager.GetModelTex().contains(3));
        manager.ReleaseTexture(pathBytes(b));
        QVERIFY(!m_gl.glIsTexture(secondId));
        QVERIFY(!manager.GetTextureInfoByName(pathBytes(b)));
        QCOMPARE(manager.GetModelTex(), (QMap<int, QString>{{2, c}}));
        QVERIFY(m_gl.glIsTexture(thirdId));
        manager.ReleaseTextures();
        QVERIFY(!m_gl.glIsTexture(thirdId));
        QVERIFY(!manager.GetTextureInfoById(thirdId));
        QVERIFY(manager.GetModelTex().isEmpty());
        manager.ReleaseTexture(firstId);
        manager.ReleaseTexture(pathBytes(b));
        manager.ReleaseTextures();
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void unusedReleasePreservesOnlyRetainedNames()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString a = temporary.filePath("a.png"), b = temporary.filePath("b.png"), c = temporary.filePath("c.png");
        QVERIFY(savePng(a) && savePng(b) && savePng(c));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        auto* first = manager.CreateTextureFromPngFile(pathBytes(a));
        auto* second = manager.CreateTextureFromPngFile(pathBytes(b));
        auto* third = manager.CreateTextureFromPngFile(pathBytes(c));
        QVERIFY(first && second && third);
        const GLuint firstId = first->id, secondId = second->id, thirdId = third->id;
        manager.SetModelTextures({{0, a}, {1, b}, {2, c}});
        manager.ReleaseUnusedTextures({a, c});
        QCOMPARE(manager.GetTextureInfoByName(pathBytes(a)), first);
        QCOMPARE(manager.GetTextureInfoByName(pathBytes(c)), third);
        QVERIFY(!manager.GetTextureInfoByName(pathBytes(b)));
        QVERIFY(m_gl.glIsTexture(firstId) && m_gl.glIsTexture(thirdId));
        QVERIFY(!m_gl.glIsTexture(secondId));
        QCOMPARE(manager.GetModelTex(), (QMap<int, QString>{{0, a}, {2, c}}));
        manager.ReleaseUnusedTextures({a, c});
        QCOMPARE(manager.GetTextureInfoById(firstId), first);
        manager.ReleaseUnusedTextures({});
        QVERIFY(!m_gl.glIsTexture(firstId) && !m_gl.glIsTexture(thirdId));
        QVERIFY(manager.GetModelTex().isEmpty());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void destructorDeletesTexturesWithTheOwnerCurrent()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("owned.png");
        QVERIFY(savePng(path));
        GLuint id = 0;
        {
            LAppTextureManager manager;
            manager.setGlOwningContext(m_context.get());
            const auto* info = manager.CreateTextureFromPngFile(pathBytes(path));
            QVERIFY(info);
            id = info->id;
            QVERIFY(m_gl.glIsTexture(id));
        }
        QVERIFY(!m_gl.glIsTexture(id));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
    }

    void uploadsLeavePreexistingGlErrorsAvailableToTheCaller()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("owned.png");
        QVERIFY(savePng(path));
        LAppTextureManager manager;
        manager.setGlOwningContext(m_context.get());
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 3);
        const auto* info = manager.CreateTextureFromPngFile(pathBytes(path));
        QVERIFY(info);
        QCOMPARE(m_gl.glGetError(), GLenum(GL_INVALID_VALUE));
        QCOMPARE(m_gl.glGetError(), GLenum(GL_NO_ERROR));
        QCOMPARE(gpuPixels(info->id), expectedPixels());
    }

    void cleanupTestCase()
    {
        if (m_context) m_context->doneCurrent();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    TextureLoaderTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "texture_loader_tests.moc"
