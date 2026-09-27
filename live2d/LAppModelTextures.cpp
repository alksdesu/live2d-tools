/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "LAppModel.hpp"
#include "LAppDelegate.hpp"
#include "LAppTextureManager.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "std/cmodelresourcepath.h"
#include <QScopeGuard>
#include <QCryptographicHash>
#include <exception>
#include <algorithm>

using namespace Live2D::Cubism::Framework;

LAppModel::TextureState LAppModel::GetEffectiveTexturePngs() const
{
    TextureState result;
    live2dstruct::CTextureWorkspace::Effective(m_textureSources, m_textureWorkspace, result);
    return result;
}

LAppModel::PreparedTextures::~PreparedTextures()
{
    if (manager) for (csmUint32 id : release) manager->ReleaseTexture(id);
}

bool LAppModel::PrepareTextures(const TextureCatalog& sources, const TextureState& state,
                                PreparedTextures& result, QString* error)
{
    AtlasState workspace;
    return live2dstruct::CTextureWorkspace::Initialize(sources, state, workspace, error) &&
        PrepareTextures(sources, workspace, result, error);
}

bool LAppModel::PrepareTextures(const TextureCatalog& sources, const AtlasState& state,
                                PreparedTextures& result, QString* error)
{
    if (!live2dstruct::CTextureWorkspace::Freeze(sources, state, result.workspace, error)) return false;
    auto* delegate = LAppDelegate::GetInstance();
    auto* manager = delegate->GetTextureManager();
    if (!manager || !delegate->getGlOwningContext() || QOpenGLContext::currentContext() != delegate->getGlOwningContext()) {
        if (error) *error = QStringLiteral("纹理准备需要当前模型所属 OpenGL 上下文");
        return false;
    }
    result.manager = manager;
    TextureState effective;
    if (!live2dstruct::CTextureWorkspace::Effective(sources, result.workspace, effective, error)) return false;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        const auto atlas = result.workspace.atlases.constFind(it.key());
        if (atlas != result.workspace.atlases.cend() && atlas->size == it->size && effective.value(it.key()) != it->png)
            result.state.insert(it.key(), effective.value(it.key()));
    }
    QMap<QByteArray, csmUint32> available;
    for (csmUint32 id : m_textureBindings) {
        const auto* info = manager->GetTextureInfoById(id);
        if (info) available.insert(info->sourceDigest, id);
    }
    qint64 gpuBytes = 0;
    const auto reserveGpu = [&](const QSize& size) {
        if (!size.isValid() || size.isEmpty() ||
            size.width() > live2dstruct::CTextureWorkspace::MaximumDecodedBytes / 4 / size.height()) return false;
        for (int width = size.width(), height = size.height();;) {
            const qint64 bytes = qint64(width) * height * 4;
            if (bytes > live2dstruct::CTextureWorkspace::MaximumDecodedBytes - gpuBytes) return false;
            gpuBytes += bytes;
            if (width == 1 && height == 1) return true;
            width = std::max(1, width / 2);
            height = std::max(1, height / 2);
        }
    };
    QSet<csmUint32> retained;
    for (csmUint32 id : m_textureBindings) retained.insert(id);
    for (const auto& source : sources) {
        const auto* info = manager->GetTextureInfoByName(source.path.toUtf8().toStdString());
        if (info) retained.insert(info->id);
    }
    QSet<csmUint32> counted;
    for (csmUint32 id : retained) {
        const auto* info = manager->GetTextureInfoById(id);
        if (info && !counted.contains(id)) {
            if (!reserveGpu(QSize(info->width, info->height))) {
                if (error) *error = QStringLiteral("当前图集 GPU 资源超过预算");
                return false;
            }
            counted.insert(id);
        }
    }
    for (auto it = result.workspace.atlases.cbegin(); it != result.workspace.atlases.cend(); ++it) {
        const auto& png = effective[it.key()];
        const auto digest = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
        const LAppTextureManager::TextureInfo* info = nullptr;
        const auto found = available.constFind(digest);
        if (found != available.cend()) info = manager->GetTextureInfoById(*found);
        const auto source = sources.constFind(it->sourceSlot);
        if (!info && source != sources.cend() && png == source->png)
            info = manager->GetTextureInfoByName(source->path.toUtf8().toStdString());
        if (info && (info->pngBytes != png || QSize(info->width, info->height) != it->size)) info = nullptr;
        if (!info) {
            if (!reserveGpu(it->size)) {
                if (error) *error = QStringLiteral("图集候选与当前 GPU 资源合计超过预算");
                return false;
            }
            info = manager->CreateTextureFromPngBytes(png, it->size, error);
            if (!info) return false;
            auto cleanup = qScopeGuard([&] { manager->ReleaseTexture(info->id); });
            result.release.insert(info->id);
            result.owned.insert(info->id);
            counted.insert(info->id);
            available.insert(digest, info->id);
            cleanup.dismiss();
        } else if (m_ownedTextureIds.contains(info->id) || result.release.contains(info->id)) {
            result.owned.insert(info->id);
        }
        if (!counted.contains(info->id)) {
            if (!reserveGpu(it->size)) {
                if (error) *error = QStringLiteral("图集 GPU 资源合计超过预算");
                return false;
            }
            counted.insert(info->id);
        }
        result.bindings.insert(it.key(), info->id);
    }
    result.retired = m_ownedTextureIds - result.owned;
    return true;
}

