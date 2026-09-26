#include "cmocmeshstructurebuilder.h"
#include "cmeshtopology.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QtEndian>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocMeshStructureBuilder;
constexpr int MaximumCount = 1000000;

bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool range(qint64 first, qint64 count, qint64 size) { return first >= 0 && count >= 0 && first <= size && count <= size - first; }
QByteArray encodedInt(int value)
{
    QByteArray result(4, Qt::Uninitialized); qToLittleEndian(qint32(value), result.data()); return result;
}
float decodedFloat(const char* bytes)
{
    const quint32 bits = qFromLittleEndian<quint32>(bytes); float result; std::memcpy(&result, &bits, 4); return result;
}
void appendFloat(QByteArray& bytes, float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4); char encoded[4]; qToLittleEndian(bits, encoded); bytes.append(encoded, 4);
}
void appendIndex(QByteArray& bytes, int value)
{
    char encoded[2]; qToLittleEndian(quint16(value), encoded); bytes.append(encoded, 2);
}

class Columns
{
public:
    Columns(const QByteArray& source, const CSectionTable& table) : bytes(source)
    {
        const auto schema = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        for (const auto& column : schema) {
            spans.insert(QByteArray(column.name), column);
            counts.insert(column.countIndex, int(column.count));
        }
    }
    const CSectionValidation::ColumnSpan& span(const QByteArray& name) const
    {
        const auto found = spans.constFind(name);
        if (found == spans.cend()) throw std::length_error("Unknown MOC mesh column");
        return *found;
    }
    QByteArray data(const QByteArray& name) const
    {
        const auto& column = span(name);
        const auto found = state.columns.constFind(column.slot);
        if (found != state.columns.cend()) return *found;
        if (!column.offset) return {};
        return bytes.mid(column.offset, column.count * column.width);
    }
    QByteArray cell(const QByteArray& name, int row) const
    {
        const auto& column = span(name); const auto values = data(name);
        if (!range(row, 1, values.size() / column.width)) throw std::length_error("MOC mesh row outside column");
        return values.mid(qint64(row) * column.width, column.width);
    }
    QByteArray& writable(const QByteArray& name)
    {
        const auto& column = span(name);
        auto found = state.columns.find(column.slot);
        if (found == state.columns.end()) found = state.columns.insert(column.slot, data(name));
        return *found;
    }
    void set(const QByteArray& name, int row, int value)
    {
        const auto& column = span(name);
        if (column.width != 4 || !range(row, 1, counts.value(column.countIndex))) throw std::length_error("Invalid MOC mesh field");
        auto found = state.columns.find(column.slot);
        if (found == state.columns.end()) found = state.columns.insert(column.slot, data(name));
        const auto encoded = encodedInt(value);
        std::memcpy(found->data() + qint64(row) * 4, encoded.constData(), 4);
    }
    int appendRows(const QByteArray& name, int first, int count)
    {
        const auto group = name.left(name.indexOf('.'));
        const auto& primary = span(name);
        const int previous = counts.value(primary.countIndex);
        if (!range(first, count, previous) || count > MaximumCount - previous) throw std::length_error("MOC mesh keyform count limit");
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            const auto& column = *it;
            if (column.countIndex != primary.countIndex) continue;
            if (column.optional && !column.offset && !state.columns.contains(column.slot)) continue;
            if (data(it.key()).size() != qint64(previous) * column.width || !it.key().startsWith(group + '.'))
                throw std::length_error("MOC mesh row group length mismatch");
            QByteArray added;
            {
                const auto original = data(it.key());
                added = QByteArray(original.constData() + qint64(first) * column.width, qint64(count) * column.width);
            }
            writable(it.key()).append(added);
        }
        counts[primary.countIndex] = previous + count; state.counts[primary.countIndex] = previous + count;
        return previous;
    }
    int appendPool(const QByteArray& name, const QByteArray& payload)
    {
        const auto& column = span(name); const int previous = counts.value(column.countIndex);
        if (payload.size() % column.width || payload.size() / column.width > MaximumCount - previous)
            throw std::length_error("MOC mesh pool count limit");
        auto& next = writable(name);
        if (next.size() != qint64(previous) * column.width) throw std::length_error("MOC mesh pool length mismatch");
        next.append(payload);
        counts[column.countIndex] = previous + payload.size() / column.width;
        state.counts[column.countIndex] = counts[column.countIndex];
        return previous;
    }
    void appendGlue(const QByteArray& indices, const QByteArray& weights, int& begin)
    {
        if (indices.size() % 2 || weights.size() != indices.size() * 2) throw std::length_error("Glue pair columns mismatch");
        const auto& indexColumn = span("glue_info.position_indices");
        begin = counts.value(indexColumn.countIndex);
        const int added = indices.size() / 2;
        if (added > MaximumCount - begin) throw std::length_error("Glue pair count limit");
        if (data("glue_info.position_indices").size() != qint64(begin) * 2 || data("glue_info.weights").size() != qint64(begin) * 4)
            throw std::length_error("Glue pair source length mismatch");
        writable("glue_info.position_indices").append(indices);
        writable("glue_info.weights").append(weights);
        counts[indexColumn.countIndex] = begin + added; state.counts[indexColumn.countIndex] = begin + added;
    }
    CMocParameterStructureBuilder::ColumnState state;
