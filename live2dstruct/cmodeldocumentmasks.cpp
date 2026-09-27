#include "cmodeldocument.h"
#include "csectiontable.h"
#include "cmaskeditpolicy.h"
#include <algorithm>

namespace live2dstruct {
namespace {
constexpr int MaximumReferences = CMaskEditPolicy::MaximumReferences;

struct MaskDirectory
{
    QVector<QString> ids;
    QHash<QString, int> indices;

    void append(const QString& id)
    {
        indices.insert(id, id.isEmpty() || indices.contains(id) ? -1 : ids.size());
        ids.append(id);
    }

    explicit MaskDirectory(const QVector<CModelDrawable>& drawables)
    {
        ids.reserve(drawables.size());
        for (const auto& drawable : drawables)
            append(drawable.id);
    }
    MaskDirectory() = default;
};
}

struct CModelDocument::MaskSourceSnapshot
{
    struct Slice
    {
        int begin = -1, count = 0;
        bool valid = false;
    };
    MaskDirectory directory;
    QVector<int> references;
    QVector<Slice> slices;
    QHash<int, QVector<int>> occurrences;

    int count(const QString& owner, const QString& target) const
    {
        const int index = directory.indices.value(owner, -1);
        const int mask = directory.indices.value(target, -1);
        if (index < 0 || mask < 0 || !slices[index].valid)
            return 0;
        const auto& slice = slices[index];
        const auto found = occurrences.constFind(mask);
        if (found == occurrences.cend())
            return 0;
        return std::lower_bound(found->cbegin(), found->cend(), slice.begin + slice.count)
            - std::lower_bound(found->cbegin(), found->cend(), slice.begin);
    }

