#include "cmodeldocument.h"
#include "cmocmaterialbuilder.h"
#include <QCryptographicHash>
#include <algorithm>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool references(const CModelDocument& document, const CTextureWorkspace::State& state, QString* error)
{
    CModelMaterialState materials;
    if (!document.CaptureDrawableMaterials(materials, error)) return false;
    for (const auto& material : materials)
        if (!material.uvs.isEmpty() && !state.atlases.contains(material.textureIndex))
            return fail(error, QStringLiteral("网格 %1 引用了不存在的图集槽 %2").arg(material.id).arg(material.textureIndex));
    return true;
}
bool states(const CModelEdit& edit, CModelAtlasState& before, CModelAtlasState& after, QString* error)
{
    if (edit.field != CModelEdit::AtlasLayout || edit.type != QLatin1String("textures") || edit.name != QLatin1String("atlases") ||
        edit.index != -1 || edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None || edit.freezeValue ||
        edit.meshRequest || !edit.meshRemaps.isEmpty() || edit.bindingKeysRequest || edit.deformerRequest ||
        edit.before.metaType() != QMetaType::fromType<CModelAtlasState>() || edit.after.metaType() != QMetaType::fromType<CModelAtlasState>())
        return fail(error, QStringLiteral("图集编辑命令格式无效"));
    before = edit.before.value<CModelAtlasState>(); after = edit.after.value<CModelAtlasState>();
    if (before.materials.size() != after.materials.size() || !CModelMaterialEdit::Validate(before.materials, error) ||
        !CModelMaterialEdit::Validate(after.materials, error)) return false;
    for (qsizetype index = 0; index < before.materials.size(); ++index)
        if (before.materials[index].id != after.materials[index].id ||
            (index && before.materials[index - 1].id >= before.materials[index].id))
            return fail(error, QStringLiteral("图集编辑的材质对象顺序不一致"));
    return true;
}
bool freeze(const CTextureAssets::Catalog& sources, const QMap<QByteArray, QByteArray>& trusted,
            const CModelAtlasState& state, CModelAtlasState& result, QString* error)
{
    CModelAtlasState candidate;
    candidate.workspace.sourceFingerprint = QByteArray(state.workspace.sourceFingerprint.constData(), state.workspace.sourceFingerprint.size());
    auto blobs = trusted;
    for (auto it = state.workspace.atlases.cbegin(); it != state.workspace.atlases.cend(); ++it) {
        QByteArray png;
        if (!it->png.isEmpty()) {
            const auto digest = QCryptographicHash::hash(it->png, QCryptographicHash::Sha256);
            const auto existing = blobs.constFind(digest);
            if (existing != blobs.cend() && *existing == it->png) png = *existing;
            else { png = QByteArray(it->png.constData(), it->png.size()); blobs.insert(digest, png); }
        }
        candidate.workspace.atlases.insert(it.key(), {QString(it->id.constData(), it->id.size()),
            QString(it->name.constData(), it->name.size()), it->sourceSlot, it->size, std::move(png)});
    }
    if (!CTextureWorkspace::Validate(sources, candidate.workspace, error) || !CModelMaterialEdit::Validate(state.materials, error)) return false;
    for (const auto& material : state.materials) candidate.materials.append({QString(material.id.constData(), material.id.size()),
        material.textureIndex, QVector<float>(material.uvs.cbegin(), material.uvs.cend())});
    std::sort(candidate.materials.begin(), candidate.materials.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    result = std::move(candidate); return true;
}
}
bool CModelDocument::InitializeTextureWorkspace(const CTextureAssets::Catalog& sources, const CTextureAssets::State& legacy,
                                               QString* error) try
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    CTextureWorkspace::State state;
    if (!CTextureWorkspace::Initialize(sources, legacy, state, error)) return false;
    CTextureAssets::Catalog frozen;
    QMap<QByteArray, QByteArray> blobs;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        QByteArray png; const auto existing = blobs.constFind(it->digest);
        if (existing != blobs.cend() && *existing == it->png) png = *existing;
        else { png = QByteArray(it->png.constData(), it->png.size()); blobs.insert(it->digest, png); }
        frozen.insert(it.key(), {QString(it->path.constData(), it->path.size()), png,
            QByteArray(it->digest.constData(), it->digest.size()), it->size});
    }
    if (!CTextureWorkspace::Validate(frozen, state, error) || !references(*this, state, error)) return false;
    if (HasTextureWorkspace())
        return (frozen == m_textureSources && state == m_textureWorkspace) || fail(error, QStringLiteral("已加载的图集来源不能重新定义"));
    m_textureSources.swap(frozen); m_textureWorkspace = std::move(state); ++m_revision; return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法初始化模型图集")); }

