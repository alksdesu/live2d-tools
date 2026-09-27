#include "cmodeldocument.h"
#include "cartworkreconciler.h"
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

bool CModelDocument::ValidateArtworkReferences(const CModelArtwork::State& state, QString* error) const try
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    if (state.bindings.isEmpty()) return true;
    CModelMaterialState materials;
    if (!CaptureDrawableMaterials(materials, error)) return false;
    CArtworkReconciler::Triangles triangles;
    for (const auto& mesh : m_drawables) triangles.insert(mesh.id, m_drawableIndexOverrides.value(mesh.id, mesh.indices));
    return CArtworkReconciler::ValidateLinks(state, m_textureWorkspace, materials, triangles, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法校验模型素材关联")); }

bool CModelDocument::ReconcileArtworkAppearance(const CModelAtlasState& before, QString* error)
{
    if (before.artwork.bindings.isEmpty()) return true;
    CModelMaterialState materials;
    if (!CaptureDrawableMaterials(materials, error)) return false;
    CArtworkReconciler::Triangles triangles;
    for (const auto& mesh : m_drawables) triangles.insert(mesh.id, m_drawableIndexOverrides.value(mesh.id, mesh.indices));
    CModelArtwork::State reconciled;
    return CArtworkReconciler::ManualAppearance(m_textureSources, before, m_textureWorkspace, materials,
                                               triangles, reconciled, error) && SetArtwork(reconciled, error);
}

bool CModelDocument::SetArtwork(const CModelArtwork::State& state, QString* error) try
{
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    if (state == m_artwork) return ValidateArtworkReferences(state, error);
    CModelAtlasState appearance;
    appearance.workspace = m_textureWorkspace; appearance.artwork = state;
    CModelEdit budget;
    budget.field = CModelEdit::AtlasLayout; budget.after = QVariant::fromValue(appearance);
    if (!ValidateAtlasHistory(budget, error)) return false;
    CModelArtwork::State frozen;
    QByteArray fingerprint;
    if (!CModelArtwork::Freeze(state, frozen, error) || !ValidateArtworkReferences(frozen, error) ||
        (!frozen.IsEmpty() && !CModelArtwork::Fingerprint(frozen, fingerprint, error))) return false;
    m_artwork = std::move(frozen); m_artworkFingerprint = std::move(fingerprint); ++m_revision;
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法应用素材关联，原状态保持不变")); }
}