private:
    const QByteArray& bytes;
    QMap<QByteArray, CSectionValidation::ColumnSpan> spans;
    QMap<int, int> counts;
};

struct MeshPlan {
    int index = -1;
    Builder::MeshEdit edit;
    Builder::MeshRemap remap;
    bool geometry = false;
    bool topology = false;
    bool uvChanged = false;
    bool remapsVertices = false;
    QByteArray uvs;
};
struct GluePlan {
    Builder::GlueChange change;
    QByteArray indices, weights;
};
struct Plan {
    QVector<MeshPlan> meshes;
    QVector<GluePlan> glues;
    Builder::Impact impact;
};

bool resample(const std::vector<float>& source, int first, int oldCount, const MeshPlan& mesh, QByteArray& result, QString* error);

bool checkMesh(const CSectionTable& table, int index, const Builder::MeshEdit& request, MeshPlan& plan, QString* error)
{
    const auto* mesh = table.art_meshes[index];
    if (mesh->vertex_counts < 0 || mesh->vertex_counts > Builder::MaximumVertices ||
        request.vertices.isEmpty() || request.vertices.size() > Builder::MaximumVertices ||
        request.oldToNew.size() != mesh->vertex_counts || !CMeshTopology::IsValid(request.indices, request.vertices.size()))
        return fail(error, QStringLiteral("网格顶点数、映射或三角索引无效：%1").arg(mesh->ids));
    if (!range(mesh->uv_sources_begin_indices, qint64(mesh->vertex_counts) * 2, table.uvs->uvs.size()) ||
        !range(mesh->position_index_sources_begin_indices, mesh->position_index_sources_counts, table.position_indices->indices.size()) ||
        mesh->position_index_sources_counts < 0 || mesh->position_index_sources_counts % 3 ||
        !range(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, table.art_mesh_keyforms.size()))
        return fail(error, QStringLiteral("源网格的 UV、索引或关键形范围无效：%1").arg(mesh->ids));
    plan.index = index; plan.edit = request;
    plan.remap = {mesh->ids, mesh->vertex_counts, int(request.vertices.size()), request.oldToNew, QVector<int>(request.vertices.size(), -1)};
    qint64 weightsTotal = 0;
    for (int vertex = 0; vertex < plan.edit.vertices.size(); ++vertex) {
        auto& source = plan.edit.vertices[vertex];
        weightsTotal += source.indices.size();
        if (source.indices.isEmpty() || source.indices.size() != source.weights.size() || weightsTotal > MaximumCount)
            return fail(error, QStringLiteral("顶点插值来源数量无效：%1[%2]").arg(mesh->ids).arg(vertex));
        double sum = 0;
        for (int i = 0; i < source.indices.size(); ++i) {
            if (source.indices[i] < 0 || source.indices[i] >= mesh->vertex_counts || !std::isfinite(source.weights[i]) || source.weights[i] < 0)
                return fail(error, QStringLiteral("顶点插值索引或权重无效：%1[%2]").arg(mesh->ids).arg(vertex));
            sum += source.weights[i];
        }
        if (!std::isfinite(sum) || std::abs(sum - 1.0) > Builder::WeightTolerance)
            return fail(error, QStringLiteral("顶点插值权重之和必须为 1：%1[%2]").arg(mesh->ids).arg(vertex));
        for (double& weight : source.weights) weight /= sum;
        if (source.indices.size() != 1 || source.indices.first() != vertex || source.weights.first() != 1)
            plan.geometry = true;
    }
    plan.geometry = plan.geometry || request.vertices.size() != mesh->vertex_counts;
    for (int old = 0; old < request.oldToNew.size(); ++old) {
        const int next = request.oldToNew[old];
        plan.remapsVertices = plan.remapsVertices || next != old;
        if (next < -1 || next >= request.vertices.size() || (next >= 0 && plan.remap.newToOld[next] >= 0))
            return fail(error, QStringLiteral("旧顶点映射越界或多个旧点映射到同一点：%1").arg(mesh->ids));
        if (next < 0) continue;
        const auto& vertex = plan.edit.vertices[next];
        if (vertex.indices.size() != 1 || vertex.indices.first() != old || vertex.weights.first() != 1)
            return fail(error, QStringLiteral("保留顶点必须对应其原始直接副本：%1[%2]").arg(mesh->ids).arg(old));
        plan.remap.newToOld[next] = old;
    }
    if (plan.geometry) {
        if (!resample(table.uvs->uvs, mesh->uv_sources_begin_indices, mesh->vertex_counts, plan, plan.uvs, error)) return false;
        QByteArray originalUvs; originalUvs.reserve(mesh->vertex_counts * 8);
        for (int i = 0; i < mesh->vertex_counts * 2; ++i) appendFloat(originalUvs, table.uvs->uvs[mesh->uv_sources_begin_indices + i]);
        plan.uvChanged = plan.uvs != originalUvs;
    }
    plan.topology = request.indices.size() != mesh->position_index_sources_counts;
    for (int i = 0; i < mesh->position_index_sources_counts; ++i) {
        const int value = table.position_indices->indices[mesh->position_index_sources_begin_indices + i];
        if (value < 0 || value >= mesh->vertex_counts) return fail(error, QStringLiteral("源网格三角索引越界：%1").arg(mesh->ids));
        if (i < request.indices.size() && request.indices[i] != value) plan.topology = true;
    }
    return true;
}

