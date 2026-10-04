#include "cmocmeshstructurebuilder.h"
#include "cmocgeometrypool.h"
#include "cmeshtopology.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include "cartmeshgeometrylayout.h"
#include "cparentgraph.h"
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

using AllocationFailure = detail::CMocGeometryAllocationFailure;
using PoolSpace = detail::CMocGeometryPool;

struct Storage
{
    PoolSpace uvs, indices, positions, keyforms, bindings, glue;
    qint64 references = 0, work = 0;
    void reference(qint64 count = 1)
    {
        if (count < 0 || count > MaximumCount - references)
            throw AllocationFailure { QStringLiteral("网格结构存活引用检查总数超过 1000000") };
        references += count;
    }
};

class Columns
{
public:
    Columns(const QByteArray& source, const CSectionTable& table) : bytes(source)
    {
        const auto schema = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        for (auto column : schema) {
            column.width = CSectionValidation::StorageWidth(column);
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
        return originalData(name);
    }
    QByteArray originalData(const QByteArray& name) const
    {
        const auto& column = span(name);
        return column.offset ? bytes.mid(column.offset, column.count * column.width) : QByteArray();
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
    void setString(const QByteArray& name, int row, const QString& value)
    {
        const auto& column = span(name);
        const QByteArray encoded = value.toUtf8();
        if (column.width <= 0 || encoded.size() >= column.width || row < 0 || row >= counts.value(column.countIndex))
            throw std::length_error("MOC string field is invalid");
        auto& next = writable(name);
        std::memset(next.data() + qint64(row) * column.width, 0, column.width);
        std::memcpy(next.data() + qint64(row) * column.width, encoded.constData(), encoded.size());
    }
    void write(const QByteArray& name, int first, const QByteArray& payload, int count)
    {
        const auto& column = span(name);
        if (count < counts.value(column.countIndex) || count > MaximumCount || payload.size() % column.width
            || !range(first, payload.size() / column.width, count))
            throw std::length_error("MOC reusable pool range mismatch");
        auto& next = writable(name);
        const qint64 required = qint64(count) * column.width;
        if (next.size() < required)
            next.append(QByteArray(required - next.size(), '\0'));
        if (next.size() != required)
            throw std::length_error("MOC reusable pool count mismatch");
        if (!payload.isEmpty())
            std::memcpy(next.data() + qint64(first) * column.width, payload.constData(), payload.size());
        counts[column.countIndex] = count;
        state.counts[column.countIndex] = count;
    }
    int copyRows(const QByteArray& name, int first, int count, PoolSpace& space, qint64& work)
    {
        const auto group = name.left(name.indexOf('.'));
        const auto& primary = span(name);
        if (!range(first, count, primary.count))
            throw std::length_error("MOC mesh original keyform range mismatch");
        const int destination = space.allocate(count, work);
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            const auto& column = *it;
            if (column.countIndex != primary.countIndex)
                continue;
            if (column.optional && !column.offset)
                continue;
            if (!it.key().startsWith(group + '.'))
                throw std::length_error("MOC mesh row group mismatch");
            const auto original = originalData(it.key());
            if (original.size() != qint64(primary.count) * column.width)
                throw std::length_error("MOC mesh original row group length mismatch");
            const auto payload = original.mid(qint64(first) * column.width, qint64(count) * column.width);
            write(it.key(), destination, payload, space.size());
        }
        return destination;
    }
    int writePool(const QByteArray& name, const QByteArray& payload, PoolSpace& space, qint64& work)
    {
        const auto& column = span(name);
        if (payload.size() % column.width)
            throw std::length_error("MOC mesh reusable pool payload mismatch");
        const int first = space.allocate(payload.size() / column.width, work);
        write(name, first, payload, space.size());
        return first;
    }
    void writeGlue(const QByteArray& indices, const QByteArray& weights, Storage& storage, int& begin)
    {
        if (indices.size() % 2 || weights.size() != indices.size() * 2)
            throw std::length_error("Glue pair columns mismatch");
        begin = storage.glue.allocate(indices.size() / 2, storage.work);
        write("glue_info.position_indices", begin, indices, storage.glue.size());
        write("glue_info.weights", begin, weights, storage.glue.size());
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
    QStringList conflicts;
    if (!CArtMeshGeometryLayout::CheckIsolation(table, changed, uv ? CArtMeshGeometryLayout::Pool::Uvs :
                                                CArtMeshGeometryLayout::Pool::Indices, conflicts, error)) return false;
    plan.impact.blockingReasons.append(conflicts);
    return true;
}

bool makePlan(const CSectionTable& table, const Builder::Request& request, Plan& result, QString* error)
{
    if (request.gluePolicy != Builder::GlueEndpointPolicy::Reject && request.gluePolicy != Builder::GlueEndpointPolicy::DropPair &&
        request.gluePolicy != Builder::GlueEndpointPolicy::Retarget) return fail(error, QStringLiteral("Glue 删除策略无效"));
    if (request.meshes.size() > table.art_meshes.size() || request.glueRetargets.size() > MaximumCount ||
        request.glueWeights.size() > MaximumCount ||
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
    QMap<quint64, int> weightEdits;
    QSet<int> weightGlueIndices;
    for (int index = 0; index < request.glueWeights.size(); ++index) {
        const auto& edit = request.glueWeights[index];
        if (edit.glueIndex < 0 || edit.glueIndex >= table.glue.size() || edit.pairIndex < 0 ||
            (edit.side != Builder::GlueRetarget::A && edit.side != Builder::GlueRetarget::B) ||
            !std::isfinite(edit.weight) || edit.weight < 0.0f || edit.weight > 1.0f)
            return fail(error, QStringLiteral("Glue 端点权重编辑无效"));
        const quint64 key = endpointKey(edit.glueIndex, edit.pairIndex, edit.side);
        if (weightEdits.contains(key)) return fail(error, QStringLiteral("Glue 端点权重编辑重复"));
        weightEdits.insert(key, index);
        weightGlueIndices.insert(edit.glueIndex);
    }
    QSet<quint64> usedWeightEdits;
    qint64 traversedGlueRows = 0;
    for (int index = 0; index < table.glue.size(); ++index) {
        const auto* glue = table.glue[index];
        const int a = glue->art_mesh_indices_a, b = glue->art_mesh_indices_b;
        if (a < 0 || b < 0 || a >= table.art_meshes.size() || b >= table.art_meshes.size() ||
            !range(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts, table.glue_info.size()) || glue->glue_info_sources_counts % 2)
            return fail(error, QStringLiteral("Glue 配对端点或范围无效：%1").arg(glue->ids));
        const bool changesA = edits.contains(a) && result.meshes[edits.value(a)].remapsVertices;
        const bool changesB = edits.contains(b) && result.meshes[edits.value(b)].remapsVertices;
        const bool hasWeightEdits = weightGlueIndices.contains(index);
        if (!changesA && !changesB && !hasWeightEdits) continue;
        if (glue->glue_info_sources_counts > MaximumCount - traversedGlueRows)
            return fail(error, QStringLiteral("Glue 配对检查的引用总数超过 1000000"));
        traversedGlueRows += glue->glue_info_sources_counts;
        GluePlan plan; plan.change = {index, glue->ids, glue->glue_info_sources_counts / 2, glue->glue_info_sources_counts / 2, {}, {}, {}};
        bool changed = false;
        for (int pair = 0; pair < plan.change.oldPairCount; ++pair) {
            int next[2]; const CGlue_info* info[2]; bool removed = false;
            float nextWeight[2] = {0.0f, 0.0f};
            for (int side = 0; side < 2; ++side) {
                const int mesh = side ? b : a;
                info[side] = table.glue_info[glue->glue_info_sources_begin_indices + pair * 2 + side];
                const int old = info[side]->position_indices;
                if (old < 0 || old >= table.art_meshes[mesh]->vertex_counts || !std::isfinite(info[side]->weights))
                    return fail(error, QStringLiteral("Glue 顶点索引或权重无效：%1[%2]").arg(glue->ids).arg(pair));
                nextWeight[side] = info[side]->weights;
                const quint64 weightKey = endpointKey(index, pair, side);
                const auto weight = weightEdits.constFind(weightKey);
                if (weight != weightEdits.cend()) {
                    nextWeight[side] = request.glueWeights[*weight].weight;
                    usedWeightEdits.insert(weightKey);
                    plan.change.weightEdits.append(request.glueWeights[*weight]);
                    changed = changed || nextWeight[side] != info[side]->weights;
                }
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
            for (int side = 0; side < 2; ++side) { appendIndex(plan.indices, std::max(0, next[side])); appendFloat(plan.weights, nextWeight[side]); }
        }
        if (changed) {
            result.impact.glues.append(plan.change); result.glues.append(std::move(plan));
        }
    }
    if (usedRetargets.size() != retargets.size()) return fail(error, QStringLiteral("Glue 重定向没有对应待删除端点"));
    if (usedWeightEdits.size() != weightEdits.size()) return fail(error, QStringLiteral("Glue 端点权重编辑没有对应存活端点"));
    return true;
}

bool prepareStorage(const CSectionTable& table, const Plan& plan, Storage& storage, QString* error)
{
    storage.uvs = PoolSpace(table.uvs->uvs.size(), QStringLiteral("UV"));
    storage.indices = PoolSpace(table.position_indices->indices.size(), QStringLiteral("三角索引"));
    storage.positions = PoolSpace(table.keyform_positions->xys.size(), QStringLiteral("关键形位置"));
    storage.keyforms = PoolSpace(table.art_mesh_keyforms.size(), QStringLiteral("ArtMesh 关键形"));
    storage.bindings = PoolSpace(table.blend_shape_keyform_bindings.size(), QStringLiteral("BlendShape 绑定"));
    storage.glue = PoolSpace(table.glue_info.size(), QStringLiteral("Glue 信息"));
    QSet<int> geometry, changedUvs, changedIndices, changedGlues;
    for (const auto& mesh : plan.meshes) {
        if (mesh.geometry)
            geometry.insert(mesh.index);
        if (mesh.uvChanged)
            changedUvs.insert(mesh.index);
        if (mesh.topology)
            changedIndices.insert(mesh.index);
    }
    for (const auto& glue : plan.glues)
        changedGlues.insert(glue.change.glueIndex);
    const auto validReference = [](int first, int count, qint64 size) {
        return count == 0 ? first >= -1 && first <= size : range(first, count, size);
    };
    QVector<int> warpVertices;
    for (const auto* warp : table.warp_deformers) {
        const qint64 count
            = warp->rows >= 0 && warp->columns >= 0 ? (qint64(warp->rows) + 1) * (qint64(warp->columns) + 1) : 0;
        if (count <= 0 || count > MaximumCount || count != warp->vertex_counts)
            return fail(error, QStringLiteral("Warp 顶点数与网格行列定义不一致，不能回收关键形位置"));
        warpVertices.append(int(count));
    }
    const auto artForms = [&](int first, int count, int vertices, bool released) {
        storage.reference(count);
        if (!validReference(first, count, table.art_mesh_keyforms.size()) || vertices < 0)
            return fail(error, QStringLiteral("ArtMesh 存活关键形引用范围无效"));
        if (released)
            return true;
        storage.keyforms.protect(first, count);
        for (int row = first; row < first + count; ++row)
            storage.positions.protect(
                table.art_mesh_keyforms[row]->keyform_position_sources_begin_indices, qint64(vertices) * 2);
        return true;
    };
    const auto warpForms = [&](int first, int count, int vertices) {
        storage.reference(count);
        if (!validReference(first, count, table.warp_deformer_keyforms.size()) || vertices < 0)
            return fail(error, QStringLiteral("Warp 存活关键形引用范围无效"));
        for (int row = first; row < first + count; ++row)
            storage.positions.protect(
                table.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices, qint64(vertices) * 2);
        return true;
    };
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        storage.reference();
        const auto* mesh = table.art_meshes[index];
        if (!changedUvs.contains(index))
            storage.uvs.protect(mesh->uv_sources_begin_indices, qint64(mesh->vertex_counts) * 2);
        if (!changedIndices.contains(index))
            storage.indices.protect(mesh->position_index_sources_begin_indices, mesh->position_index_sources_counts);
        if (!artForms(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts, mesh->vertex_counts,
                geometry.contains(index)))
            return false;
    }
    for (const auto* warp : table.warp_deformers) {
        storage.reference();
        if (!warpForms(warp->keyform_sources_begin_indices, warp->keyform_sources_counts, warp->vertex_counts))
            return false;
    }
    enum class ShapeOwner { ArtMesh, Warp, Part, Rotation };
    const auto shapes = [&](const auto& owners, ShapeOwner kind, int targetCount) {
        for (const auto* shape : owners) {
            storage.reference();
            const int target = shape->target_indices;
            const int first = shape->blend_shape_keyform_binding_sources_begin_indices;
            const int count = shape->blend_shape_keyform_binding_sources_counts;
            if (target < 0 || target >= targetCount
                || !validReference(first, count, table.blend_shape_keyform_bindings.size()))
                return fail(error, QStringLiteral("BlendShape 存活目标或绑定引用范围无效"));
            const bool released = kind == ShapeOwner::ArtMesh && geometry.contains(target);
            if (!released)
                storage.bindings.protect(first, count);
            storage.reference(count);
            for (int row = first; row < first + count; ++row) {
                const auto* binding = table.blend_shape_keyform_bindings[row];
                if (kind == ShapeOwner::ArtMesh
                    && !artForms(binding->keyform_sources_blend_shape_indices,
                        binding->keyform_sources_blend_shape_counts, table.art_meshes[target]->vertex_counts, released))
                    return false;
                if (kind == ShapeOwner::Warp
                    && !warpForms(binding->keyform_sources_blend_shape_indices,
                        binding->keyform_sources_blend_shape_counts, warpVertices[target]))
                    return false;
            }
        }
        return true;
    };
    if (!shapes(table.blend_shapes_art_meshes, ShapeOwner::ArtMesh, table.art_meshes.size())
        || !shapes(table.blend_shapes_warp_deformers, ShapeOwner::Warp, table.warp_deformers.size())
        || !shapes(table.blend_shapes_parts, ShapeOwner::Part, table.parts.size())
        || !shapes(table.blend_shapes_rotation_deformers, ShapeOwner::Rotation, table.rotation_deformers.size()))
        return false;
    for (int index = 0; index < table.glue.size(); ++index) {
        storage.reference();
        const auto* glue = table.glue[index];
        if (!changedGlues.contains(index))
            storage.glue.protect(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts);
    }
    for (auto* pool :
        { &storage.uvs, &storage.indices, &storage.positions, &storage.keyforms, &storage.bindings, &storage.glue })
        pool->prepare();
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
    if (plan.glues.isEmpty() && std::none_of(plan.meshes.cbegin(), plan.meshes.cend(), [](const MeshPlan& mesh) {
            return mesh.geometry || mesh.topology;
        }))
        return true;
    Storage storage;
    if (!prepareStorage(table, plan, storage, error))
        return false;
    for (const auto& mesh : plan.meshes) {
        const auto* source = table.art_meshes[mesh.index];
        if (mesh.geometry) {
            columns.set("art_meshes.vertex_counts", mesh.index, mesh.edit.vertices.size());
            if (mesh.uvChanged)
                columns.set("art_meshes.uv_sources_begin_indices", mesh.index,
                    columns.writePool("uvs.uvs", mesh.uvs, storage.uvs, storage.work));
            QHash<int, int> positions;
            const auto cloneRows = [&](int first, int count, int& newFirst) {
                if (!range(first, count, table.art_mesh_keyforms.size())) return fail(error, QStringLiteral("网格普通或 BlendShape 关键形越界"));
                newFirst
                    = columns.copyRows("art_mesh_keyforms.opacities", first, count, storage.keyforms, storage.work);
                for (int local = 0; local < count; ++local) {
                    const int begin = table.art_mesh_keyforms[first + local]->keyform_position_sources_begin_indices;
                    if (!positions.contains(begin)) {
                        QByteArray values;
                        if (!resample(table.keyform_positions->xys, begin, source->vertex_counts, mesh, values, error)) return false;
                        positions.insert(
                            begin, columns.writePool("keyform_positions.xys", values, storage.positions, storage.work));
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
                const int newBinding
                    = columns.copyRows("blend_shape_keyform_bindings.parameter_binding_sources_indices", firstBinding,
                        bindingCount, storage.bindings, storage.work);
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
            columns.set("art_meshes.position_index_sources_begin_indices", mesh.index,
                columns.writePool("position_indices.indices", values, storage.indices, storage.work));
            columns.set("art_meshes.position_index_sources_counts", mesh.index, mesh.edit.indices.size());
        }
    }
    for (const auto& glue : plan.glues) {
        int begin;
        columns.writeGlue(glue.indices, glue.weights, storage, begin);
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
    } catch (const AllocationFailure& rejected) {
        return fail(error, rejected.message);
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备网格结构，原模型保持不变"));
    }
}
}

bool CMocMeshStructureBuilder::Analyze(const QByteArray& base, const Request& request, Impact& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table) {
        Plan plan;
        if (!makePlan(table, request, plan, error))
            return false;
        Columns columns(base, table);
        if (!transform(table, plan, columns, error))
            return false;
        result = std::move(plan.impact);
        return true;
    });
}

bool CMocMeshStructureBuilder::Build(const QByteArray& base, const Request& request, Result& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table) {
        Plan plan;
        if (!makePlan(table, request, plan, error))
            return false;
        if (!plan.impact.blockingReasons.isEmpty())
            return fail(error, plan.impact.blockingReasons.join(QLatin1Char('\n')));
        Columns columns(base, table);
        if (!transform(table, plan, columns, error))
            return false;
        Result candidate;
        if (!CMocParameterStructureBuilder::BuildColumnState(base, columns.state, candidate.bytes, error))
            return false;
        candidate.state = std::move(columns.state);
        candidate.meshes = std::move(plan.impact.meshes);
        candidate.glues = std::move(plan.impact.glues);
        result = std::move(candidate);
        return true;
    });
}

bool CMocMeshStructureBuilder::Clone(const QByteArray& base, const CloneRequest& request,
                                     Result& result, QString* error)
{
    return execute(base, error, [&](const CSectionTable& table) {
        if (request.sourceArtMeshId.isEmpty() || request.newArtMeshId.isEmpty() ||
            request.sourceArtMeshId == request.newArtMeshId || request.newArtMeshId.contains(QChar::Null))
            return fail(error, QStringLiteral("ArtMesh 克隆身份无效"));
        const auto validId = [](const QString& id) {
            const QByteArray encoded = id.toUtf8();
            return !id.contains(QChar::Null) && encoded.size() < 64 && QString::fromUtf8(encoded) == id;
        };
        if (!validId(request.sourceArtMeshId) || !validId(request.newArtMeshId))
            return fail(error, QStringLiteral("ArtMesh ID 必须是有效 UTF-8 且不超过 63 字节"));
        int sourceIndex = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            if (table.art_meshes[index]->ids == request.newArtMeshId)
                return fail(error, QStringLiteral("ArtMesh ID 已存在：%1").arg(request.newArtMeshId));
            if (table.art_meshes[index]->ids == request.sourceArtMeshId) {
                if (sourceIndex >= 0)
                    return fail(error, QStringLiteral("克隆源 ArtMesh ID 不唯一：%1").arg(request.sourceArtMeshId));
                sourceIndex = index;
            }
        }
        if (sourceIndex < 0) return fail(error, QStringLiteral("克隆源 ArtMesh 不存在：%1").arg(request.sourceArtMeshId));
        const auto* source = table.art_meshes[sourceIndex];
        if (source->vertex_counts < 3 || source->vertex_counts > MaximumVertices)
            return fail(error, QStringLiteral("克隆源 ArtMesh 顶点数无效：%1").arg(request.sourceArtMeshId));
        if (!range(source->uv_sources_begin_indices, qint64(source->vertex_counts) * 2, table.uvs->uvs.size()) ||
            !range(source->position_index_sources_begin_indices, source->position_index_sources_counts,
                   table.position_indices->indices.size()) ||
            !range(source->keyform_sources_begin_indices, source->keyform_sources_counts,
                   table.art_mesh_keyforms.size()) ||
            !range(source->drawable_mask_sources_begin_indices, source->drawable_mask_sources_counts,
                   table.drawable_masks.size()))
            return fail(error, QStringLiteral("克隆源 ArtMesh 几何或蒙版范围无效"));
        for (int index = source->position_index_sources_begin_indices;
             index < source->position_index_sources_begin_indices + source->position_index_sources_counts; ++index) {
            const int vertex = table.position_indices->indices[index];
            if (vertex < 0 || vertex >= source->vertex_counts)
                return fail(error, QStringLiteral("克隆源 ArtMesh 含越界三角索引"));
        }
        for (int local = 0; local < source->keyform_sources_counts; ++local) {
            const auto* form = table.art_mesh_keyforms[source->keyform_sources_begin_indices + local];
            if (!range(form->keyform_position_sources_begin_indices, qint64(source->vertex_counts) * 2,
                       table.keyform_positions->xys.size()))
                return fail(error, QStringLiteral("克隆源 ArtMesh 关键形位置范围无效"));
        }
        for (int local = 0; local < source->drawable_mask_sources_counts; ++local) {
            const int mask = table.drawable_masks[source->drawable_mask_sources_begin_indices + local]->art_mesh_sources_indices;
            if (mask < 0 || mask >= table.art_meshes.size())
                return fail(error, QStringLiteral("克隆源 ArtMesh 蒙版引用无效"));
        }
        for (const auto* shape : table.blend_shapes_art_meshes)
            if (shape->target_indices == sourceIndex)
                return fail(error, QStringLiteral("含 BlendShape 的 ArtMesh 暂不能直接克隆：%1").arg(request.sourceArtMeshId));
        for (const auto* glue : table.glue)
            if (glue->art_mesh_indices_a == sourceIndex || glue->art_mesh_indices_b == sourceIndex)
                return fail(error, QStringLiteral("参与 Glue 的 ArtMesh 暂不能直接克隆：%1").arg(request.sourceArtMeshId));
        if (request.parentPartIndex < -2 || request.parentPartIndex >= table.parts.size() ||
            request.parentDeformerIndex < -2 || request.parentDeformerIndex >= table.deformers.size() || request.textureIndex < -2)
            return fail(error, QStringLiteral("克隆 ArtMesh 父级或图集索引无效"));
        int sourceGroup = -1, sourceObject = -1;
        for (int group = 0; group < table.draw_order_groups.size(); ++group) {
            const auto* definition = table.draw_order_groups[group];
            if (!range(definition->object_sources_begin_indices, definition->object_sources_counts,
                       table.draw_order_group_objects.size()))
                return fail(error, QStringLiteral("源 ArtMesh 绘制组范围无效"));
            for (int row = definition->object_sources_begin_indices;
                 row < definition->object_sources_begin_indices + definition->object_sources_counts; ++row) {
                const auto* object = table.draw_order_group_objects[row];
                if (object->types == 0 && object->indices == sourceIndex) {
                    if (sourceGroup >= 0)
                        return fail(error, QStringLiteral("源 ArtMesh 绘制组引用不唯一"));
                    sourceGroup = group;
                    sourceObject = row;
                }
            }
        }
        if (sourceGroup < 0)
            return fail(error, QStringLiteral("源 ArtMesh 缺少绘制顺序组"));
        Columns columns(base, table); Storage storage;
        detail::CMocGeometryPool meshRows(table.art_meshes.size(), QStringLiteral("ArtMesh 行"));
        detail::CMocGeometryPool maskRows(table.drawable_masks.size(), QStringLiteral("蒙版行"));
        detail::CMocGeometryPool objectRows(table.draw_order_group_objects.size(), QStringLiteral("绘制组对象行"));
        const auto prepare = [](detail::CMocGeometryPool& pool, int count) { pool.protect(0, count); pool.prepare(); };
        prepare(meshRows, table.art_meshes.size());
        prepare(maskRows, table.drawable_masks.size());
        prepare(objectRows, table.draw_order_group_objects.size());
        storage.uvs = PoolSpace(table.uvs->uvs.size(), QStringLiteral("UV")); prepare(storage.uvs, table.uvs->uvs.size());
        storage.indices = PoolSpace(table.position_indices->indices.size(), QStringLiteral("三角索引")); prepare(storage.indices, table.position_indices->indices.size());
        storage.positions = PoolSpace(table.keyform_positions->xys.size(), QStringLiteral("关键形位置")); prepare(storage.positions, table.keyform_positions->xys.size());
        storage.keyforms = PoolSpace(table.art_mesh_keyforms.size(), QStringLiteral("ArtMesh 关键形")); prepare(storage.keyforms, table.art_mesh_keyforms.size());
        qint64 work = 0;
        const int meshIndex = columns.copyRows("art_meshes.ids", sourceIndex, 1, meshRows, work);
        columns.setString("art_meshes.ids", meshIndex, request.newArtMeshId);
        columns.set("art_meshes.uv_sources_begin_indices", meshIndex,
                    columns.copyRows("uvs.uvs", source->uv_sources_begin_indices, source->vertex_counts * 2, storage.uvs, work));
        columns.set("art_meshes.position_index_sources_begin_indices", meshIndex,
                    columns.copyRows("position_indices.indices", source->position_index_sources_begin_indices,
                                     source->position_index_sources_counts, storage.indices, work));
        const int formBegin = columns.copyRows("art_mesh_keyforms.opacities", source->keyform_sources_begin_indices,
                                               source->keyform_sources_counts, storage.keyforms, work);
        columns.set("art_meshes.keyform_sources_begin_indices", meshIndex, formBegin);
        for (int local = 0; local < source->keyform_sources_counts; ++local) {
            const auto* form = table.art_mesh_keyforms[source->keyform_sources_begin_indices + local];
            columns.set("art_mesh_keyforms.keyform_position_sources_begin_indices", formBegin + local,
                        columns.copyRows("keyform_positions.xys", form->keyform_position_sources_begin_indices,
                                         source->vertex_counts * 2, storage.positions, work));
        }
        const int maskBegin = columns.copyRows("drawable_masks.art_mesh_sources_indices",
                                               source->drawable_mask_sources_begin_indices,
                                               source->drawable_mask_sources_counts, maskRows, work);
        columns.set("art_meshes.drawable_mask_sources_begin_indices", meshIndex, maskBegin);
        columns.set("art_meshes.drawable_mask_sources_counts", meshIndex, source->drawable_mask_sources_counts);
        const auto* sourceDefinition = table.draw_order_groups[sourceGroup];
        QVector<int> parents(table.draw_order_groups.size(), -1);
        for (int group = 0; group < table.draw_order_groups.size(); ++group) {
            const auto* definition = table.draw_order_groups[group];
            if (!range(definition->object_sources_begin_indices, definition->object_sources_counts,
                       table.draw_order_group_objects.size()) ||
                definition->object_sources_total_counts < definition->object_sources_counts ||
                definition->minimum_draw_orders > definition->maximum_draw_orders)
                return fail(error, QStringLiteral("绘制组对象范围或顺序范围无效"));
            for (int row = definition->object_sources_begin_indices;
                 row < definition->object_sources_begin_indices + definition->object_sources_counts; ++row) {
                const auto* object = table.draw_order_group_objects[row];
                if (object->types == 0) {
                    if (!range(object->indices, 1, table.art_meshes.size()))
                        return fail(error, QStringLiteral("绘制组 ArtMesh 引用无效"));
                } else if (object->types == 1) {
                    if (!range(object->indices, 1, table.parts.size()) ||
                        !range(object->self_indices, 1, table.draw_order_groups.size()) ||
                        (parents[object->self_indices] >= 0 && parents[object->self_indices] != group))
                        return fail(error, QStringLiteral("绘制组父级引用无效或重复"));
                    parents[object->self_indices] = group;
                } else {
                    return fail(error, QStringLiteral("绘制组对象类型无效"));
                }
            }
        }
        if (!CParentGraph(parents).IsValid()) return fail(error, QStringLiteral("绘制组层级存在循环"));
        const int originalCount = sourceDefinition->object_sources_counts;
        const int sourceObjectBegin = sourceDefinition->object_sources_begin_indices;
        const int copiedBegin = columns.copyRows("draw_order_group_objects.types", sourceObjectBegin,
                                                  originalCount, objectRows, work);
        const int objectIndex = columns.copyRows("draw_order_group_objects.types", sourceObject,
                                                  1, objectRows, work);
        columns.set("draw_order_groups.object_sources_begin_indices", sourceGroup, copiedBegin);
        columns.set("draw_order_groups.object_sources_counts", sourceGroup, originalCount + 1);
        columns.set("draw_order_groups.object_sources_total_counts", sourceGroup,
                    sourceDefinition->object_sources_total_counts + 1);
        columns.set("draw_order_group_objects.types", objectIndex, 0);
        columns.set("draw_order_group_objects.indices", objectIndex, meshIndex);
        columns.set("draw_order_group_objects.self_indices", objectIndex,
                    table.draw_order_group_objects[sourceObject]->self_indices);
        for (int ancestor = parents[sourceGroup]; ancestor >= 0; ancestor = parents[ancestor])
            columns.set("draw_order_groups.object_sources_total_counts", ancestor,
                        table.draw_order_groups[ancestor]->object_sources_total_counts + 1);
        if (request.parentPartIndex != -2) columns.set("art_meshes.parent_part_indices", meshIndex, request.parentPartIndex);
        if (request.parentDeformerIndex != -2) columns.set("art_meshes.parent_deformer_indices", meshIndex, request.parentDeformerIndex);
        if (request.textureIndex != -2) columns.set("art_meshes.texture_nos", meshIndex, request.textureIndex);
        Result candidate;
        if (!CMocParameterStructureBuilder::BuildColumnState(base, columns.state, candidate.bytes, error)) return false;
        candidate.state = std::move(columns.state);
        result = std::move(candidate);
        return true;
    });
}

}
