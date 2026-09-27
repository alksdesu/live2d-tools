#include "cmodeldocument.h"
#include "cmocmaterialbuilder.h"
#include <QSet>
#include <algorithm>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool editable(const CModelDocument& document, const QString& id, QString* error)
{
    if (document.IsLocked(QStringLiteral("artmesh"), id)) return fail(error, QStringLiteral("网格已锁定：%1").arg(id));
    for (const auto& mesh : document.Drawables()) if (mesh.id == id) {
        const auto checkParts = [&](int part) {
            QSet<int> seen;
            while (part != -1) {
                if (part < 0 || part >= document.Parts().size() || seen.contains(part)) return fail(error, QStringLiteral("网格部件父级无效"));
                seen.insert(part); const auto& node = document.Parts()[part];
                if (document.IsLocked(QStringLiteral("part"), node.id)) return fail(error, QStringLiteral("网格所属部件已锁定：%1").arg(node.id));
                part = node.parentIndex;
            }
            return true;
        };
        if (!checkParts(mesh.parentPartIndex)) return false;
        QSet<int> seen;
        for (int parent = mesh.parentDeformerIndex; parent != -1;) {
            if (parent < 0 || parent >= document.Deformers().size() || seen.contains(parent)) return fail(error, QStringLiteral("网格变形器父级无效"));
            seen.insert(parent); const auto& node = document.Deformers()[parent];
            if (document.IsLocked(node.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation"), node.id))
                return fail(error, QStringLiteral("网格父变形器已锁定：%1").arg(node.id));
            if (!checkParts(node.parentPartIndex)) return false;
            parent = document.DeformerParentIndex(node.id);
        }
        return true;
    }
    return fail(error, QStringLiteral("材质网格不存在：%1").arg(id));
}
bool states(const CModelEdit& edit, CModelMaterialState& before, CModelMaterialState& after, QString* error)
{
    if (edit.field != CModelEdit::DrawableMaterials || edit.type != QLatin1String("materials") || edit.name != QLatin1String("drawables") ||
        edit.index != -1 || edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None || edit.freezeValue ||
        edit.meshRequest || !edit.meshRemaps.isEmpty() || edit.bindingKeysRequest || edit.deformerRequest ||
        edit.before.metaType() != QMetaType::fromType<CModelMaterialState>() || edit.after.metaType() != QMetaType::fromType<CModelMaterialState>())
        return fail(error, QStringLiteral("材质编辑命令格式无效"));
    before = edit.before.value<CModelMaterialState>(); after = edit.after.value<CModelMaterialState>();
    if (before.isEmpty() || before.size() != after.size() || !CModelMaterialEdit::Validate(before, error) || !CModelMaterialEdit::Validate(after, error)) return false;
    for (qsizetype index = 0; index < before.size(); ++index)
        if (before[index].id != after[index].id || (index && before[index - 1].id >= before[index].id))
            return fail(error, QStringLiteral("材质编辑对象顺序不一致"));
    return true;
}
}
bool CModelDocument::CaptureDrawableMaterials(CModelMaterialState& result, QString* error) const try
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    CModelMaterialOverrides materials;
    if (!CMocMaterialBuilder::Capture(m_workingBaseBytes, materials, error)) return false;
    for (auto it = m_drawableMaterialOverrides.cbegin(); it != m_drawableMaterialOverrides.cend(); ++it) materials.insert(it.key(), *it);
    CModelMaterialState candidate; candidate.reserve(materials.size());
    for (const auto& material : materials) candidate.append(material);
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取模型材质")); }
bool CModelDocument::CaptureDrawableMaterial(const QString& id, CModelDrawableMaterial& result, QString* error) const try
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    const auto found = m_drawableMaterialOverrides.constFind(id);
    if (found != m_drawableMaterialOverrides.cend()) { result = *found; if (error) error->clear(); return true; }
    return CMocMaterialBuilder::Capture(m_workingBaseBytes, id, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取网格材质")); }
bool CModelDocument::SetDrawableMaterials(const CModelMaterialState& state, QString* error)
{
    return ApplyDrawableMaterials(state, true, error);
}
bool CModelDocument::ApplyDrawableMaterials(const CModelMaterialState& state, bool updateArtwork, QString* error) try
{
    if (!m_loaded) return fail(error, QStringLiteral("尚未加载模型"));
    if (!CModelMaterialEdit::Validate(state, error)) return false;
    if (state.isEmpty()) { if (error) error->clear(); return true; }
    CModelMaterialOverrides baseline;
    if (!CMocMaterialBuilder::Capture(m_workingBaseBytes, baseline, error)) return false;
    auto overrides = m_drawableMaterialOverrides;
    for (const auto& material : state) {
        const auto found = baseline.constFind(material.id);
        if (found == baseline.cend() || found->uvs.size() != material.uvs.size()) return fail(error, QStringLiteral("材质对象或 UV 顶点数不匹配：%1").arg(material.id));
        if (material == *found) overrides.remove(material.id); else overrides.insert(material.id, material);
    }
    if (HasTextureWorkspace()) for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        const auto material = overrides.value(it.key(), *it);
        if (!material.uvs.isEmpty() && !m_textureWorkspace.atlases.contains(material.textureIndex))
            return fail(error, QStringLiteral("网格 %1 引用了不存在的图集槽 %2").arg(material.id).arg(material.textureIndex));
    }
    if (overrides == m_drawableMaterialOverrides) { if (error) error->clear(); return true; }
    QByteArray check;
    if (!CMocMaterialBuilder::Build(m_workingBaseBytes, overrides, check, error)) return false;
    CModelAtlasState before;
    if (updateArtwork && !m_artwork.bindings.isEmpty() && !CaptureAtlasState(before, error)) return false;
    auto candidate = *this;
    candidate.m_drawableMaterialOverrides.swap(overrides);
    if (updateArtwork && !candidate.ReconcileArtworkAppearance(before, error)) return false;
    CommitCandidate(std::move(candidate)); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法应用材质修改，原状态保持不变")); }
bool CModelDocument::MakeMaterialEdit(const CModelMaterialState& before, const CModelMaterialState& after,
                                     CModelEdit& edit, QString* error) const try
{
    auto previous = before, next = after;
    const auto order = [](const auto& first, const auto& second) { return first.id < second.id; };
    std::sort(previous.begin(), previous.end(), order); std::sort(next.begin(), next.end(), order);
    CModelEdit command{QStringLiteral("drawables"), QStringLiteral("materials"), CModelEdit::DrawableMaterials,
        QVariant::fromValue(previous), QVariant::fromValue(next), -1, false};
    auto candidate = *this;
    if (!candidate.ApplyMaterialEdit(command, true, error)) return false;
    edit = std::move(command); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备材质修改，原结果保持不变")); }
bool CModelDocument::ApplyMaterialEdit(const CModelEdit& edit, bool forward, QString* error, bool updateArtwork)
{
    CModelMaterialState before, after;
    if (!states(edit, before, after, error)) return false;
    const auto& expected = forward ? before : after; const auto& next = forward ? after : before;
    CModelMaterialOverrides current;
    if (!CMocMaterialBuilder::Capture(m_workingBaseBytes, current, error)) return false;
    for (auto it = m_drawableMaterialOverrides.cbegin(); it != m_drawableMaterialOverrides.cend(); ++it) current.insert(it.key(), it.value());
    for (qsizetype index = 0; index < expected.size(); ++index) {
        if (!current.contains(expected[index].id) || current.value(expected[index].id) != expected[index])
            return fail(error, QStringLiteral("材质对象在操作期间已改变：%1").arg(expected[index].id));
        if (forward && expected[index] != next[index] && !editable(*this, expected[index].id, error)) return false;
    }
    return ApplyDrawableMaterials(next, updateArtwork, error);
}
bool CModelDocument::ValidateMaterialEditStorage(const CModelEdit& edit) const
{
    CModelMaterialState before, after, current;
    if (!states(edit, before, after, nullptr)) return false;
    for (const auto& material : before) {
        CModelDrawableMaterial value;
        if (!CaptureDrawableMaterial(material.id, value)) return false;
        current.append(std::move(value));
    }
    if (current != before && current != after) return false;
    if (HasTextureWorkspace()) {
        CModelMaterialState materials;
        if (!CaptureDrawableMaterials(materials)) return false;
        for (const auto& material : materials)
            if (!material.uvs.isEmpty() && !m_textureWorkspace.atlases.contains(material.textureIndex)) return false;
    }
    QByteArray check;
    return CMocMaterialBuilder::Build(m_workingBaseBytes, m_drawableMaterialOverrides, check);
}
}