quint64 endpointKey(int glue, int pair, int side) { return (quint64(glue) * MaximumCount + pair) * 2 + side; }

bool checkAliasedGeometry(const CSectionTable& table, Plan& plan, bool uv, QString* error)
{
    QSet<int> changed;
    for (const auto& mesh : plan.meshes) if (uv ? mesh.uvChanged : mesh.topology) changed.insert(mesh.index);
    if (changed.isEmpty()) return true;
    struct Interval { qint64 first, end; int mesh; };
    QVector<Interval> intervals; intervals.reserve(table.art_meshes.size());
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto* mesh = table.art_meshes[index];
        const qint64 first = uv ? mesh->uv_sources_begin_indices : mesh->position_index_sources_begin_indices;
        const qint64 count = uv ? qint64(mesh->vertex_counts) * 2 : mesh->position_index_sources_counts;
        const qint64 size = uv ? table.uvs->uvs.size() : table.position_indices->indices.size();
        if (!count) continue;
        if (!range(first, count, size)) return fail(error, QStringLiteral("源 ArtMesh 的 UV 或三角索引切片越界：%1").arg(mesh->ids));
        intervals.append({first, first + count, index});
    }
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) {
        return a.first != b.first ? a.first < b.first : a.end != b.end ? a.end < b.end : a.mesh < b.mesh;
    });
    qint64 end = -1; int previous = -1;
    for (int i = 0; i < intervals.size(); ++i) {
        const auto& current = intervals[i];
        const int other = end > current.first ? previous :
            i + 1 < intervals.size() && intervals[i + 1].first < current.end ? intervals[i + 1].mesh : -1;
        if (changed.contains(current.mesh) && other >= 0)
            plan.impact.blockingReasons.append(QStringLiteral("ArtMesh %1 的%2存储与 %3 重叠；独立编辑会改变 Core 的原地转换，需要先规范化几何")
                .arg(table.art_meshes[current.mesh]->ids, uv ? QStringLiteral(" UV ") : QStringLiteral("三角索引"), table.art_meshes[other]->ids));
        if (current.end > end) { end = current.end; previous = current.mesh; }
    }
    return true;
}

