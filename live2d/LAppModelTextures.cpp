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

using namespace Live2D::Cubism::Framework;

LAppModel::TextureState LAppModel::GetEffectiveTexturePngs() const
{
    return live2dstruct::CTextureAssets::Effective(m_textureSources, m_textureState);
}

LAppModel::PreparedTextures::~PreparedTextures()
{
    if (manager) for (csmUint32 id : release) manager->ReleaseTexture(id);
}

bool LAppModel::PrepareTextures(const TextureCatalog& sources, const TextureState& state,
                                PreparedTextures& result, QString* error)
{
    if (!live2dstruct::CTextureAssets::ValidateState(sources, state, error)) return false;
    auto* delegate = LAppDelegate::GetInstance();
    auto* manager = delegate->GetTextureManager();
    if (!manager || !delegate->getGlOwningContext() || QOpenGLContext::currentContext() != delegate->getGlOwningContext()) {
        if (error) *error = QStringLiteral("纹理准备需要当前模型所属 OpenGL 上下文");
        return false;
    }
    result.manager = manager;
    for (auto it = state.cbegin(); it != state.cend(); ++it)
        result.state.insert(it.key(), QByteArray(it->constData(), it->size()));
    const auto effective = live2dstruct::CTextureAssets::Effective(sources, result.state);
    QMap<QByteArray, csmUint32> available;
    for (csmUint32 id : m_textureBindings) {
        const auto* info = manager->GetTextureInfoById(id);
        if (info) available.insert(info->sourceDigest, id);
    }
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        const auto& png = effective[it.key()];
        const auto digest = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
        const LAppTextureManager::TextureInfo* info = nullptr;
        const auto found = available.constFind(digest);
        if (found != available.cend()) info = manager->GetTextureInfoById(*found);
        if (!info && png == it->png) info = manager->GetTextureInfoByName(it->path.toUtf8().toStdString());
        if (info && (info->pngBytes != png || QSize(info->width, info->height) != it->size)) info = nullptr;
        if (!info) {
            info = manager->CreateTextureFromPngBytes(png, it->size, error);
            if (!info) return false;
            auto cleanup = qScopeGuard([&] { manager->ReleaseTexture(info->id); });
            result.release.insert(info->id);
            result.owned.insert(info->id);
            available.insert(digest, info->id);
            cleanup.dismiss();
        } else if (m_ownedTextureIds.contains(info->id) || result.release.contains(info->id)) {
            result.owned.insert(info->id);
        }
        result.bindings.insert(it.key(), info->id);
    }
    result.retired = m_ownedTextureIds - result.owned;
    return true;
}

void LAppModel::CommitTextures(PreparedTextures& prepared) noexcept
{
    m_textureState.swap(prepared.state);
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
    if (!live2dstruct::CTextureAssets::ValidateState(m_textureSources, state, error)) return false;
    if (state == m_textureState) return true;
    return ApplyParameterStructurePreview(m_previewMocBytes, m_sourceMocDigest, {}, {}, HasEditorPreview(),
                                         error, {}, nullptr, nullptr, {}, nullptr, nullptr, nullptr, &state);
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
