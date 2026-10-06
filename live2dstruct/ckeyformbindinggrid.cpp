#include "ckeyformbindinggrid.h"
#include "csectiontable.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
bool validSlice(qint64 begin, qint64 count, qint64 size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

namespace live2dstruct {

bool CKeyformBindingGrid::Build(const CSectionTable& table, QString* error, bool allowBlendShapePose)
{
    if (!table.IsValid()) return fail(error, QStringLiteral("关键形绑定：源区段表无效"));

    const qsizetype bindingCount = table.parameter_bindings.size();
    QVector<int> ownerCoverage(bindingCount + 1, 0), invalidCoverage(bindingCount + 1, 0);
    QVector<quint32> ownerTokens(bindingCount + 1, 0);
    QHash<QString, int> parameterIdCounts;
    for (const CParameters* parameter : table.parameters)
        if (parameter) ++parameterIdCounts[parameter->ids];
    for (qsizetype index = 0; index < table.parameters.size(); ++index) {
        const CParameters* parameter = table.parameters[index];
        if (!parameter) continue;
        const qint64 begin = parameter->parameter_binding_sources_begin_indices;
        const qint64 count = parameter->parameter_binding_sources_counts;
        if (!validSlice(begin, count, bindingCount)) {
            if (count > 0) {
                const qint64 first = std::max(qint64(0), begin);
                const qint64 end = std::min(qint64(bindingCount), begin + count);
                if (first < end) {
                    ++invalidCoverage[first];
                    --invalidCoverage[end];
                }
            }
            continue;
        }
        ++ownerCoverage[begin];
        --ownerCoverage[begin + count];
        const quint32 token = static_cast<quint32>(index) + 1;
        ownerTokens[begin] ^= token;
        ownerTokens[begin + count] ^= token;
    }

    QVector<float> keys;
    if (table.keys) keys = QVector<float>(table.keys->values.cbegin(), table.keys->values.cend());
    QVector<int> invalidKeys(keys.size() + 1, 0), invalidOrder(keys.size() + 1, 0);
    for (qsizetype index = 0; index < keys.size(); ++index) {
        invalidKeys[index + 1] = invalidKeys[index] + !std::isfinite(keys[index]);
        invalidOrder[index + 1] = invalidOrder[index] + (index > 0 && !(keys[index] > keys[index - 1]));
    }
    QVector<Binding> bindings(bindingCount);
    int owners = 0, invalidOwners = 0;
    quint32 owner = 0;
    for (qsizetype index = 0; index < bindingCount; ++index) {
        owners += ownerCoverage[index];
        invalidOwners += invalidCoverage[index];
        owner ^= ownerTokens[index];
        Binding& binding = bindings[index];
        if (owners != 1 || invalidOwners != 0 || owner == 0 || owner > table.parameters.size()) {
            binding.error = QStringLiteral("参数绑定 %1 的所属参数缺失、重复或区间无效").arg(index);
            continue;
        }
        const CParameters* parameter = table.parameters[owner - 1];
        binding.parameterId = parameter->ids;
        binding.parameterType = parameter->parameter_types;
        const CParameter_bindings* source = table.parameter_bindings[index];
        if (!source || source->keys_sources_counts <= 0 ||
            !validSlice(source->keys_sources_begin_indices, source->keys_sources_counts, keys.size())) {
            binding.error = QStringLiteral("参数绑定 %1 的关键值区间无效").arg(index);
            continue;
        }
        if (parameter->ids.isEmpty() || parameterIdCounts.value(parameter->ids) != 1 ||
            !std::isfinite(parameter->min_values) || !std::isfinite(parameter->max_values) ||
            parameter->min_values > parameter->max_values) {
            binding.error = QStringLiteral("参数绑定 %1 的参数标识或范围无效").arg(index);
            continue;
        }
        const qsizetype begin = source->keys_sources_begin_indices;
        const qsizetype end = begin + source->keys_sources_counts;
        if (invalidKeys[end] != invalidKeys[begin] || invalidOrder[end] != invalidOrder[begin + 1]) {
            binding.error = QStringLiteral("参数绑定 %1 的关键值必须有限且严格递增").arg(index);
            continue;
        }
        binding.keyBegin = source->keys_sources_begin_indices;
        binding.keyCount = source->keys_sources_counts;
        binding.structureValid = true;
        if (parameter->parameter_types != 0)
            binding.error = QStringLiteral("参数 %1 的类型 %2 尚不支持关键形姿态反解")
                                .arg(parameter->ids).arg(parameter->parameter_types);
    }

    const qsizetype referenceCount = table.parameter_binding_indices.size();
    QVector<int> references(referenceCount, -1), invalidReferences(referenceCount + 1, 0);
    QVector<int> unsupportedReferences(referenceCount + 1, 0);
    QVector<qsizetype> nextNonSingleton(referenceCount + 1, referenceCount);
    for (qsizetype index = 0; index < referenceCount; ++index) {
        const CParameter_binding_indices* entry = table.parameter_binding_indices[index];
        const int reference = entry ? entry->binding_sources_indices : -1;
        references[index] = reference;
        const bool valid = reference >= 0 && reference < bindingCount && bindings[reference].structureValid;
        invalidReferences[index + 1] = invalidReferences[index] + !valid;
        unsupportedReferences[index + 1] = unsupportedReferences[index] +
            (valid && bindings[reference].parameterType != 0);
    }
    for (qsizetype index = referenceCount; index-- > 0;) {
        const int reference = references[index];
        nextNonSingleton[index] = reference >= 0 && reference < bindingCount &&
            bindings[reference].structureValid && bindings[reference].keyCount > 1
            ? index : nextNonSingleton[index + 1];
    }
    const auto firstMarked = [](const QVector<int>& prefix, qsizetype begin, qsizetype end) {
        const auto found = std::lower_bound(prefix.cbegin() + begin + 1, prefix.cbegin() + end + 1,
                                            prefix[begin] + 1);
        return static_cast<qsizetype>(found - prefix.cbegin() - 1);
    };

    std::array<QHash<QString, Grid>, 3> grids;
    const auto buildGrid = [&](int bindingIndex, int begin, int count, qsizetype available) {
        Grid grid;
        grid.keyformCount = count;
        if (count <= 0 || !validSlice(begin, count, available)) {
            grid.error = QStringLiteral("关键形源区间无效");
        }
        if (bindingIndex != -1) {
            if (bindingIndex < 0 || bindingIndex >= table.keyform_bindings.size() ||
                !table.keyform_bindings[bindingIndex]) {
                grid.error = QStringLiteral("关键形绑定索引无效");
                return grid;
            }
            const CKeyform_bindings* source = table.keyform_bindings[bindingIndex];
            if (!validSlice(source->parameter_binding_index_sources_begin_indices,
                            source->parameter_binding_index_sources_counts, referenceCount)) {
                grid.error = QStringLiteral("关键形绑定轴区间无效");
                return grid;
            }
            grid.axisBegin = source->parameter_binding_index_sources_begin_indices;
            grid.axisCount = source->parameter_binding_index_sources_counts;
        }
        const qsizetype first = grid.axisBegin, end = first + grid.axisCount;
        if (invalidReferences[end] != invalidReferences[first]) {
            const int reference = references[firstMarked(invalidReferences, first, end)];
            grid.error = reference >= 0 && reference < bindingCount ? bindings[reference].error
                : QStringLiteral("关键形绑定轴引用无效");
            return grid;
        }
        grid.axesValid = true;
        qint64 product = 1;
        for (qsizetype axis = nextNonSingleton[first]; axis < end; axis = nextNonSingleton[axis + 1]) {
            const int keyCount = bindings[references[axis]].keyCount;
            if (product > std::numeric_limits<qint64>::max() / keyCount) {
                grid.expectedKeyformCount = -1;
                grid.error = QStringLiteral("绑定轴关键值数量的乘积与关键形数量不符");
                return grid;
            }
            product *= keyCount;
        }
        grid.expectedKeyformCount = product;
        grid.missingKeyformCount = count >= 0 && product >= count ? product - count : -1;
        if (product != count) {
            if (grid.error.isEmpty()) grid.error = QStringLiteral("绑定轴关键值数量的乘积与关键形数量不符");
            return grid;
        }
        if (!grid.error.isEmpty()) return grid;
        grid.structureValid = true;
        if (!allowBlendShapePose && unsupportedReferences[end] != unsupportedReferences[first])
            grid.error = bindings[references[firstMarked(unsupportedReferences, first, end)]].error;
        return grid;
    };
    const auto insert = [&](ObjectType type, const QString& id, Grid grid) {
        if (id.isEmpty()) {
            grid.structureValid = false;
            grid.axesValid = false;
            grid.error = QStringLiteral("对象标识为空");
        }
        auto& objects = grids[static_cast<size_t>(type)];
        auto previous = objects.find(id);
        if (previous != objects.end()) {
            previous->structureValid = false;
            previous->axesValid = false;
            previous->error = QStringLiteral("对象标识重复：%1").arg(id);
        }
        else objects.insert(id, std::move(grid));
    };
    QSet<int> blendMeshes, blendWarps, blendRotations;
    for (const CBlend_shapes_art_meshes* blend : table.blend_shapes_art_meshes)
        if (blend) blendMeshes.insert(blend->target_indices);
    for (const CBlend_shapes_warp_deformers* blend : table.blend_shapes_warp_deformers)
        if (blend) blendWarps.insert(blend->target_indices);
    for (const CBlend_shape_targets* blend : table.blend_shapes_rotation_deformers)
        if (blend) blendRotations.insert(blend->target_indices);
    for (qsizetype index = 0; index < table.art_meshes.size(); ++index) {
        const CArt_meshes* source = table.art_meshes[index];
        if (!source) continue;
        Grid grid = buildGrid(source->keyform_binding_sources_indices, source->keyform_sources_begin_indices,
                              source->keyform_sources_counts, table.art_mesh_keyforms.size());
        if (!allowBlendShapePose && blendMeshes.contains(static_cast<int>(index)) && grid.error.isEmpty())
            grid.error = QStringLiteral("对象包含尚不支持姿态反解的 BlendShape");
        insert(ArtMesh, source->ids, std::move(grid));
    }
    for (const CDeformers* source : table.deformers) {
        if (!source) continue;
        const int specific = source->specific_sources_indices;
        Grid grid;
        if (source->types == 0) {
            if (specific >= 0 && specific < table.warp_deformers.size() && table.warp_deformers[specific]) {
                const CWarp_deformers* warp = table.warp_deformers[specific];
                grid = buildGrid(source->keyform_binding_sources_indices, warp->keyform_sources_begin_indices,
                                 warp->keyform_sources_counts, table.warp_deformer_keyforms.size());
                if (!allowBlendShapePose && blendWarps.contains(specific) && grid.error.isEmpty())
                    grid.error = QStringLiteral("对象包含尚不支持姿态反解的 BlendShape");
            } else grid.error = QStringLiteral("Warp 专用源索引无效");
            insert(Warp, source->ids, std::move(grid));
        } else if (source->types == 1) {
            if (specific >= 0 && specific < table.rotation_deformers.size() && table.rotation_deformers[specific]) {
                const CRotation_deformers* rotation = table.rotation_deformers[specific];
                grid = buildGrid(source->keyform_binding_sources_indices, rotation->keyform_sources_begin_indices,
                                 rotation->keyform_sources_counts, table.rotation_deformer_keyforms.size());
                if (!allowBlendShapePose && blendRotations.contains(specific) && grid.error.isEmpty())
                    grid.error = QStringLiteral("对象包含尚不支持姿态反解的 BlendShape");
            } else grid.error = QStringLiteral("Rotation 专用源索引无效");
            insert(Rotation, source->ids, std::move(grid));
        } else {
            grid.error = QStringLiteral("变形器类型无效：%1").arg(source->types);
            insert(Warp, source->ids, grid);
            insert(Rotation, source->ids, std::move(grid));
        }
    }
    m_grids = std::move(grids);
    m_keys = std::move(keys);
    m_bindings = std::move(bindings);
    m_references = std::move(references);
    if (error) error->clear();
    return true;
}

const CKeyformBindingGrid::Grid* CKeyformBindingGrid::Find(ObjectType type, const QString& id) const
{
    if (type < ArtMesh || type > Rotation) return nullptr;
    const auto& objects = m_grids[static_cast<size_t>(type)];
    const auto found = objects.constFind(id);
    return found == objects.cend() ? nullptr : &found.value();
}

bool CKeyformBindingGrid::Pose(ObjectType type, const QString& id, int keyformIndex,
                             QMap<QString, float>& values, QString* error) const
{
    const Grid* grid = Find(type, id);
    if (!grid) return fail(error, QStringLiteral("未找到对象的关键形绑定：%1").arg(id));
    if (!grid->error.isEmpty()) return fail(error, grid->error);
    if (keyformIndex < 0 || keyformIndex >= grid->keyformCount)
        return fail(error, QStringLiteral("关键形索引越界：%1").arg(keyformIndex));
    QMap<QString, float> candidate;
    int remainder = keyformIndex;
    for (int offset = 0; offset < grid->axisCount; ++offset) {
        const Binding& binding = m_bindings[m_references[grid->axisBegin + offset]];
        const int keyIndex = remainder % binding.keyCount;
        remainder /= binding.keyCount;
        const float value = m_keys[binding.keyBegin + keyIndex];
        const auto existing = candidate.constFind(binding.parameterId);
        if (existing != candidate.cend() && existing.value() != value)
            return fail(error, QStringLiteral("同一参数 %1 的绑定轴要求不同关键值").arg(binding.parameterId));
        candidate.insert(binding.parameterId, value);
    }
    if (remainder != 0) return fail(error, QStringLiteral("关键形索引无法完整反解"));
    values = std::move(candidate);
    if (error) error->clear();
    return true;
}

QVector<CKeyformBindingGrid::Axis> CKeyformBindingGrid::Axes(ObjectType type, const QString& id) const
{
    const Grid* grid = Find(type, id);
    if (!grid || !grid->structureValid) return {};
    return CollectAxes(*grid);
}

QVector<CKeyformBindingGrid::Axis> CKeyformBindingGrid::CollectAxes(const Grid& grid) const
{
    QVector<Axis> axes;
    axes.reserve(grid.axisCount);
    QHash<QPair<int, int>, QVector<float>> slices;
    for (int offset = 0; offset < grid.axisCount; ++offset) {
        const int reference = m_references[grid.axisBegin + offset];
        const Binding& binding = m_bindings[reference];
        const QPair<int, int> range{binding.keyBegin, binding.keyCount};
        auto found = slices.constFind(range);
        if (found == slices.cend()) {
            const auto begin = m_keys.cbegin() + binding.keyBegin;
            slices.insert(range, QVector<float>(begin, begin + binding.keyCount));
            found = slices.constFind(range);
        }
        axes.append({reference, binding.parameterId, binding.parameterType, found.value()});
    }
    return axes;
}

CKeyformBindingGrid::Description CKeyformBindingGrid::Describe(ObjectType type, const QString& id) const
{
    const auto* grid = Find(type, id);
    if (!grid) return {{}, 0, 0, -1, false, QStringLiteral("未找到对象的关键形绑定：%1").arg(id)};
    return {grid->axesValid ? CollectAxes(*grid) : QVector<Axis>{}, grid->keyformCount,
            grid->expectedKeyformCount, grid->missingKeyformCount, grid->axesValid, grid->error};
}

QString CKeyformBindingGrid::Diagnostic(ObjectType type, const QString& id) const
{
    const Grid* grid = Find(type, id);
    return grid ? grid->error : QStringLiteral("未找到对象的关键形绑定：%1").arg(id);
}

}