bool makePlan(const CSectionTable& table, const Builder::Request& request, Plan& result, QString* error)
{
    if (request.gluePolicy != Builder::GlueEndpointPolicy::Reject && request.gluePolicy != Builder::GlueEndpointPolicy::DropPair &&
        request.gluePolicy != Builder::GlueEndpointPolicy::Retarget) return fail(error, QStringLiteral("Glue 删除策略无效"));
    if (request.meshes.size() > table.art_meshes.size() || request.glueRetargets.size() > MaximumCount ||
        (request.gluePolicy != Builder::GlueEndpointPolicy::Retarget && !request.glueRetargets.isEmpty()))
        return fail(error, QStringLiteral("网格修改或 Glue 重定向请求无效"));
    qint64 totalReferences = 0;
    for (const auto& mesh : request.meshes) {
        if (mesh.vertices.isEmpty() || mesh.vertices.size() > Builder::MaximumVertices)
            return fail(error, QStringLiteral("网格顶点数量无效：%1").arg(mesh.artMeshId));
        for (const auto& vertex : mesh.vertices) {
            if (vertex.indices.isEmpty() || vertex.indices.size() != vertex.weights.size())
                return fail(error, QStringLiteral("顶点插值来源数量无效：%1").arg(mesh.artMeshId));
            if (vertex.indices.size() > MaximumCount - totalReferences)
                return fail(error, QStringLiteral("网格请求的顶点引用总数超过 1000000"));
            totalReferences += vertex.indices.size();
        }
    }
    QHash<QString, int> meshes;
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        const auto& id = table.art_meshes[index]->ids;
        if (id.isEmpty() || meshes.contains(id)) return fail(error, QStringLiteral("ArtMesh ID 缺失或重复：%1").arg(id));
        meshes.insert(id, index);
    }
    QHash<int, int> edits;
    for (const auto& edit : request.meshes) {
        const int index = meshes.value(edit.artMeshId, -1);
        if (index < 0 || edits.contains(index)) return fail(error, QStringLiteral("修改目标不存在或重复：%1").arg(edit.artMeshId));
        MeshPlan plan;
        if (!checkMesh(table, index, edit, plan, error)) return false;
        edits.insert(index, result.meshes.size()); result.impact.meshes.append(plan.remap); result.meshes.append(std::move(plan));
    }
    if (!checkAliasedGeometry(table, result, true, error) || !checkAliasedGeometry(table, result, false, error)) return false;
    QMap<quint64, int> retargets;
    for (int index = 0; index < request.glueRetargets.size(); ++index) {
        const auto& redirect = request.glueRetargets[index];
        if (redirect.glueIndex < 0 || redirect.glueIndex >= table.glue.size() || redirect.pairIndex < 0 ||
            redirect.pairIndex >= MaximumCount || (redirect.side != Builder::GlueRetarget::A && redirect.side != Builder::GlueRetarget::B))
            return fail(error, QStringLiteral("Glue 重定向目标无效"));
        const quint64 key = endpointKey(redirect.glueIndex, redirect.pairIndex, redirect.side);
        if (retargets.contains(key)) return fail(error, QStringLiteral("Glue 端点重定向重复"));
        retargets.insert(key, index);
    }
    QSet<quint64> usedRetargets;
    qint64 glueRows = table.glue_info.size(), traversedGlueRows = 0;
    for (int index = 0; index < table.glue.size(); ++index) {
        const auto* glue = table.glue[index];
        const int a = glue->art_mesh_indices_a, b = glue->art_mesh_indices_b;
        if (a < 0 || b < 0 || a >= table.art_meshes.size() || b >= table.art_meshes.size() ||
            !range(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts, table.glue_info.size()) || glue->glue_info_sources_counts % 2)
            return fail(error, QStringLiteral("Glue 配对端点或范围无效：%1").arg(glue->ids));
        const bool changesA = edits.contains(a) && result.meshes[edits.value(a)].remapsVertices;
        const bool changesB = edits.contains(b) && result.meshes[edits.value(b)].remapsVertices;
        if (!changesA && !changesB) continue;
        if (glue->glue_info_sources_counts > MaximumCount - traversedGlueRows)
            return fail(error, QStringLiteral("Glue 配对检查的引用总数超过 1000000"));
        traversedGlueRows += glue->glue_info_sources_counts;
        GluePlan plan; plan.change = {index, glue->ids, glue->glue_info_sources_counts / 2, glue->glue_info_sources_counts / 2, {}, {}};
        bool changed = false;
        for (int pair = 0; pair < plan.change.oldPairCount; ++pair) {
            int next[2]; const CGlue_info* info[2]; bool removed = false;
            for (int side = 0; side < 2; ++side) {
                const int mesh = side ? b : a;
                info[side] = table.glue_info[glue->glue_info_sources_begin_indices + pair * 2 + side];
                const int old = info[side]->position_indices;
                if (old < 0 || old >= table.art_meshes[mesh]->vertex_counts || !std::isfinite(info[side]->weights))
                    return fail(error, QStringLiteral("Glue 顶点索引或权重无效：%1[%2]").arg(glue->ids).arg(pair));
                next[side] = edits.contains(mesh) ? result.meshes[edits.value(mesh)].remap.oldToNew[old] : old;
                if (next[side] >= 0) { changed = changed || next[side] != old; continue; }
                changed = true; removed = true;
                if (request.gluePolicy == Builder::GlueEndpointPolicy::Reject) {
                    result.impact.blockingReasons.append(QStringLiteral("删除命中 Glue %1 第 %2 对的 %3 端点，请选择解除配对或重定向")
                        .arg(glue->ids).arg(pair).arg(side ? QStringLiteral("B") : QStringLiteral("A")));
                } else if (request.gluePolicy == Builder::GlueEndpointPolicy::Retarget) {
                    const quint64 key = endpointKey(index, pair, side); const auto redirect = retargets.constFind(key);
                    if (redirect == retargets.cend()) {
                        result.impact.blockingReasons.append(QStringLiteral("Glue %1 第 %2 对缺少端点重定向").arg(glue->ids).arg(pair));
                    } else {
                        const auto& value = request.glueRetargets[*redirect];
                        if (value.newVertex < 0 || value.newVertex >= result.meshes[edits.value(mesh)].remap.newVertexCount)
                            return fail(error, QStringLiteral("Glue 替代顶点越界：%1[%2]").arg(glue->ids).arg(pair));
                        next[side] = value.newVertex; usedRetargets.insert(key); plan.change.retargetedEndpoints.append(value);
                    }
                }
            }
            if (removed && request.gluePolicy == Builder::GlueEndpointPolicy::DropPair) { plan.change.droppedPairs.append(pair); --plan.change.newPairCount; continue; }
            for (int side = 0; side < 2; ++side) { appendIndex(plan.indices, std::max(0, next[side])); appendFloat(plan.weights, info[side]->weights); }
        }
        if (changed) {
            const qint64 added = qint64(plan.change.newPairCount) * 2;
            if (added > MaximumCount - glueRows) return fail(error, QStringLiteral("重映射后的 Glue 条目超过 1000000"));
            glueRows += added;
            result.impact.glues.append(plan.change); result.glues.append(std::move(plan));
        }
    }
    if (usedRetargets.size() != retargets.size()) return fail(error, QStringLiteral("Glue 重定向没有对应待删除端点"));
    return true;
}

