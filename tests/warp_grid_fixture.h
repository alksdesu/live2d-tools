#ifndef WARP_GRID_FIXTURE_H
#define WARP_GRID_FIXTURE_H

#include "deformer_replay_fixture.h"
#include "live2dstruct/coor/cwarpcoortype.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace WarpGridFixture {
using namespace live2dstruct;
using Raw = CMocDeformerStructureBuilder;
using Codec = CMocParameterStructureBuilder;

inline int target(const CSectionTable& table, bool blend = false)
{
    for (int index = 0; index < table.deformers.size(); ++index) {
        const auto* node = table.deformers[index];
        if (node->types != 0)
            continue;
        const auto* warp = table.warp_deformers[node->specific_sources_indices];
        if (warp->rows < 1 || warp->columns < 1 || warp->rows > 12 || warp->columns > 12
            || warp->keyform_sources_counts < 1)
            continue;
        const bool hasBlend = std::any_of(table.blend_shapes_warp_deformers.cbegin(),
            table.blend_shapes_warp_deformers.cend(), [&](const auto* shape) {
                return shape->target_indices == node->specific_sources_indices
                    && shape->blend_shape_keyform_binding_sources_counts > 0;
            });
        if (blend == hasBlend)
            return index;
    }
    return -1;
}

inline QVector<QPair<QString, int>> formRows(const CSectionTable& table, int specific)
{
    const auto* warp = table.warp_deformers[specific];
    QVector<QPair<QString, int>> result;
    for (int form = 0; form < warp->keyform_sources_counts; ++form)
        result.append({ QString("ordinary/%1").arg(form), warp->keyform_sources_begin_indices + form });
    int ordinal = 0;
    for (const auto* shape : table.blend_shapes_warp_deformers) {
        if (shape->target_indices != specific)
            continue;
        for (int local = 0; local < shape->blend_shape_keyform_binding_sources_counts; ++local) {
            const auto* binding
                = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices + local];
            for (int form = 0; form < binding->keyform_sources_blend_shape_counts; ++form)
                result.append({ QString("blend/%1/%2/%3").arg(ordinal).arg(local).arg(form),
                    binding->keyform_sources_blend_shape_indices + form });
        }
        ++ordinal;
    }
    return result;
}

inline QVector<float> positions(const CSectionTable& table, int row, int count)
{
    const int begin = table.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices;
    const auto first = table.keyform_positions->xys.cbegin() + begin;
    return QVector<float>(first, first + count * 2);
}

inline QByteArray floatBytes(const std::vector<float>& source, int first, int count)
{
    return QByteArray(reinterpret_cast<const char*>(source.data() + first), count * sizeof(float));
}

inline QMap<QString, QByteArray> siblingPositions(const CSectionTable& table, const QString& omitted)
{
    QMap<QString, QByteArray> result;
    for (const auto* node : table.deformers) {
        if (node->types != 0 || node->ids == omitted)
            continue;
        const auto* warp = table.warp_deformers[node->specific_sources_indices];
        for (const auto& form : formRows(table, node->specific_sources_indices)) {
            const int begin = table.warp_deformer_keyforms[form.second]->keyform_position_sources_begin_indices;
            result.insert("warp/" + node->ids + '/' + form.first,
                floatBytes(table.keyform_positions->xys, begin, warp->vertex_counts * 2));
        }
    }
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        for (int local = 0; local < mesh->keyform_sources_counts; ++local) {
            const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + local]
                                  ->keyform_position_sources_begin_indices;
            result.insert("mesh/" + mesh->ids + '/' + QString::number(local),
                floatBytes(table.keyform_positions->xys, begin, mesh->vertex_counts * 2));
        }
    }
    for (int index = 0; index < table.blend_shapes_art_meshes.size(); ++index) {
        const auto* shape = table.blend_shapes_art_meshes[index];
        const auto* mesh = table.art_meshes[shape->target_indices];
        for (int local = 0; local < shape->blend_shape_keyform_binding_sources_counts; ++local) {
            const auto* binding
                = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices + local];
            for (int form = 0; form < binding->keyform_sources_blend_shape_counts; ++form) {
                const int begin = table.art_mesh_keyforms[binding->keyform_sources_blend_shape_indices + form]
                                      ->keyform_position_sources_begin_indices;
                result.insert(QString("meshBlend/%1/%2/%3").arg(index).arg(local).arg(form),
                    floatBytes(table.keyform_positions->xys, begin, mesh->vertex_counts * 2));
            }
        }
    }
    return result;
}

inline QVector<int> poolCounts(const CSectionTable& table)
{
    return { int(table.keyform_positions->xys.size()), int(table.warp_deformer_keyforms.size()),
        int(table.blend_shape_keyform_bindings.size()), int(table.warp_deformers.size()),
        int(table.blend_shapes_warp_deformers.size()) };
}

