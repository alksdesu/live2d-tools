/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppTextureManager.hpp"
#include "LAppPal.hpp"
#include <QScopeGuard>
#include <algorithm>
#include <limits>

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "std/stb_image.h"

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
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(address.get(), static_cast<int>(size), &width, &height, &channels)) return nullptr;
    GLint maximumSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumSize);
    if (width <= 0 || height <= 0 || width > maximumSize || height > maximumSize ||
        qint64(width) * height > std::numeric_limits<int>::max() / 4) return nullptr;

    int decodedWidth = 0, decodedHeight = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(address.get(), static_cast<int>(size), &decodedWidth, &decodedHeight,
                              &channels, STBI_rgb_alpha), &stbi_image_free);
    if (!pixels || decodedWidth != width || decodedHeight != height) return nullptr;
#ifdef PREMULTIPLIED_ALPHA_ENABLE
    for (qint64 pixel = 0; pixel < qint64(width) * height; ++pixel) {
        stbi_uc* color = pixels.get() + pixel * 4;
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
    if (!textureId) return nullptr;
    const auto release = qScopeGuard([&] { if (textureId) glDeleteTextures(1, &textureId); });
    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
    GLint uploadedWidth = 0, uploadedHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &uploadedWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &uploadedHeight);
    if (uploadedWidth != width || uploadedHeight != height) return nullptr;
    glGenerateMipmap(GL_TEXTURE_2D);
    for (int level = 1, mipWidth = width, mipHeight = height; mipWidth > 1 || mipHeight > 1; ++level) {
        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &uploadedWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &uploadedHeight);
        if (uploadedWidth != mipWidth || uploadedHeight != mipHeight) return nullptr;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    auto info = std::make_unique<TextureInfo>();
    info->fileName = std::move(fileName);
    info->width = width;
    info->height = height;
    info->id = textureId;
    TextureInfo* result = info.get();
    m_textures.push_back(std::move(info));
    textureId = 0;
    return result;
}

void LAppTextureManager::RemoveModelTexture(const std::string& fileName)
{
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

void LAppTextureManager::ReleaseUnusedTextures(const QSet<QString>& retainedPaths)
{
    if (!CanUseContext()) return;
    for (auto texture = m_textures.begin(); texture != m_textures.end();) {
        if (retainedPaths.contains(QString::fromUtf8((*texture)->fileName.c_str()))) {
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
    for (const auto& texture : m_textures) if (texture->fileName == fileName) return texture.get();
    return nullptr;
}