bool validateOutputBudgets(const CSectionTable& table, const Plan& plan, QString* error)
{
    qint64 keyforms = table.art_mesh_keyforms.size(), positions = table.keyform_positions->xys.size();
    qint64 uvs = table.uvs->uvs.size(), indices = table.position_indices->indices.size();
    qint64 bindings = table.blend_shape_keyform_bindings.size();
    const auto reserve = [&](qint64& size, qint64 added, const QString& label) {
        if (added < 0 || added > MaximumCount - size)
            return fail(error, QStringLiteral("网格结构的 %1 数量超过 1000000").arg(label));
        size += added; return true;
    };
    for (const auto& mesh : plan.meshes) {
        if (mesh.topology && !reserve(indices, mesh.edit.indices.size(), QStringLiteral("三角索引"))) return false;
        if (!mesh.geometry) continue;
        if (mesh.uvChanged && !reserve(uvs, qint64(mesh.edit.vertices.size()) * 2, QStringLiteral("UV"))) return false;
        const auto* source = table.art_meshes[mesh.index];
        QSet<int> begins;
        const auto addKeyforms = [&](int first, int count) {
            if (!range(first, count, table.art_mesh_keyforms.size())) return fail(error, QStringLiteral("网格普通或 BlendShape 关键形越界"));
            if (!reserve(keyforms, count, QStringLiteral("关键形"))) return false;
            for (int row = first; row < first + count; ++row) {
                const int begin = table.art_mesh_keyforms[row]->keyform_position_sources_begin_indices;
                if (!range(begin, qint64(source->vertex_counts) * 2, table.keyform_positions->xys.size()))
                    return fail(error, QStringLiteral("网格关键形位置切片越界"));
                begins.insert(begin);
            }
            return true;
        };
        if (!addKeyforms(source->keyform_sources_begin_indices, source->keyform_sources_counts)) return false;
        for (const auto* shape : table.blend_shapes_art_meshes) {
            if (shape->target_indices != mesh.index) continue;
            const int first = shape->blend_shape_keyform_binding_sources_begin_indices, count = shape->blend_shape_keyform_binding_sources_counts;
            if (!range(first, count, table.blend_shape_keyform_bindings.size())) return fail(error, QStringLiteral("ArtMesh BlendShape 绑定范围无效"));
            if (!reserve(bindings, count, QStringLiteral("BlendShape 绑定"))) return false;
            for (int row = first; row < first + count; ++row) {
                const auto* binding = table.blend_shape_keyform_bindings[row];
                if (!addKeyforms(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts)) return false;
            }
        }
        if (!reserve(positions, qint64(begins.size()) * mesh.edit.vertices.size() * 2, QStringLiteral("关键形位置"))) return false;
    }
    return true;
}