inline QVector<QVector<int>> bindingMetadata(const CSectionTable& table, int specific)
{
    QVector<QVector<int>> result;
    for (const auto* shape : table.blend_shapes_warp_deformers) {
        if (shape->target_indices != specific)
            continue;
        result.append({ shape->blend_shape_keyform_binding_sources_counts });
        for (int local = 0; local < shape->blend_shape_keyform_binding_sources_counts; ++local) {
            const auto* binding
                = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices + local];
            result.append({ binding->parameter_binding_sources_indices, binding->keyform_sources_blend_shape_counts,
                binding->blend_shape_constraint_index_sources_begin_indices,
                binding->blend_shape_constraint_index_sources_counts });
        }
    }
    return result;
}

inline QMap<QByteArray, QByteArray> fixedColumns(const QByteArray& bytes, const CSectionTable& table)
{
    QMap<QByteArray, QByteArray> result;
    for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        const QByteArray name(span.name);
        if (name.startsWith("parameters.") || name.startsWith("parameter_") || name.startsWith("keyform_bindings.")
            || name.startsWith("parts.") || name.startsWith("part_keyforms.") || name.startsWith("rotation_")
            || name.startsWith("glue") || name.startsWith("art_meshes.") || name == "uvs.uvs"
            || name == "position_indices.indices")
            result.insert(name,
                span.offset ? bytes.mid(span.offset, span.count * CSectionValidation::StorageWidth(span))
                            : QByteArray());
    }
    return result;
}

inline QVector<float> floatInputOracle(const CSectionTable& table, int specific, int form, int rows, int columns)
{
    const auto* warp = table.warp_deformers[specific];
    const auto source = positions(table, form, warp->vertex_counts);
    CWarpCoorType transform(
        warp->columns, warp->rows, std::vector<float>(source.cbegin(), source.cend()), warp->is_quad_source);
    std::vector<float> points;
    for (int row = 0; row <= rows; ++row)
        for (int column = 0; column <= columns; ++column) {
            points.push_back(float(column) / columns);
            points.push_back(float(row) / rows);
        }
    std::vector<float> result(points.size());
    if (!transform.transformLocalToCanvas(points.data(), result.data(), points.size() / 2, 0, 2))
        return { };
    return QVector<float>(result.cbegin(), result.cend());
}

inline QVector<float> oracle(const CSectionTable& table, int specific, int form, int rows, int columns)
{
    const auto* warp = table.warp_deformers[specific];
    const auto source = positions(table, form, warp->vertex_counts);
    QVector<float> result;
    const qint64 denominator = qint64(rows) * columns;
    for (int row = 0; row <= rows; ++row)
        for (int column = 0; column <= columns; ++column) {
            const qint64 x = qint64(column) * warp->columns, y = qint64(row) * warp->rows;
            const int cellX = std::min(int(x / columns), warp->columns - 1);
            const int cellY = std::min(int(y / rows), warp->rows - 1);
            const qint64 dx = x - qint64(cellX) * columns, dy = y - qint64(cellY) * rows;
            const int a = cellY * (warp->columns + 1) + cellX, b = a + 1, c = a + warp->columns + 1, d = c + 1;
            for (int axis = 0; axis < 2; ++axis) {
                double numerator;
                if (warp->is_quad_source) {
                    numerator = double(source[a * 2 + axis]) * ((columns - dx) * (rows - dy))
                        + double(source[b * 2 + axis]) * (dx * (rows - dy))
                        + double(source[c * 2 + axis]) * ((columns - dx) * dy)
                        + double(source[d * 2 + axis]) * (dx * dy);
                } else if (dx * rows + dy * columns <= denominator) {
                    numerator = double(source[a * 2 + axis]) * (denominator - dx * rows - dy * columns)
                        + double(source[b * 2 + axis]) * (dx * rows) + double(source[c * 2 + axis]) * (dy * columns);
                } else {
                    numerator = double(source[d * 2 + axis]) * (dx * rows + dy * columns - denominator)
                        + double(source[c * 2 + axis]) * ((columns - dx) * rows)
                        + double(source[b * 2 + axis]) * ((rows - dy) * columns);
                }
                result.append(float(numerator / denominator));
            }
        }
    return result;
}

inline bool close(const QVector<float>& expected, const QVector<float>& actual)
{
    if (expected.size() != actual.size())
        return false;
    for (int index = 0; index < expected.size(); ++index)
        if (!std::isfinite(actual[index])
            || std::abs(double(actual[index]) - expected[index]) > 8 * std::numeric_limits<float>::epsilon()
                    * std::max({ 1., std::abs(double(actual[index])), std::abs(double(expected[index])) }))
            return false;
    return true;
}
}

#endif
