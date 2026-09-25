#ifndef DEFORMER_SNAPSHOT_FIXTURE_H
#define DEFORMER_SNAPSHOT_FIXTURE_H

#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <utility>

namespace DeformerSnapshotData {
using namespace live2dstruct;

inline QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}

inline void put(QByteArray& bytes, int offset, int value)
{
    Q_ASSERT(offset >= 0 && offset <= bytes.size() - 4);
    qToLittleEndian(qint32(value), bytes.data() + offset);
}

inline void real(QByteArray& bytes, int offset, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, 4);
    qToLittleEndian(bits, bytes.data() + offset);
}

inline int integer(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

inline bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

inline QMap<QString, float> pose(const CModelDocument& document)
{
    QMap<QString, float> values;
    for (const auto& parameter : document.Parameters()) values.insert(parameter.id, document.ParameterValue(parameter.id));
    return values;
}

inline bool near(const QVector<float>& actual, const QVector<float>& expected)
{
    if (actual.size() != expected.size()) return false;
    for (int index = 0; index < actual.size(); ++index)
        if (!std::isfinite(actual[index]) || std::abs(double(actual[index]) - expected[index]) > 0.00002) return false;
    return true;
}

class Fixture
{
public:
    Fixture(bool quad = true) : reader(sample("Mark")), table(reader), bytes(reader.m_byte)
    {
        if (!table.IsValid()) return;
        const auto& o = *table.sectionOffsetTable;
        for (int index = 0; index < 32; ++index) count(index, 0);
        for (const auto& entry : {std::pair{1, 3}, {2, 1}, {3, 2}, {4, 1}, {5, 1}, {7, 1},
                                  {8, 2}, {9, 2}, {10, 20}, {11, 1}, {12, 1}, {13, 1}, {14, 2}, {15, 6}, {16, 3}})
            count(entry.first, entry.second);
        for (int index = 0; index < 3; ++index) {
            id(o.deformers->ids, index, index == 0 ? "Root" : index == 1 ? "Warp" : "Child");
            column(o.deformers->keyform_binding_sources_indices, index, -1);
            column(o.deformers->is_visible, index, 1);
            column(o.deformers->is_enabled, index, 1);
            column(o.deformers->parent_part_indices, index, -1);
            column(o.deformers->parent_deformer_indices, index, index - 1);
            column(o.deformers->types, index, index == 1 ? 0 : 1);
            column(o.deformers->specific_sources_indices, index, index == 2 ? 1 : 0);
        }
        for (int index = 0; index < 2; ++index) {
            column(o.rotation_deformers->keyform_binding_sources_indices, index, -1);
            column(o.rotation_deformers->keyform_sources_begin_indices, index, index);
            column(o.rotation_deformers->keyform_sources_counts, index, 1);
            real(bytes, o.rotation_deformers->base_angles + index * 4, 0);
            real(bytes, o.rotation_deformer_keyforms->opacities + index * 4, 1);
            real(bytes, o.rotation_deformer_keyforms->angles + index * 4, index == 0 ? 90 : 0);
            real(bytes, o.rotation_deformer_keyforms->origin_x + index * 4, index == 0 ? 10 : 0.25f);
            real(bytes, o.rotation_deformer_keyforms->origin_y + index * 4, index == 0 ? -4 : 0.25f);
            real(bytes, o.rotation_deformer_keyforms->scales + index * 4, index == 0 ? 1 : 0.5f);
            column(o.rotation_deformer_keyforms->is_reflect_x, index, 0);
            column(o.rotation_deformer_keyforms->is_reflect_y, index, 0);
        }
        column(o.warp_deformers->keyform_binding_sources_indices, 0, -1);
        column(o.warp_deformers->keyform_sources_begin_indices, 0, 0);
        column(o.warp_deformers->keyform_sources_counts, 0, 1);
        column(o.warp_deformers->vertex_counts, 0, 4);
        column(o.warp_deformers->rows, 0, 1);
        column(o.warp_deformers->columns, 0, 1);
        appendColumn(0x1d4, {quad ? 1 : 0});
        real(bytes, o.warp_deformer_keyforms->opacities, 1);
        column(o.warp_deformer_keyforms->keyform_position_sources_begin_indices, 0, 12);
        id(o.art_meshes->ids, 0, "Mesh");
        column(o.art_meshes->keyform_binding_sources_indices, 0, 0);
        column(o.art_meshes->keyform_sources_begin_indices, 0, 0);
        column(o.art_meshes->keyform_sources_counts, 0, 2);
        column(o.art_meshes->is_visible, 0, 1);
        column(o.art_meshes->is_enabled, 0, 1);
        column(o.art_meshes->parent_part_indices, 0, -1);
        column(o.art_meshes->parent_deformer_indices, 0, 2);
        column(o.art_meshes->texture_nos, 0, 0);
        bytes[o.art_meshes->drawable_flags] = 4;
        column(o.art_meshes->vertex_counts, 0, 3);
        column(o.art_meshes->uv_sources_begin_indices, 0, 0);
        column(o.art_meshes->position_index_sources_begin_indices, 0, 0);
        column(o.art_meshes->position_index_sources_counts, 0, 3);
        column(o.art_meshes->drawable_mask_sources_begin_indices, 0, 0);
        column(o.art_meshes->drawable_mask_sources_counts, 0, 0);
        for (int index = 0; index < 2; ++index) {
            real(bytes, o.art_mesh_keyforms->opacities + index * 4, 1);
            real(bytes, o.art_mesh_keyforms->draw_orders + index * 4, 0);
            column(o.art_mesh_keyforms->keyform_position_sources_begin_indices, index, index * 6);
        }
        const QVector<float> positions{0, 0.5f, 1, 0, 0, 0, 0.1f, 0.7f, 1.1f, 0.2f, 0.1f, 0.2f,
                                       0, 0, 2, 0, 0, 2, 3, 3};
        for (int index = 0; index < positions.size(); ++index) real(bytes, o.keyform_positions->xys + index * 4, positions[index]);
        for (int index = 0; index < 6; ++index) real(bytes, o.uvs->uvs + index * 4, float(index % 2));
        for (int index = 0; index < 3; ++index) qToLittleEndian(quint16(index), bytes.data() + o.position_indices->indices + index * 2);
        id(o.parameters->ids, 0, "P");
        real(bytes, o.parameters->min_values, 0);
        real(bytes, o.parameters->max_values, 1);
        real(bytes, o.parameters->default_values, 0);
        column(o.parameters->is_repeat, 0, 0);
        column(o.parameters->decimal_places, 0, 3);
        column(o.parameters->parameter_binding_sources_begin_indices, 0, 0);
        column(o.parameters->parameter_binding_sources_counts, 0, 1);
        appendColumn(0x1d8, {0});
        appendColumn(0x1dc, {0});
        appendColumn(0x1e0, {2});
        appendColumn(0x208, {0});
        appendColumn(0x20c, {0});
        appendColumn(0x210, {0});
        column(o.keyform_bindings->parameter_binding_index_sources_begin_indices, 0, 0);
        column(o.keyform_bindings->parameter_binding_index_sources_counts, 0, 1);
        column(o.parameter_binding_indices->binding_sources_indices, 0, 0);
        column(o.parameter_bindings->keys_sources_begin_indices, 0, 0);
        column(o.parameter_bindings->keys_sources_counts, 0, 2);
        real(bytes, o.keys->values, 0);
        real(bytes, o.keys->values + 4, 1);
    }

    void count(int index, int value) { put(bytes, table.sectionOffsetTable->count_info->count_info_offset + index * 4, value); }
    void BindRoot(bool differingReflections)
    {
        const auto& offsets = *table.sectionOffsetTable;
        column(offsets.art_meshes->keyform_binding_sources_indices, 0, -1);
        column(offsets.art_meshes->keyform_sources_counts, 0, 1);
        column(offsets.deformers->keyform_binding_sources_indices, 0, 0);
        column(offsets.rotation_deformers->keyform_sources_counts, 0, 2);
        column(offsets.rotation_deformers->keyform_sources_begin_indices, 1, 2);
        count(8, 3);
        const auto* rotations = offsets.rotation_deformer_keyforms;
        for (const auto offset : {rotations->opacities, rotations->angles, rotations->origin_x, rotations->origin_y,
                                   rotations->scales, rotations->is_reflect_x, rotations->is_reflect_y})
            column(offset, 2, integer(bytes, offset + 4));
        for (const auto offset : {rotations->opacities, rotations->angles, rotations->origin_x, rotations->origin_y,
                                   rotations->scales, rotations->is_reflect_x, rotations->is_reflect_y})
            column(offset, 1, integer(bytes, offset));
        column(rotations->is_reflect_x, 1, differingReflections ? 1 : 0);
    }
    void column(int offset, int row, int value) { put(bytes, offset + row * 4, value); }
    void id(int offset, int row, const QByteArray& value) { bytes.replace(offset + row * 64, 64, value.leftJustified(64, '\0')); }
    void appendColumn(int slot, std::initializer_list<int> values)
    {
        const int offset = bytes.size();
        bytes.append(QByteArray(values.size() * 4, '\0'));
        put(bytes, slot, offset);
        int index = 0;
        for (int value : values) put(bytes, offset + index++ * 4, value);
    }

    CReader reader;
    CSectionTable table;
    QByteArray bytes;
};
}

#endif