bool resample(const std::vector<float>& source, int first, int oldCount, const MeshPlan& mesh, QByteArray& result, QString* error)
{
    if (!range(first, qint64(oldCount) * 2, source.size())) return fail(error, QStringLiteral("网格位置或 UV 切片越界：%1").arg(mesh.edit.artMeshId));
    QByteArray candidate; candidate.reserve(mesh.edit.vertices.size() * 8);
    for (const auto& vertex : mesh.edit.vertices) {
        for (int axis = 0; axis < 2; ++axis) {
            if (vertex.indices.size() == 1 && vertex.weights.first() == 1) {
                const float value = source[first + vertex.indices.first() * 2 + axis];
                if (!std::isfinite(value)) return fail(error, QStringLiteral("源顶点包含非有限值：%1").arg(mesh.edit.artMeshId));
                appendFloat(candidate, value);
            } else {
                double value = 0, minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                for (int i = 0; i < vertex.indices.size(); ++i) {
                    const float original = source[first + vertex.indices[i] * 2 + axis];
                    if (!std::isfinite(original)) return fail(error, QStringLiteral("源顶点包含非有限值：%1").arg(mesh.edit.artMeshId));
                    minimum = std::min(minimum, double(original)); maximum = std::max(maximum, double(original));
                    value += double(original) * vertex.weights[i];
                }
                if (!std::isfinite(value))
                    return fail(error, QStringLiteral("新顶点超出有限浮点范围：%1").arg(mesh.edit.artMeshId));
                value = std::clamp(value, minimum, maximum);
                appendFloat(candidate, float(value));
            }
        }
    }
    result.swap(candidate); return true;
}

