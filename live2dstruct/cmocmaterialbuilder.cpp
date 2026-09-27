#include "cmocmaterialbuilder.h"
#include "cmocparameterstructurebuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace live2dstruct {
namespace {
constexpr int MaximumPool = 1000000;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool same(float first, float second) { return std::memcmp(&first, &second, 4) == 0; }
bool sameUvs(const QVector<float>& first, const QVector<float>& second)
{
    return first.size() == second.size() && (first.isEmpty() || std::memcmp(first.constData(), second.constData(), size_t(first.size()) * 4) == 0);
}
void appendFloat(QByteArray& bytes, float value)
{
    quint32 raw; std::memcpy(&raw, &value, 4); char encoded[4]; qToLittleEndian(raw, encoded); bytes.append(encoded, 4);
}
struct Layout
{
    struct Mesh { QString id; int begin, count, texture, component; };
    QVector<Mesh> meshes;
    QHash<QString, int> indices;
    QVector<float> effective, previous;
    mutable QMap<QPair<int, int>, QVector<float>> uvCache;
    int components = 0;

    bool read(const CSectionTable& table, QString* error)
    {
        if (!table.IsValid() || !table.count_info || !table.uvs || table.uvs->uvs.size() > MaximumPool ||
            table.count_info->art_meshes < 0 || table.count_info->art_meshes > table.art_meshes.size() || table.count_info->art_meshes > 100000)
            return fail(error, QStringLiteral("材质源模型或数量无效"));
        effective = QVector<float>(table.uvs->uvs.begin(), table.uvs->uvs.end()); previous = effective;
        QVector<int> invalid(effective.size() + 1, 0);
        for (qsizetype index = 0; index < effective.size(); ++index) invalid[index + 1] = invalid[index] + !std::isfinite(effective[index]);
        QVector<int> differences[2]{QVector<int>((effective.size() + 1) / 2 + 1, 0), QVector<int>((effective.size() + 1) / 2 + 1, 0)};
        QSet<QPair<int, int>> slices;
        qint64 total = 0;
        for (int index = 0; index < table.count_info->art_meshes; ++index) {
            const auto* mesh = table.art_meshes[index];
            if (!mesh) return fail(error, QStringLiteral("材质源对象为空"));
            const qint64 count = qint64(mesh->vertex_counts) * 2;
            if (mesh->ids.isEmpty() || indices.contains(mesh->ids) || count < 0 || count > MaximumPool || (count && (mesh->uv_sources_begin_indices < 0 ||
                mesh->uv_sources_begin_indices > effective.size() || count > effective.size() - mesh->uv_sources_begin_indices)))
                return fail(error, QStringLiteral("材质源对象标识或 UV 范围无效：%1").arg(mesh->ids));
            const QPair<int, int> slice{mesh->uv_sources_begin_indices, int(count)};
            if (!slices.contains(slice)) {
                if (count > CModelMaterialEdit::MaximumValues - total) return fail(error, QStringLiteral("不同 UV 切片的检查总量超过限制"));
                total += count; slices.insert(slice);
            }
            indices.insert(mesh->ids, meshes.size());
            meshes.append({mesh->ids, mesh->uv_sources_begin_indices, int(count), mesh->texture_nos, -1});
            if (count) {
                const int begin = mesh->uv_sources_begin_indices;
                if (invalid[begin + count] != invalid[begin]) return fail(error, QStringLiteral("源 UV 包含非有限数值：%1").arg(mesh->ids));
                const int first = begin + 1;
                ++differences[first % 2][first / 2]; --differences[first % 2][first / 2 + count / 2];
            }
        }
        for (int parity = 0; parity < 2; ++parity) {
            int count = 0;
            for (int at = parity; at < effective.size(); at += 2) {
                count += differences[parity][at / 2];
                if (!count) continue;
                const float first = 1.0f - effective[at], second = 1.0f - first;
                if (!std::isfinite(first) || !same(1.0f - second, first))
                    return fail(error, QStringLiteral("Core UV 重复转换无法精确归一化"));
                previous[at] = count == 1 ? effective[at] : count % 2 ? second : first;
                effective[at] = count % 2 ? first : second;
            }
        }
        QVector<int> order;
        for (int index = 0; index < meshes.size(); ++index) if (meshes[index].count) order.append(index);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return meshes[a].begin < meshes[b].begin; });
        qint64 end = -1;
        for (int index : order) {
            auto& mesh = meshes[index];
            if (mesh.begin >= end) ++components;
            mesh.component = components - 1; end = std::max(end, qint64(mesh.begin) + mesh.count);
        }
        return true;
    }
    CModelDrawableMaterial material(int index) const
    {
        const auto& mesh = meshes[index]; const QPair<int, int> slice{mesh.begin, mesh.count};
        auto cached = uvCache.constFind(slice);
        if (cached == uvCache.cend()) {
            QVector<float> values; values.reserve(mesh.count);
            for (int value = 0; value < mesh.count; ++value) values.append(value % 2 ? previous[mesh.begin + value] : effective[mesh.begin + value]);
            cached = uvCache.insert(slice, std::move(values));
        }
        return {mesh.id, mesh.texture, *cached};
    }
    bool effectiveEqual(const Layout& other, int index, int next) const
    {
        const auto& first = meshes[index]; const auto& second = other.meshes[next];
        if (first.count != second.count) return false;
        for (int value = 0; value < first.count; ++value)
            if (!same(effective[first.begin + value], other.effective[second.begin + value])) return false;
        return true;
    }
};
bool isolatedColumns(const QByteArray& bytes, const CSectionTable& table, const QSet<int>& changed, QString* error)
{
    auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
    QSet<int> known;
    QMap<qint64, int> offsets;
    const int header = table.sectionOffsetTable->count_info->count_info_offset;
    for (int slot = CSectionValidation::OffsetTableStart; slot < header; slot += 4) {
        const qint64 offset = qFromLittleEndian<quint32>(bytes.constData() + slot);
        if (offset) offsets.insert(offset, slot);
    }
    for (auto& span : spans) { span.width = CSectionValidation::StorageWidth(span); known.insert(span.slot); }
    const auto overlaps = [](qint64 first, qint64 size, qint64 second, qint64 length) {
        return size > 0 && length > 0 && first < second + length && second < first + size;
    };
    for (const auto& column : spans) {
        if (!changed.contains(column.slot) || !column.offset || !column.count) continue;
        const qint64 size = column.count * column.width;
        if (overlaps(column.offset, size, header, CSectionValidation::CountInfoSize(table.sectionOffsetTable->mocVersion)) ||
            overlaps(column.offset, size, table.sectionOffsetTable->canvas_info->CanvasOffset, 20))
            return fail(error, QStringLiteral("材质区段与模型元数据共享物理存储"));
        for (const auto& other : spans) {
            if (other.slot == column.slot || !other.offset || !other.count) continue;
            if (overlaps(column.offset, size, other.offset, other.count * other.width))
                return fail(error, QStringLiteral("材质区段 %1 与 %2 共享物理存储，无法独立编辑")
                            .arg(QString::fromLatin1(column.name), QString::fromLatin1(other.name)));
        }
        for (int slot = CSectionValidation::OffsetTableStart; slot < header; slot += 4) {
            if (known.contains(slot) || slot < CSectionValidation::OffsetTableStart + 8) continue;
            const qint64 offset = qFromLittleEndian<quint32>(bytes.constData() + slot);
            if (!offset) continue;
            const auto next = offsets.upperBound(offset);
            const qint64 length = (next == offsets.cend() ? bytes.size() : next.key()) - offset;
            if (overlaps(column.offset, size, offset, length))
                return fail(error, QStringLiteral("材质区段与未解释区段 0x%1 共享物理存储").arg(slot, 0, 16));
        }
    }
    return true;
}
bool capture(const CSectionTable& table, CModelMaterialOverrides& result, QString* error)
{
    Layout layout;
    if (!layout.read(table, error)) return false;
    CModelMaterialOverrides candidate;
    for (int index = 0; index < layout.meshes.size(); ++index) candidate.insert(layout.meshes[index].id, layout.material(index));
    result = std::move(candidate); if (error) error->clear(); return true;
}
}
bool CMocMaterialBuilder::Capture(const CSectionTable& table, CModelMaterialOverrides& result, QString* error) try
{
    return capture(table, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取模型材质")); }
bool CMocMaterialBuilder::Capture(const QByteArray& base, CModelMaterialOverrides& result, QString* error) try
{
    auto reader = CReader::FromBytes(base); CSectionTable table(reader);
    return capture(table, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取模型材质")); }
bool CMocMaterialBuilder::Capture(const QByteArray& base, const QString& id, CModelDrawableMaterial& result, QString* error) try
{
    CModelMaterialOverrides materials;
    if (!Capture(base, materials, error)) return false;
    const auto found = materials.constFind(id);
    if (found == materials.cend()) return fail(error, QStringLiteral("材质对象不存在：%1").arg(id));
    result = *found; return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取对象材质")); }
bool CMocMaterialBuilder::Build(const QByteArray& base, const CModelMaterialOverrides& overrides, QByteArray& result, QString* error) try
{
    if (error) error->clear();
    if (overrides.isEmpty()) { result = base; return true; }
    CModelMaterialState values;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        if (it.key() != it->id) return fail(error, QStringLiteral("材质覆盖对象标识不一致"));
        values.append(*it);
    }
    if (!CModelMaterialEdit::Validate(values, error)) return false;
    auto reader = CReader::FromBytes(base); CSectionTable table(reader); Layout layout;
    if (!layout.read(table, error)) return false;
    QSet<int> changedComponents;
    QSet<QString> changed;
    CMocParameterStructureBuilder::ColumnState state;
    const auto columns = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
    const auto column = [&](int slot) {
        for (const auto& span : columns) if (span.slot == slot) return base.mid(span.offset, span.count * span.width);
        return QByteArray{};
    };
    auto textureColumn = column(0xe4), starts = column(0xf0), uvs = column(0x178);
    for (const auto& value : values) {
        const auto found = layout.indices.constFind(value.id);
        if (found == layout.indices.cend() || value.uvs.size() != layout.meshes[*found].count)
            return fail(error, QStringLiteral("材质对象不存在或 UV 数量不符：%1").arg(value.id));
        const auto before = layout.material(*found);
        if (value == before) continue;
        changed.insert(value.id);
        if (value.textureIndex != before.textureIndex) qToLittleEndian(qint32(value.textureIndex), textureColumn.data() + *found * 4);
        if (!sameUvs(value.uvs, before.uvs)) changedComponents.insert(layout.meshes[*found].component);
    }
    if (changed.isEmpty()) { result = base; return true; }
    if (textureColumn != column(0xe4)) state.columns.insert(0xe4, textureColumn);
    for (int index = 0; index < layout.meshes.size(); ++index) {
        const auto& mesh = layout.meshes[index];
        if (!changedComponents.contains(mesh.component)) continue;
        if (mesh.count > MaximumPool - uvs.size() / 4) return fail(error, QStringLiteral("隔离共享 UV 后数量超出范围"));
        auto material = overrides.value(mesh.id, layout.material(index));
        qToLittleEndian(qint32(uvs.size() / 4), starts.data() + index * 4);
        for (float value : material.uvs) appendFloat(uvs, value);
    }
    if (!changedComponents.isEmpty()) {
        state.columns.insert(0xf0, starts); state.columns.insert(0x178, uvs); state.counts.insert(15, uvs.size() / 4);
    }
    QByteArray candidate;
    if (!CMocParameterStructureBuilder::BuildColumnState(base, state, candidate, error) ||
        !ValidateTransition(base, candidate, changed, error)) return false;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法构建材质修改，原结果保持不变")); }
bool CMocMaterialBuilder::ValidateTransition(const QByteArray& before, const QByteArray& after,
                                             const QSet<QString>& allowedIds, QString* error) try
{
    if (allowedIds.size() > 100000) return fail(error, QStringLiteral("材质修改缺少对象范围"));
    CMocParameterStructureBuilder::ColumnState changes;
    if (!CMocParameterStructureBuilder::CaptureColumnState(before, after, changes, error)) return false;
    for (auto it = changes.columns.cbegin(); it != changes.columns.cend(); ++it)
        if (it.key() != 0xe4 && it.key() != 0xf0 && it.key() != 0x178)
            return fail(error, QStringLiteral("材质修改改变了其他模型区段：0x%1").arg(it.key(), 0, 16));
    for (auto it = changes.counts.cbegin(); it != changes.counts.cend(); ++it)
        if (it.key() != 15) return fail(error, QStringLiteral("材质修改改变了其他模型数量"));
    auto previousReader = CReader::FromBytes(before), nextReader = CReader::FromBytes(after);
    CSectionTable previousTable(previousReader), nextTable(nextReader); Layout previous, next;
    if (!previous.read(previousTable, error) || !next.read(nextTable, error)) return false;
    QSet<int> changedColumns;
    for (auto it = changes.columns.cbegin(); it != changes.columns.cend(); ++it) changedColumns.insert(it.key());
    if (!isolatedColumns(before, previousTable, changedColumns, error) || !isolatedColumns(after, nextTable, changedColumns, error)) return false;
    if (previous.meshes.size() != next.meshes.size()) return fail(error, QStringLiteral("材质修改改变了对象目录"));
    for (const auto& id : allowedIds) if (!previous.indices.contains(id)) return fail(error, QStringLiteral("材质修改对象不存在：%1").arg(id));
    for (int index = 0; index < previous.meshes.size(); ++index) {
        const auto& first = previous.meshes[index]; const auto& second = next.meshes[index];
        if (first.id != second.id || first.count != second.count ||
            (second.texture < 0 && (second.count != 0 || first.texture != second.texture)))
            return fail(error, QStringLiteral("材质修改改变了对象标识、顶点数量或无效纹理槽"));
        if (!allowedIds.contains(first.id) && (first.texture != second.texture || !previous.effectiveEqual(next, index, index)))
            return fail(error, QStringLiteral("材质修改影响了未选对象：%1").arg(first.id));
    }
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证材质模型转换")); }
}