void LAppModel::CommitTextures(PreparedTextures& prepared) noexcept
{
    m_textureState.swap(prepared.state);
    std::swap(m_textureWorkspace, prepared.workspace);
    m_textureBindings.swap(prepared.bindings);
    m_ownedTextureIds.swap(prepared.owned);
    prepared.release.swap(prepared.retired);
}

bool LAppModel::ApplyTextureState(const TextureState& state, QString* error)
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || HasExportPreview() || m_physicsDragActive) {
        if (error) *error = QStringLiteral("当前模型状态不允许替换纹理");
        return false;
    }
    if (m_textureWorkspace.atlases.size() != m_textureSources.size()) {
        if (error) *error = QStringLiteral("图集目录已改变，请通过图集工作区替换纹理");
        return false;
    }
    for (auto it = m_textureSources.cbegin(); it != m_textureSources.cend(); ++it) {
        const auto atlas = m_textureWorkspace.atlases.constFind(it.key());
        if (atlas == m_textureWorkspace.atlases.cend() || atlas->sourceSlot != it.key() || atlas->size != it->size) {
            if (error) *error = QStringLiteral("图集尺寸已改变，请通过图集工作区替换纹理");
            return false;
        }
    }
    AtlasState candidate;
    QByteArray fingerprint;
    if (!live2dstruct::CTextureWorkspace::Initialize(m_textureSources, state, candidate, error) ||
        !live2dstruct::CTextureWorkspace::Fingerprint(m_textureSources, m_textureWorkspace, fingerprint, error)) return false;
    for (auto it = candidate.atlases.begin(); it != candidate.atlases.end(); ++it) {
        it->id = m_textureWorkspace.atlases[it.key()].id;
        it->name = m_textureWorkspace.atlases[it.key()].name;
    }
    return ApplyAtlasPreview(m_previewMocBytes, m_sourceMocDigest, m_previewMocDigest, fingerprint, candidate, {}, error);
}

bool LAppModel::SetupTextures()
{
    if (!_model || !_modelSetting || _modelSetting->GetTextureCount() < 0 ||
        _modelSetting->GetTextureCount() > live2dstruct::CTextureAssets::MaximumSlots) return false;
    auto* renderer = GetRenderer<Rendering::CubismRenderer_OpenGLES2>();
    auto* manager = LAppDelegate::GetInstance()->GetTextureManager();
    if (!renderer || !manager) return false;
    QMap<int, QString> paths;
    TextureCatalog sources;
    QSet<csmUint32> created;
    qint64 totalBytes = 0;
    auto rollback = qScopeGuard([&] { for (csmUint32 id : created) manager->ReleaseTexture(id); });
    for (int slot = 0; slot < _modelSetting->GetTextureCount(); ++slot) {
        const char* name = _modelSetting->GetTextureFileName(slot);
        if (!name) return false;
        if (!name[0]) continue;
        const QString path = CModelResourcePath::Resolve(QString::fromUtf8(_modelHomeDir.GetRawString()), QString::fromUtf8(name));
        if (path.isEmpty()) return false;
        paths.insert(slot, path);
        const auto original = m_textureSources.constFind(slot);
        if (original != m_textureSources.cend()) {
            if (original->path != path || original->png.size() > live2dstruct::CTextureAssets::MaximumBytes - totalBytes) return false;
            totalBytes += original->png.size();
            sources.insert(slot, *original);
            continue;
        }
        const auto fileName = path.toUtf8().toStdString();
        auto* info = manager->GetTextureInfoByName(fileName);
        if (!info) {
            info = manager->CreateTextureFromPngFile(fileName);
            if (info) {
                auto cleanup = qScopeGuard([&] { manager->ReleaseTexture(info->id); });
                created.insert(info->id);
                cleanup.dismiss();
            }
        }
        if (!info || !info->id || info->width <= 0 || info->height <= 0 || info->pngBytes.isEmpty()) return false;
        if (info->pngBytes.size() > live2dstruct::CTextureAssets::MaximumBytes - totalBytes) return false;
        totalBytes += info->pngBytes.size();
        sources.insert(slot, {path, info->pngBytes, info->sourceDigest, QSize(info->width, info->height)});
    }
    PreparedTextures prepared;
    if (!PrepareTextures(sources, m_textureState, prepared, nullptr)) return false;
    for (auto it = prepared.bindings.cbegin(); it != prepared.bindings.cend(); ++it) renderer->BindTexture(it.key(), it.value());
#ifdef PREMULTIPLIED_ALPHA_ENABLE
    renderer->IsPremultipliedAlpha(true);
#else
    renderer->IsPremultipliedAlpha(false);
#endif
    m_pendingTextureIds.unite(created);
    m_textureSources.swap(sources);
    m_texturePaths.swap(paths);
    CommitTextures(prepared);
    rollback.dismiss();
    return true;
}

void LAppModel::ReleaseOwnedTextures()
{
    auto* manager = LAppDelegate::GetInstance()->GetTextureManager();
    if (manager) for (csmUint32 id : m_ownedTextureIds) manager->ReleaseTexture(id);
    m_ownedTextureIds.clear();
    m_textureBindings.clear();
}