bool CModelDocument::SetTextureWorkspace(const CTextureWorkspace::State& state, QString* error) try
{
    if (!m_loaded || !HasTextureWorkspace()) return fail(error, QStringLiteral("尚未初始化模型图集来源"));
    CTextureWorkspace::State frozen;
    if (!CTextureWorkspace::Freeze(m_textureSources, state, frozen, error) || !references(*this, frozen, error)) return false;
    if (frozen != m_textureWorkspace) { m_textureWorkspace = std::move(frozen); ++m_revision; }
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法恢复模型图集")); }

bool CModelDocument::CaptureAtlasState(CModelAtlasState& result, QString* error) const
{
    if (!m_loaded || !HasTextureWorkspace()) return fail(error, QStringLiteral("尚未初始化模型图集来源"));
    CModelAtlasState candidate; candidate.workspace = m_textureWorkspace;
    if (!CaptureDrawableMaterials(candidate.materials, error)) return false;
    result = std::move(candidate); return true;
}

bool CModelDocument::MakeAtlasEdit(const CModelAtlasState& before, const CModelAtlasState& after,
                                  CModelEdit& edit, QString* error) const try
{
    if (!HasTextureWorkspace()) return fail(error, QStringLiteral("尚未初始化模型图集来源"));
    if (before.workspace != m_textureWorkspace) return fail(error, QStringLiteral("图集在操作期间已改变"));
    const CModelEdit request{QStringLiteral("atlases"), QStringLiteral("textures"), CModelEdit::AtlasLayout,
                            QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!ValidateAtlasHistory(request, error)) return false;
    QMap<QByteArray, QByteArray> blobs;
    const auto remember = [&](const CTextureWorkspace::State& workspace) {
        for (const auto& atlas : workspace.atlases) if (!atlas.png.isEmpty())
            blobs.insert(QCryptographicHash::hash(atlas.png, QCryptographicHash::Sha256), atlas.png);
    };
    for (const auto& source : m_textureSources) blobs.insert(source.digest, source.png);
    remember(m_textureWorkspace);
    for (const auto& entry : m_editHistory) for (const auto& value : {entry.before, entry.after}) {
        if (entry.field == CModelEdit::AtlasLayout) remember(value.value<CModelAtlasState>().workspace);
        else if (entry.field == CModelEdit::ParameterStructure || entry.field == CModelEdit::MeshStructure || entry.field == CModelEdit::DeformerStructure)
            remember(value.toMap().value("textureWorkspace").value<CTextureWorkspace::State>());
    }
    CModelAtlasState previous, next;
    if (!freeze(m_textureSources, blobs, before, previous, error) || !freeze(m_textureSources, blobs, after, next, error)) return false;
    CModelEdit command{QStringLiteral("atlases"), QStringLiteral("textures"), CModelEdit::AtlasLayout,
                      QVariant::fromValue(previous), QVariant::fromValue(next), -1, false};
    if (!ValidateAtlasHistory(command, error)) return false;
    auto candidate = *this;
    if (!candidate.ApplyAtlasEdit(command, true, error)) return false;
    edit = std::move(command); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备图集编辑")); }

bool CModelDocument::ApplyAtlasEdit(const CModelEdit& edit, bool forward, QString* error)
{
    CModelAtlasState before, after;
    if (!states(edit, before, after, error) || !HasTextureWorkspace()) return false;
    const auto& expected = forward ? before : after; const auto& next = forward ? after : before;
    if (m_textureWorkspace != expected.workspace) return fail(error, QStringLiteral("图集在操作期间已改变"));
    if (!CTextureWorkspace::ValidateTransition(m_textureSources, expected.workspace, next.workspace,
                                              CTextureWorkspace::Transition::ReplaceGenerated, error)) return false;
    auto candidate = *this;
    candidate.m_textureWorkspace = next.workspace;
    if (!expected.materials.isEmpty()) {
        const CModelEdit material{QStringLiteral("drawables"), QStringLiteral("materials"), CModelEdit::DrawableMaterials,
            QVariant::fromValue(before.materials), QVariant::fromValue(after.materials), -1, false};
        if (!candidate.ApplyMaterialEdit(material, forward, error)) return false;
    }
    if (!references(candidate, candidate.m_textureWorkspace, error)) return false;
    CommitCandidate(std::move(candidate)); return true;
}

bool CModelDocument::ValidateAtlasEditStorage(const CModelEdit& edit) const
{
    CModelAtlasState before, after;
    if (!states(edit, before, after, nullptr) || !HasTextureWorkspace() ||
        !CTextureWorkspace::Validate(m_textureSources, m_textureWorkspace) || !references(*this, m_textureWorkspace, nullptr)) return false;
    CModelMaterialState current;
    for (const auto& material : before.materials) {
        CModelDrawableMaterial value; if (!CaptureDrawableMaterial(material.id, value)) return false;
        current.append(std::move(value));
    }
    if (!((m_textureWorkspace == before.workspace && current == before.materials) ||
          (m_textureWorkspace == after.workspace && current == after.materials))) return false;
    QByteArray bytes; return CMocMaterialBuilder::Build(m_workingBaseBytes, m_drawableMaterialOverrides, bytes);
}

bool CModelDocument::ValidateAtlasHistory(const CModelEdit& edit, QString* error) const
{
    QSet<QByteArray> digests; qint64 encoded = 0, decoded = 0;
    const auto count = [&](const QByteArray& png, const QSize& size) {
        if (png.isEmpty()) return true;
        const auto digest = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
        if (digests.contains(digest)) return true;
        const qint64 pixels = qint64(size.width()) * size.height();
        if (size.width() <= 0 || size.height() <= 0 || pixels > CTextureWorkspace::MaximumDecodedBytes / 4 ||
            png.size() > CTextureAssets::MaximumBytes - encoded || pixels * 4 > CTextureWorkspace::MaximumDecodedBytes - decoded)
            return fail(error, QStringLiteral("图集和撤销历史的素材总量超出支持范围，原历史保持不变"));
        digests.insert(digest); encoded += png.size(); decoded += pixels * 4; return true;
    };
    const auto workspace = [&](const CTextureWorkspace::State& state) {
        for (const auto& atlas : state.atlases) if (!count(atlas.png, atlas.size)) return false;
        return true;
    };
    const auto command = [&](const CModelEdit& entry) {
        for (const auto& value : {entry.before, entry.after}) {
            if (entry.field == CModelEdit::AtlasLayout) {
                if (!workspace(value.value<CModelAtlasState>().workspace)) return false;
            } else if (entry.field == CModelEdit::ParameterStructure || entry.field == CModelEdit::MeshStructure || entry.field == CModelEdit::DeformerStructure) {
                if (!workspace(value.toMap().value("textureWorkspace").value<CTextureWorkspace::State>())) return false;
            }
        }
        return true;
    };
    for (const auto& source : m_textureSources) if (!count(source.png, source.size)) return false;
    if (!workspace(m_textureWorkspace)) return false;
    for (const auto& entry : m_editHistory) if (!command(entry)) return false;
    return command(edit);
}
}
