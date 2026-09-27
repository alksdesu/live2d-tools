/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppTextureManager.hpp"
#include "LAppPal.hpp"
#include <QCryptographicHash>
#include <QScopeGuard>
#include <algorithm>
#include <limits>

#include "std/cpngimage.h"
#include <QImage>
#include <exception>

LAppTextureManager::LAppTextureManager() = default;

LAppTextureManager::~LAppTextureManager()
{
    ReleaseTextures();
}

void LAppTextureManager::setGlOwningContext(const QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context ||
        (m_context && m_context != context) || (owningContext() && owningContext() != context)) return;
    if (!initializeOpenGLFunctions()) return;
    if (m_context != context && !m_textures.empty()) {
        ReleaseTextures();
        if (!m_textures.empty()) return;
    }
    m_context = const_cast<QOpenGLContext*>(context);
}

bool LAppTextureManager::CanUseContext()
{
    return m_context && m_context->isValid() && QOpenGLContext::currentContext() == m_context &&
           owningContext() == m_context &&
           initializeOpenGLFunctions();
}

LAppTextureManager::TextureInfo* LAppTextureManager::CreateTextureFromPngFile(std::string fileName)
{
    if (!CanUseContext()) return nullptr;
    if (auto* cached = GetTextureInfoByName(fileName)) return cached;

    Csm::csmSizeInt size = 0;
    std::unique_ptr<Csm::csmByte, decltype(&LAppPal::ReleaseBytes)> address(
        LAppPal::LoadFileAsBytes(fileName, &size), &LAppPal::ReleaseBytes);
    if (!address || size == 0 || size > static_cast<Csm::csmSizeInt>(std::numeric_limits<int>::max())) return nullptr;
    return UploadPng(QByteArray(reinterpret_cast<const char*>(address.get()), int(size)), std::move(fileName), {}, nullptr);
}

LAppTextureManager::TextureInfo* LAppTextureManager::CreateTextureFromPngBytes(const QByteArray& png,
                                                                             const QSize& expectedSize, QString* error)
{
    if (error) error->clear();
    if (!expectedSize.isValid() || expectedSize.isEmpty()) {
        if (error) *error = QStringLiteral("替换纹理缺少有效原始尺寸");
        return nullptr;
    }
    return UploadPng(png, {}, expectedSize, error);
}

LAppTextureManager::TextureInfo* LAppTextureManager::UploadPng(const QByteArray& png, std::string fileName,
                                                            const QSize& expectedSize, QString* error)
{
    const auto fail = [&](const QString& message) -> TextureInfo* { if (error) *error = message; return nullptr; };
    if (!CanUseContext()) return fail(QStringLiteral("纹理上传需要所属 OpenGL 上下文"));
    try {
        QSize size;
        if (!CPngImage::Size(png, size, error)) return nullptr;
        if (expectedSize.isValid() && size != expectedSize) return fail(QStringLiteral("替换纹理尺寸必须保持 %1×%2").arg(expectedSize.width()).arg(expectedSize.height()));
        const int width = size.width(), height = size.height();
        GLint maximumSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumSize);
        if (width > maximumSize || height > maximumSize) return fail(QStringLiteral("纹理尺寸超过 OpenGL 支持范围"));
        QImage pixels;
        if (!CPngImage::Decode(png, pixels, error)) return nullptr;
#ifdef PREMULTIPLIED_ALPHA_ENABLE
        for (qint64 pixel = 0; pixel < qint64(width) * height; ++pixel) {
            uchar* color = pixels.bits() + pixel * 4;
            color[0] = color[0] * (color[3] + 1) >> 8;
            color[1] = color[1] * (color[3] + 1) >> 8;
            color[2] = color[2] * (color[3] + 1) >> 8;
        }
#endif

        GLint binding = 0, unpackBuffer = 0, alignment = 0, rowLength = 0, skipRows = 0, skipPixels = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        const auto restore = qScopeGuard([&] {
            glBindTexture(GL_TEXTURE_2D, binding);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
            glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        });
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        GLuint textureId = 0;
        glGenTextures(1, &textureId);
        if (!textureId) return fail(QStringLiteral("无法创建 OpenGL 纹理"));
        const auto release = qScopeGuard([&] { if (textureId) glDeleteTextures(1, &textureId); });
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.constBits());
        GLint uploadedWidth = 0, uploadedHeight = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &uploadedWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &uploadedHeight);
        if (uploadedWidth != width || uploadedHeight != height) return fail(QStringLiteral("OpenGL 纹理上传失败"));
        glGenerateMipmap(GL_TEXTURE_2D);
        for (int level = 1, mipWidth = width, mipHeight = height; mipWidth > 1 || mipHeight > 1; ++level) {
            mipWidth = std::max(1, mipWidth / 2);
            mipHeight = std::max(1, mipHeight / 2);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &uploadedWidth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &uploadedHeight);
            if (uploadedWidth != mipWidth || uploadedHeight != mipHeight) return fail(QStringLiteral("OpenGL 纹理缩略层创建失败"));
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        auto info = std::make_unique<TextureInfo>();
        info->fileName = std::move(fileName);
        info->width = width;
        info->height = height;
        info->id = textureId;
        info->pngBytes = QByteArray(png.constData(), png.size());
        info->sourceDigest = QCryptographicHash::hash(info->pngBytes, QCryptographicHash::Sha256);
        TextureInfo* result = info.get();
        m_textures.push_back(std::move(info));
        textureId = 0;
        return result;
    } catch (const std::exception&) { return fail(QStringLiteral("无法准备纹理内存或 OpenGL 资源")); }
}