    bool same(const QString& owner, const QVector<int>& values, const MaskDirectory& current) const
    {
        const int index = directory.indices.value(owner, -1);
        if (index < 0 || current.indices.value(owner, -1) < 0 || !slices[index].valid)
            return false;
        const auto& slice = slices[index];
        if (slice.count != values.size())
            return false;
        for (int at = 0; at < values.size(); ++at) {
            const int original = references[slice.begin + at], requested = values[at];
            if (original < 0 || original >= directory.ids.size()) {
                if (original != requested || (requested >= 0 && requested < current.ids.size())) return false;
            } else if (requested < 0 || requested >= current.ids.size()) {
                return false;
            } else if (directory.ids[original] != current.ids[requested]
                || directory.indices.value(directory.ids[original], -1) != original
                || current.indices.value(current.ids[requested], -1) != requested)
                return false;
        }
        return true;
    }
    bool allowed(
        const QString& id, const QVector<int>& values, const MaskDirectory& current, const QVector<int>* baseline) const
    {
        const int owner = current.indices.value(id, -1);
        if (owner < 0 || values.size() > MaximumReferences)
            return false;
        if ((!baseline || values == *baseline) && same(id, values, current))
            return true;
        if (baseline && !CMaskEditPolicy::Validate(*baseline, values, owner, current.ids.size())) return false;
        QHash<int, int> requested;
        for (int index : values) {
            if (index < 0 || index >= current.ids.size() || index == owner
                || current.indices.value(current.ids[index], -1) != index)
                return false;
            const int amount = ++requested[index];
            if (amount > 1 && amount > count(id, current.ids[index]))
                return false;
        }
        return true;
    }
};

void CModelDocument::CaptureMaskSource(const CSectionTable& table)
{
    auto source = std::make_shared<MaskSourceSnapshot>();
    m_invalidMaskSlices.clear();
    source->references.reserve(table.drawable_masks.size());
    for (const auto* mask : table.drawable_masks) {
        source->occurrences[mask->art_mesh_sources_indices].append(source->references.size());
        source->references.append(mask->art_mesh_sources_indices);
    }
    for (const auto* mesh : table.art_meshes) {
        source->directory.append(QString(mesh->ids.constData(), mesh->ids.size()));
        const int begin = mesh->drawable_mask_sources_begin_indices, count = mesh->drawable_mask_sources_counts;
        const bool valid = count >= 0 && (count == 0 ||
            (begin >= 0 && begin <= source->references.size() && count <= source->references.size() - begin));
        if (!valid) m_invalidMaskSlices.insert(source->slices.size());
        source->slices.append({ begin, count, valid });
    }
    m_maskSource = std::move(source);
}

bool CModelDocument::MaskReferencesAllowed(
    const QString& id, const QVector<int>& indices, bool constrainToWorking) const
{
    const MaskDirectory directory(m_drawables);
    const int owner = directory.indices.value(id, -1);
    if (owner >= 0 && constrainToWorking && indices.size() > CMaskEditPolicy::MaximumOverrideReferences &&
        indices != m_drawables[owner].maskIndices) return false;
    return owner >= 0 && m_maskSource
        && m_maskSource->allowed(
            id, indices, directory, constrainToWorking ? &m_drawables[owner].maskIndices : nullptr);
}

bool CModelDocument::ValidateStructureMasks(QString* error) const
{
    if (!m_invalidMaskSlices.isEmpty()) {
        if (error) *error = QStringLiteral("蒙版结构的引用切片越界");
        return false;
    }
    const MaskDirectory directory(m_drawables);
    for (const auto& drawable : m_drawables) {
        if (!m_maskSource || !m_maskSource->allowed(drawable.id, drawable.maskIndices, directory, nullptr)) {
            if (error)
                *error = QStringLiteral("蒙版结构包含新增的重复、越界或自身引用：%1").arg(drawable.id);
            return false;
        }
    }
    return true;
}

QVector<CModelDiagnostic> CModelDocument::MaskDiagnostics() const
{
    QVector<CModelDiagnostic> diagnostics;
    const MaskDirectory directory(m_drawables);
    for (int owner = 0; owner < m_drawables.size(); ++owner) {
        const auto& drawable = m_drawables[owner];
        const auto found = m_drawableMaskIndexOverrides.constFind(drawable.id);
        const auto& masks = found == m_drawableMaskIndexOverrides.cend() ? drawable.maskIndices : *found;
        const auto report = [&](const QString& code, const QString& message,
                                CModelDiagnostic::Severity severity = CModelDiagnostic::Error) {
            diagnostics.append({ severity, code, message, owner, QStringLiteral("artmesh"), drawable.id });
        };
        if (m_invalidMaskSlices.contains(owner))
            report(QStringLiteral("mask_slice"), QStringLiteral("Clipping mask 引用切片越界"));
        QHash<int, int> counts;
        bool retained = false, invalidDuplicates = false;
        for (int mask : masks) {
            if (mask < 0 || mask >= m_drawables.size()) {
                report(QStringLiteral("mask_range"), QStringLiteral("Clipping mask 索引越界"));
                continue;
            }
            if (mask == owner) {
                report(QStringLiteral("mask_self"), QStringLiteral("ArtMesh 不能引用自身作为 Clipping mask"));
                continue;
            }
            const int count = ++counts[mask];
            if (count <= 1)
                continue;
            if (directory.indices.value(drawable.id, -1) != owner
                || directory.indices.value(directory.ids[mask], -1) != mask || !m_maskSource
                || count > m_maskSource->count(drawable.id, directory.ids[mask]))
                invalidDuplicates = true;
            else
                retained = true;
        }
        if (invalidDuplicates)
            report(QStringLiteral("mask_duplicate"), QStringLiteral("Clipping mask 包含无法证明来源的新增重复引用"));
        else if (retained)
            report(QStringLiteral("mask_source_duplicate"),
                QStringLiteral("蒙版沿用了源模型的重复引用次数，已保留当前顺序；重复绘制可能影响半透明边缘"),
                CModelDiagnostic::Warning);
    }
    for (auto it = m_drawableMaskIndexOverrides.cbegin(); it != m_drawableMaskIndexOverrides.cend(); ++it)
        if (directory.indices.value(it.key(), -1) < 0)
            diagnostics.append({ CModelDiagnostic::Error, QStringLiteral("mask_override_id"),
                QStringLiteral("蒙版覆盖引用了不存在或不唯一的 ArtMesh：%1").arg(it.key()), -1,
                QStringLiteral("artmesh"), it.key() });
    return diagnostics;
}
}
