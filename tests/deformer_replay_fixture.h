#ifndef DEFORMER_REPLAY_FIXTURE_H
#define DEFORMER_REPLAY_FIXTURE_H

#include "live2dstruct/cdeformerstructurereplay.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/csectiontable.h"
#include <QCryptographicHash>
#include <QFile>
#include "live2dstruct/csectionvalidation.h"
#include <QtEndian>
#include <cstring>

namespace DeformerReplayFixture {
using namespace live2dstruct;
inline QByteArray read(const QString& name)
{
    QFile file(name == "Mao" ? qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC") : QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
inline CDeformerPreserveRequest insertRequest(const QByteArray& base, const CMocDeformerStructureBuilder::InsertRequest& insertion)
{
    return {insertion, QCryptographicHash::hash(base, QCryptographicHash::Sha256), QCryptographicHash::hash(base, QCryptographicHash::Sha256)};
}
inline CDeformerPreserveRequest removeRequest(const QByteArray& base, const QString& id)
{
    return {CMocDeformerStructureBuilder::RemoveRequest{id}, QCryptographicHash::hash(base, QCryptographicHash::Sha256), QCryptographicHash::hash(base, QCryptographicHash::Sha256)};
}
class RawColumns
{
public:
    explicit RawColumns(QByteArray bytes) : source(std::move(bytes))
    {
        auto reader = CReader::FromBytes(source); CSectionTable table(reader);
        if (!table.IsValid()) return;
        for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            span.width = CSectionValidation::StorageWidth(span); spans.insert(span.name, span);
        }
    }
    QByteArray column(const QByteArray& name) const {
        const auto span = spans.value(name); return state.columns.contains(span.slot) ? state.columns.value(span.slot) : span.offset ? source.mid(span.offset, span.count * span.width) : QByteArray();
    }
    int count(const QByteArray& name) const { const auto s = spans.value(name); return state.counts.value(s.countIndex, s.count); }
    int integer(const QByteArray& name, int row) const { const auto bytes = column(name); return qFromLittleEndian<qint32>(bytes.constData() + row * 4); }
    float number(const QByteArray& name, int row) const { const quint32 bits = integer(name, row); float result; std::memcpy(&result, &bits, 4); return result; }
    void set(const QByteArray& name, int row, int value) {
        auto bytes = column(name); qToLittleEndian(qint32(value), bytes.data() + row * 4); state.columns[spans.value(name).slot] = bytes;
    }
    void setFloat(const QByteArray& name, int row, float value) { int bits; std::memcpy(&bits, &value, 4); set(name, row, bits); }
    QByteArray encode(QString* error = nullptr) const { QByteArray result; CMocParameterStructureBuilder::BuildColumnState(source, state, result, error); return result; }
    QByteArray source;
    QMap<QByteArray, CSectionValidation::ColumnSpan> spans;
    CMocParameterStructureBuilder::ColumnState state;
};
inline CMocDeformerStructureBuilder::InsertRequest underParent(const QByteArray& bytes, const QString& parentId, bool warp)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    CMocDeformerStructureBuilder::InsertRequest result; result.id = "PreservedChild"; result.parentId = parentId;
    int parent = -1;
    for (int i = 0; i < table.deformers.size(); ++i) if (table.deformers[i]->ids == parentId) { parent = i; result.partIndex = table.deformers[i]->parent_part_indices; break; }
    for (const auto* mesh : table.art_meshes) if (mesh->parent_deformer_indices == parent && mesh->parent_part_indices == result.partIndex)
        result.children.append({CMocDeformerStructureBuilder::Owner::ArtMesh, mesh->ids});
    for (const auto* node : table.deformers) if (node->parent_deformer_indices == parent && node->parent_part_indices == result.partIndex)
        result.children.append({node->types == 0 ? CMocDeformerStructureBuilder::Owner::Warp : CMocDeformerStructureBuilder::Owner::Rotation, node->ids});
    if (warp) {
        CMocDeformerStructureBuilder::Warp shape;
        if (parent >= 0 && table.deformers[parent]->types == 0) { const auto* source = table.warp_deformers[table.deformers[parent]->specific_sources_indices]; shape.rows = source->rows; shape.columns = source->columns; shape.quad = source->is_quad_source; }
        result.shape = shape;
    } else result.shape = CMocDeformerStructureBuilder::Rotation{};
    return result;
}
inline QByteArray fixedRotation(const QByteArray& base, const QString& id, float scale, int flags, float angle = 0)
{
    auto reader = CReader::FromBytes(base); CSectionTable table(reader); RawColumns raw(base);
    for (int index = 0; index < table.deformers.size(); ++index) if (table.deformers[index]->ids == id) {
        const int specific = table.deformers[index]->specific_sources_indices;
        const int row = table.rotation_deformers[specific]->keyform_sources_begin_indices;
        int emptyBinding = -1; for (int i = 0; i < table.keyform_bindings.size(); ++i) if (table.keyform_bindings[i]->parameter_binding_index_sources_counts == 0) { emptyBinding = i; break; }
        raw.set("deformers.keyform_binding_sources_indices", index, emptyBinding);
        raw.set("rotation_deformers.keyform_binding_sources_indices", specific, emptyBinding);
        raw.set("rotation_deformers.keyform_sources_counts", specific, 1);
        raw.setFloat("rotation_deformers.base_angles", specific, 0);
        raw.setFloat("rotation_deformer_keyforms.opacities", row, 1);
        raw.setFloat("rotation_deformer_keyforms.angles", row, angle);
        raw.setFloat("rotation_deformer_keyforms.origin_x", row, 0);
        raw.setFloat("rotation_deformer_keyforms.origin_y", row, 0);
        raw.setFloat("rotation_deformer_keyforms.scales", row, scale);
        raw.set("rotation_deformer_keyforms.is_reflect_x", row, flags & 1);
        raw.set("rotation_deformer_keyforms.is_reflect_y", row, (flags >> 1) & 1);
        break;
    }
    return raw.encode();
}
inline CMocDeformerStructureBuilder::InsertRequest rootMesh(const QByteArray& bytes, bool warp)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    CMocDeformerStructureBuilder::InsertRequest result; result.id = "PreservedRoot";
    for (const auto* mesh : table.art_meshes) if (mesh->parent_deformer_indices == -1) {
        result.partIndex = mesh->parent_part_indices;
        result.children.append({CMocDeformerStructureBuilder::Owner::ArtMesh, mesh->ids}); break;
    }
    if (result.children.isEmpty()) for (const auto* node : table.deformers) if (node->parent_deformer_indices == -1) {
        result.partIndex = node->parent_part_indices;
        result.children.append({node->types == 0 ? CMocDeformerStructureBuilder::Owner::Warp : CMocDeformerStructureBuilder::Owner::Rotation, node->ids}); break;
    }
    if (warp) result.shape = CMocDeformerStructureBuilder::Warp{1, 1, false, {-.5f, -.5f, .5f, -.5f, -.5f, .5f, .5f, .5f}};
    else result.shape = CMocDeformerStructureBuilder::Rotation{};
    return result;
}
}

#endif