bool transform(const CSectionTable& table, const Plan& plan, Columns& columns, QString* error)
{
    for (const auto& mesh : plan.meshes) {
        const auto* source = table.art_meshes[mesh.index];
        if (mesh.geometry) {
            columns.set("art_meshes.vertex_counts", mesh.index, mesh.edit.vertices.size());
            if (mesh.uvChanged) columns.set("art_meshes.uv_sources_begin_indices", mesh.index, columns.appendPool("uvs.uvs", mesh.uvs));
            QHash<int, int> positions;
            const auto cloneRows = [&](int first, int count, int& newFirst) {
                if (!range(first, count, table.art_mesh_keyforms.size())) return fail(error, QStringLiteral("网格普通或 BlendShape 关键形越界"));
                newFirst = columns.appendRows("art_mesh_keyforms.opacities", first, count);
                for (int local = 0; local < count; ++local) {
                    const int begin = table.art_mesh_keyforms[first + local]->keyform_position_sources_begin_indices;
                    if (!positions.contains(begin)) {
                        QByteArray values;
                        if (!resample(table.keyform_positions->xys, begin, source->vertex_counts, mesh, values, error)) return false;
                        positions.insert(begin, columns.appendPool("keyform_positions.xys", values));
                    }
                    columns.set("art_mesh_keyforms.keyform_position_sources_begin_indices", newFirst + local, positions.value(begin));
                }
                return true;
            };
            int first;
            if (!cloneRows(source->keyform_sources_begin_indices, source->keyform_sources_counts, first)) return false;
            columns.set("art_meshes.keyform_sources_begin_indices", mesh.index, first);
            for (int shapeIndex = 0; shapeIndex < table.blend_shapes_art_meshes.size(); ++shapeIndex) {
                const auto* shape = table.blend_shapes_art_meshes[shapeIndex];
                if (shape->target_indices != mesh.index) continue;
                const int firstBinding = shape->blend_shape_keyform_binding_sources_begin_indices;
                const int bindingCount = shape->blend_shape_keyform_binding_sources_counts;
                if (!range(firstBinding, bindingCount, table.blend_shape_keyform_bindings.size()))
                    return fail(error, QStringLiteral("ArtMesh BlendShape 绑定范围无效"));
                const int newBinding = columns.appendRows("blend_shape_keyform_bindings.parameter_binding_sources_indices", firstBinding, bindingCount);
                columns.set("blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", shapeIndex, newBinding);
                for (int index = 0; index < bindingCount; ++index) {
                    const auto* binding = table.blend_shape_keyform_bindings[firstBinding + index];
                    if (!cloneRows(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, first)) return false;
                    columns.set("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", newBinding + index, first);
                }
            }
        }
        if (mesh.topology) {
            QByteArray values; values.reserve(mesh.edit.indices.size() * 2);
            for (quint16 index : mesh.edit.indices) appendIndex(values, index);
            columns.set("art_meshes.position_index_sources_begin_indices", mesh.index, columns.appendPool("position_indices.indices", values));
            columns.set("art_meshes.position_index_sources_counts", mesh.index, mesh.edit.indices.size());
        }
    }
    for (const auto& glue : plan.glues) {
        int begin; columns.appendGlue(glue.indices, glue.weights, begin);
        columns.set("glue.glue_info_sources_begin_indices", glue.change.glueIndex, begin);
        columns.set("glue.glue_info_sources_counts", glue.change.glueIndex, glue.change.newPairCount * 2);
    }
    return true;
}

template<class Operation> bool execute(const QByteArray& base, QString* error, Operation&& operation)
{
    if (error) error->clear();
    try {
        auto reader = CReader::FromBytes(base); CSectionTable table(reader);
        if (!table.IsValid() || base.size() < 8 || quint8(base[5]) != 0)
            return fail(error, QStringLiteral("网格结构需要有效的小端 MOC：%1").arg(reader.Error()));
        return operation(table);
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备网格结构，原模型保持不变")); }
}
}

bool CMocMeshStructureBuilder::Analyze(const QByteArray& base, const Request& request, Impact& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table) {
        Plan plan; if (!makePlan(table, request, plan, error) || !validateOutputBudgets(table, plan, error)) return false;
        result = std::move(plan.impact); return true;
    });
}

bool CMocMeshStructureBuilder::Build(const QByteArray& base, const Request& request, Result& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table) {
        Plan plan; if (!makePlan(table, request, plan, error) || !validateOutputBudgets(table, plan, error)) return false;
        if (!plan.impact.blockingReasons.isEmpty()) return fail(error, plan.impact.blockingReasons.join(QLatin1Char('\n')));
        Columns columns(base, table);
        if (!transform(table, plan, columns, error)) return false;
        Result candidate;
        if (!CMocParameterStructureBuilder::BuildColumnState(base, columns.state, candidate.bytes, error)) return false;
        candidate.state = std::move(columns.state);
        candidate.meshes = std::move(plan.impact.meshes); candidate.glues = std::move(plan.impact.glues);
        result = std::move(candidate); return true;
    });
}
}