void LAppTextureManager::RemoveModelTexture(const std::string& fileName)
{
    if (fileName.empty()) return;
    const QString path = QString::fromUtf8(fileName.c_str());
    for (auto entry = m_modelTex.begin(); entry != m_modelTex.end();) {
        if (entry.value() == path) entry = m_modelTex.erase(entry);
        else ++entry;
    }
}

void LAppTextureManager::ReleaseTextures()
{
    if (m_context && !m_textures.empty() && !CanUseContext()) return;
    for (const auto& texture : m_textures)
        if (m_context && texture->id) glDeleteTextures(1, &texture->id);
    m_textures.clear();
    m_modelTex.clear();
}

void LAppTextureManager::ReleaseTexture(GLuint textureId)
{
    if (!CanUseContext()) return;
    const auto found = std::find_if(m_textures.begin(), m_textures.end(),
        [textureId](const auto& texture) { return texture->id == textureId; });
    if (found == m_textures.end()) return;
    glDeleteTextures(1, &(*found)->id);
    RemoveModelTexture((*found)->fileName);
    m_textures.erase(found);
}

void LAppTextureManager::ReleaseTexture(std::string fileName)
{
    const auto* texture = GetTextureInfoByName(fileName);
    if (texture) ReleaseTexture(texture->id);
}

void LAppTextureManager::ReleaseUnusedTextures(const QSet<QString>& retainedPaths, const QSet<GLuint>& retainedIds)
{
    if (!CanUseContext()) return;
    for (auto texture = m_textures.begin(); texture != m_textures.end();) {
        if (retainedIds.contains((*texture)->id) || (!(*texture)->fileName.empty() && retainedPaths.contains(QString::fromUtf8((*texture)->fileName.c_str())))) {
            ++texture;
            continue;
        }
        glDeleteTextures(1, &(*texture)->id);
        RemoveModelTexture((*texture)->fileName);
        texture = m_textures.erase(texture);
    }
}

LAppTextureManager::TextureInfo* LAppTextureManager::GetTextureInfoById(GLuint textureId) const
{
    for (const auto& texture : m_textures) if (texture->id == textureId) return texture.get();
    return nullptr;
}

LAppTextureManager::TextureInfo* LAppTextureManager::GetTextureInfoByName(const std::string& fileName) const
{
    if (fileName.empty()) return nullptr;
    for (const auto& texture : m_textures) if (texture->fileName == fileName) return texture.get();
    return nullptr;
}
