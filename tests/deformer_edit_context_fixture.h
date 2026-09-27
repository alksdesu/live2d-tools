#ifndef DEFORMER_EDIT_CONTEXT_FIXTURE_H
#define DEFORMER_EDIT_CONTEXT_FIXTURE_H

#include "live2dstruct/cdeformereditcontext.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"
#include <QtEndian>

using namespace live2dstruct;
namespace {
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void real(QByteArray& bytes, int offset, float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, bytes.data() + offset);
}
struct Fixture
{
    QByteArray bytes;
    QString id, type, witness;
    QVector<float> localPoints;
    int targetIndex = -1, witnessIndex = -1;
};
Fixture fixture(const QString& type, const QString& parent, float scale, int reflections)
{
    Fixture result;
    result.bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    auto reader = CReader::FromBytes(result.bytes); CSectionTable table(reader);
    if (!table.IsValid()) return {};
    const auto& o = *table.sectionOffsetTable;
    int warpIndex = -1, rootIndex = -1, childIndex = -1;
    for (int index = 0; index < table.deformers.size(); ++index) {
        const auto* node = table.deformers[index];
        if (node->types == 0 && node->specific_sources_indices == 0) warpIndex = index;
        if (node->types == 1 && node->specific_sources_indices == 0) rootIndex = index;
        if (node->types == 1 && node->specific_sources_indices == 1) childIndex = index;
    }
    if (warpIndex < 0 || rootIndex < 0 || childIndex < 0) return {};
    result.type = type; result.targetIndex = type == "warp" ? warpIndex : childIndex;
    result.id = table.deformers[result.targetIndex]->ids;
    int largest = 0;
    for (int index = 0; index < table.art_meshes.size(); ++index)
        if (table.art_meshes[index]->vertex_counts > largest) { largest = table.art_meshes[index]->vertex_counts; result.witnessIndex = index; }
    if (result.witnessIndex < 0) return {};
    const auto* mesh = table.art_meshes[result.witnessIndex]; result.witness = mesh->ids;
    put(result.bytes, o.art_meshes->parent_deformer_indices + result.witnessIndex * 4, result.targetIndex);
    put(result.bytes, o.deformers->parent_deformer_indices + rootIndex * 4, -1);
    put(result.bytes, o.deformers->parent_deformer_indices + warpIndex * 4, parent == "nested" || (type == "warp" && parent == "rotation") ? rootIndex : -1);
    put(result.bytes, o.deformers->parent_deformer_indices + childIndex * 4,
        parent == "root" ? -1 : parent == "rotation" ? rootIndex : warpIndex);
    const auto patchRotation = [&](int specific, float angle, float originX, float originY, float ownScale, int reflect, float base) {
        const auto* source = table.rotation_deformers[specific];
        real(result.bytes, o.rotation_deformers->base_angles + specific * 4, base);
        for (int index = 0; index < source->keyform_sources_counts; ++index) {
            const int row = source->keyform_sources_begin_indices + index;
            real(result.bytes, o.rotation_deformer_keyforms->angles + row * 4, angle);
            real(result.bytes, o.rotation_deformer_keyforms->origin_x + row * 4, originX);
            real(result.bytes, o.rotation_deformer_keyforms->origin_y + row * 4, originY);
            real(result.bytes, o.rotation_deformer_keyforms->scales + row * 4, ownScale);
            put(result.bytes, o.rotation_deformer_keyforms->is_reflect_x + row * 4, reflect & 1);
            put(result.bytes, o.rotation_deformer_keyforms->is_reflect_y + row * 4, (reflect >> 1) & 1);
        }
    };
    patchRotation(0, 23, 0.4f, -0.3f, parent == "rotation" || parent == "nested" ? -0.7f : 1, 1, 11);
    patchRotation(1, 31, 0.27f, 0.38f, scale, reflections, 17);
    const auto* warp = table.warp_deformers[0];
    if (mesh->vertex_counts < warp->vertex_counts) return {};
    put(result.bytes, o.warp_deformers->is_quad_source, 1);
    for (int key = 0; key < warp->keyform_sources_counts; ++key) {
        const int begin = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int y = 0; y <= warp->rows; ++y) for (int x = 0; x <= warp->columns; ++x) {
            const float u = float(x) / warp->columns, v = float(y) / warp->rows;
            const float px = parent == "folded" ? 2 * u + 2 * v - 4 * u * v : 0.15f + 0.8f * u + 0.3f * v + 0.2f * u * v;
            const float py = parent == "folded" ? 2 * v : -0.1f + 0.9f * v + 0.15f * u * v;
            real(result.bytes, o.keyform_positions->xys + (begin + 2 * (y * (warp->columns + 1) + x)) * 4, px);
            real(result.bytes, o.keyform_positions->xys + (begin + 2 * (y * (warp->columns + 1) + x) + 1) * 4, py);
            if (key == 0 && type == "warp") result.localPoints << px << py;
        }
    }
    if (type == "rotation") result.localPoints = {0.27f, 0.38f};
    for (int key = 0; key < mesh->keyform_sources_counts; ++key) {
        const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
            const int gridVertex = vertex % warp->vertex_counts;
            const float x = type == "rotation" ? (vertex % 3 == 1 ? 1 : 0) : float(gridVertex % (warp->columns + 1)) / warp->columns;
            const float y = type == "rotation" ? (vertex % 3 == 2 ? 1 : 0) : float(gridVertex / (warp->columns + 1)) / warp->rows;
            real(result.bytes, o.keyform_positions->xys + (begin + vertex * 2) * 4, x);
            real(result.bytes, o.keyform_positions->xys + (begin + vertex * 2 + 1) * 4, y);
        }
    }
    QByteArray normalized;
    if (!CMocDeformerOrder::Normalize(result.bytes, normalized)) return {};
    result.bytes = normalized;
    auto normalizedReader = CReader::FromBytes(result.bytes); CSectionTable normalizedTable(normalizedReader);
    if (!normalizedTable.IsValid()) return {};
    for (int index = 0; index < normalizedTable.deformers.size(); ++index)
        if (normalizedTable.deformers[index]->ids == result.id) result.targetIndex = index;
    return result;
}
QMap<QString, float> targetPose(const CModelDocument& document, const Fixture& source)
{
    QMap<QString, float> pose, corner;
    for (const auto& parameter : document.Parameters()) pose[parameter.id] = parameter.defaultValue;
    if (!document.KeyformParameterValues(source.id, source.type, 0, corner)) return {};
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) pose[it.key()] = it.value();
    return pose;
}
void setPose(CoreModel& core, const QMap<QString, float>& pose)
{
    QVector<float> values;
    for (int index = 0; index < csmGetParameterCount(core.value); ++index)
        values.append(pose.value(QString::fromUtf8(csmGetParameterIds(core.value)[index])));
    core.pose(values);
}
QVector<float> witnessPoints(CoreModel& core, const Fixture& source, int count)
{
    const int index = source.witnessIndex;
    if (index < 0 || QString::fromUtf8(csmGetDrawableIds(core.value)[index]) != source.witness ||
        csmGetDrawableVertexCounts(core.value)[index] < count) return {};
    QVector<float> points;
    const auto* values = csmGetDrawableVertexPositions(core.value)[index];
    for (int vertex = 0; vertex < count; ++vertex) points << values[vertex].X << values[vertex].Y;
    return points;
}
bool near(const QVector<float>& a, const QVector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(double(a[i]) - b[i]) >
            CKeyformMappingContext::AbsoluteTolerance + CKeyformMappingContext::RelativeTolerance * std::max(std::abs(double(a[i])), std::abs(double(b[i])))) return false;
    return true;
}
}


#endif
