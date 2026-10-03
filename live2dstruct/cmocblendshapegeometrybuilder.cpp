#include "cmocblendshapegeometrybuilder.h"
#include "cblendshaperelations.h"
#include "cmocparameterstructurebuilder.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Builder = CMocBlendShapeGeometryBuilder;
using Span = CSectionValidation::ColumnSpan;
constexpr int MaximumRows = 1000000;
constexpr int MaximumBytes = 256 * 1024 * 1024;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool range(qint64 begin, qint64 count, qint64 size)
{ return begin >= 0 && count >= 0 && begin <= size && count <= size - begin; }
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
bool same(const QVector<float>& a, const QVector<float>& b)
{ return a.size() == b.size() && (a.isEmpty() || std::memcmp(a.constData(), b.constData(), size_t(a.size()) * sizeof(float)) == 0); }
struct Selection {
    int mesh = -1, binding = -1, row = -1;
    int bindingsBegin = 0, bindingsCount = 0, formsBegin = 0, formsCount = 0;
};
bool capture(const CSectionTable& table, const Builder::Key& key, Selection& selection, QVector<float>& positions, QString* error)
{
    if (!table.IsValid() || key.meshId.isEmpty() || key.meshId.size() > 1024 || key.meshId.contains(QChar::Null) ||
        !range(key.shapeIndex, 1, table.blend_shapes_art_meshes.size()))
        return fail(error, QStringLiteral("融合形来源、目标或形状索引无效"));
    for (int index = 0; index < table.art_meshes.size(); ++index) if (table.art_meshes[index]->ids == key.meshId) {
        if (selection.mesh >= 0) return fail(error, QStringLiteral("融合目标网格标识重复"));
        selection.mesh = index;
    }
    if (selection.mesh < 0) return fail(error, QStringLiteral("融合目标网格不存在"));
    const auto* shape = table.blend_shapes_art_meshes[key.shapeIndex];
    if (shape->target_indices != selection.mesh) return fail(error, QStringLiteral("融合形不属于指定网格"));
    selection.bindingsBegin = shape->blend_shape_keyform_binding_sources_begin_indices;
    selection.bindingsCount = shape->blend_shape_keyform_binding_sources_counts;
    if (!range(selection.bindingsBegin, selection.bindingsCount, table.blend_shape_keyform_bindings.size()) ||
        !range(key.bindingOffset, 1, selection.bindingsCount))
        return fail(error, QStringLiteral("融合关键形绑定区间无效"));
    selection.binding = selection.bindingsBegin + key.bindingOffset;
    CBlendShapeRelations relations;
    if (!relations.Build(table, error)) return false;
    if (!relations.BindingsForTarget(CBlendShapeRelations::ArtMesh, key.meshId).contains(selection.binding))
        return fail(error, QStringLiteral("融合绑定没有唯一有效的参数来源"));
    const auto* binding = table.blend_shape_keyform_bindings[selection.binding];
    selection.formsBegin = binding->keyform_sources_blend_shape_indices;
    selection.formsCount = binding->keyform_sources_blend_shape_counts;
    if (!range(selection.formsBegin, selection.formsCount, table.art_mesh_keyforms.size()) ||
        !range(key.keyformOffset, 1, selection.formsCount))
        return fail(error, QStringLiteral("融合关键形行区间无效"));
    selection.row = selection.formsBegin + key.keyformOffset;
    const int vertices = table.art_meshes[selection.mesh]->vertex_counts;
    const int begin = table.art_mesh_keyforms[selection.row]->keyform_position_sources_begin_indices;
    if (vertices <= 0 || vertices > 65536 || !range(begin, qint64(vertices) * 2, table.keyform_positions->xys.size()))
        return fail(error, QStringLiteral("融合关键形位置切片无效"));
    positions.reserve(vertices * 2);
    for (int at = 0; at < vertices * 2; ++at) {
        const float value = table.keyform_positions->xys[begin + at];
        if (!std::isfinite(value)) return fail(error, QStringLiteral("融合关键形包含非有限坐标"));
        positions.append(value);
    }
    return true;
}
class Columns {
public:
    CMocParameterStructureBuilder::ColumnState state;
    QMap<QByteArray, Span> spans;
    const QByteArray& bytes;
    explicit Columns(const QByteArray& source, const CSectionTable& table) : bytes(source)
    {
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            spans.insert(span.name, span);
    }
    Span span(const QByteArray& name) const
    {
        const auto found = spans.constFind(name);
        if (found == spans.cend() || found->slot < 0 || found->countIndex < 0)
            throw std::runtime_error("missing schema");
        return *found;
    }
    QByteArray original(const Span& s) const
    {
        const qint64 size = s.count * CSectionValidation::StorageWidth(s);
        if (s.optional && !s.offset) return {};
        if (!range(s.offset, size, bytes.size())) throw std::runtime_error("column bounds");
        return bytes.mid(s.offset, size);
    }
    QByteArray& edit(const QByteArray& name)
    {
        const auto s = span(name);
        auto found = state.columns.find(s.slot);
        if (found == state.columns.end()) found = state.columns.insert(s.slot, original(s));
        return found.value();
    }
    void word(const QByteArray& name, int row, int value)
    {
        const auto s = span(name); auto& data = edit(name);
        if (CSectionValidation::StorageWidth(s) != 4 || !range(qint64(row) * 4, 4, data.size()))
            throw std::runtime_error("cell bounds");
        qToLittleEndian(qint32(value), data.data() + row * 4);
    }
    bool canCopy(const QByteArray& prefix, int begin, int count, qint64* extraBytes = nullptr) const
    {
        int expectedCount = -1;
        int expectedCountIndex = -1;
        qint64 bytes = 0;
        bool found = false;
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            if (!it.key().startsWith(prefix + '.')) continue;
            const auto s = span(it.key());
            if (expectedCount < 0) {
                expectedCount = int(s.count);
                expectedCountIndex = s.countIndex;
            }
            if (expectedCount != s.count || expectedCountIndex != s.countIndex ||
                !range(begin, count, s.count) || count > MaximumRows - s.count)
                return false;
            bytes += qint64(count) * CSectionValidation::StorageWidth(s);
            if (bytes > MaximumBytes) return false;
            found = true;
        }
        if (extraBytes) *extraBytes = bytes;
        return found;
    }
    int copy(const QByteArray& prefix, int begin, int count)
    {
        qint64 ignored = 0;
        if (!canCopy(prefix, begin, count, &ignored)) throw std::runtime_error("row budget");
        int first = -1;
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) if (it.key().startsWith(prefix + '.')) {
            const auto s = span(it.key());
            if (first < 0) first = int(s.count);
            state.counts[s.countIndex] = int(s.count) + count;
            if (s.optional && !s.offset) continue;
            const int width = int(CSectionValidation::StorageWidth(s));
            auto data = original(s);
            data.append(data.mid(begin * width, count * width));
            state.columns[s.slot] = std::move(data);
        }
        if (first < 0) throw std::runtime_error("missing group");
        return first;
    }
    int appendPositions(const QVector<float>& positions)
    {
        const auto s = span("keyform_positions.xys");
        const qint64 width = CSectionValidation::StorageWidth(s);
        if (width != 4 || positions.size() > MaximumRows - s.count ||
            qint64(positions.size()) * width > MaximumBytes ||
            qint64(s.count) * width + qint64(positions.size()) * width > MaximumBytes)
            throw std::runtime_error("position budget");
        auto& data = edit("keyform_positions.xys"); const auto oldSize = data.size();
        data.resize(oldSize + positions.size() * 4);
        for (int i = 0; i < positions.size(); ++i) {
            quint32 bits; std::memcpy(&bits, &positions[i], sizeof(bits));
            qToLittleEndian(bits, data.data() + oldSize + i * 4);
        }
        state.counts[s.countIndex] = int(s.count) + positions.size(); return int(s.count);
    }
};
}
bool CMocBlendShapeGeometryBuilder::Capture(const QByteArray& bytes, const Key& key, Frame& result, QString* error) try
{
    if (error) error->clear();
    if (bytes.isEmpty() || bytes.size() > MaximumBytes) return fail(error, QStringLiteral("融合编辑模型超过字节预算或为空"));
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    Selection selection; Frame candidate;
    if (!capture(table, key, selection, candidate.deltas, error)) return false;
    candidate.key = key; candidate.sourceDigest = digest(bytes); candidate.sourceKeyformIndex = selection.row;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("融合几何读取失败，原输出保持不变")); }

bool CMocBlendShapeGeometryBuilder::Build(const QByteArray& bytes, const Request& request, Result& result, QString* error) try
{
    if (error) error->clear();
    Frame current;
    if (!Capture(bytes, request.before.key, current, error)) return false;
    if (current.sourceDigest != request.before.sourceDigest || current.sourceKeyformIndex != request.before.sourceKeyformIndex ||
        !same(current.deltas, request.before.deltas)) return fail(error, QStringLiteral("融合几何来源已改变"));
    if (request.deltas.size() != current.deltas.size()) return fail(error, QStringLiteral("融合几何顶点数量不能改变"));
    for (float value : request.deltas) if (!std::isfinite(value)) return fail(error, QStringLiteral("融合几何新坐标不是有限数值"));
    if (same(current.deltas, request.deltas)) return fail(error, QStringLiteral("融合几何没有变化"));
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); Selection selected; QVector<float> deltas;
    if (!capture(table, current.key, selected, deltas, error)) return false;
    Columns columns(bytes, table);
    qint64 bindingBytes = 0, formBytes = 0;
    if (!columns.canCopy("blend_shape_keyform_bindings", selected.bindingsBegin, selected.bindingsCount, &bindingBytes) ||
        !columns.canCopy("art_mesh_keyforms", selected.formsBegin, selected.formsCount, &formBytes))
        return fail(error, QStringLiteral("融合关键形复制范围或列宽不一致"));
    const auto positionSpan = columns.span("keyform_positions.xys");
    const qint64 positionBytes = qint64(request.deltas.size()) * CSectionValidation::StorageWidth(positionSpan);
    if (positionBytes < 0 || bytes.size() > MaximumBytes - bindingBytes - formBytes - positionBytes)
        return fail(error, QStringLiteral("融合几何结果超过字节预算"));
    const int bindings = columns.copy("blend_shape_keyform_bindings", selected.bindingsBegin, selected.bindingsCount);
    const int forms = columns.copy("art_mesh_keyforms", selected.formsBegin, selected.formsCount);
    const int position = columns.appendPositions(request.deltas);
    columns.word("blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", current.key.shapeIndex, bindings);
    columns.word("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", bindings + current.key.bindingOffset, forms);
    columns.word("art_mesh_keyforms.keyform_position_sources_begin_indices", forms + current.key.keyformOffset, position);
    Result candidate;
    if (!CMocParameterStructureBuilder::BuildColumnState(bytes, columns.state, candidate.bytes, error) || candidate.bytes.size() > MaximumBytes)
        return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("融合几何结果超过预算"));
    auto outputReader = CReader::FromBytes(candidate.bytes); CSectionTable output(outputReader);
    if (!output.IsValid()) return fail(error, QStringLiteral("融合几何结果结构校验失败"));
    Columns written(candidate.bytes, output);
    for (auto it = columns.spans.cbegin(); it != columns.spans.cend(); ++it) {
        const auto expected = columns.state.columns.value(it->slot, columns.original(*it));
        if (!written.spans.contains(it.key())) return fail(error, QStringLiteral("融合几何结果缺少列：%1").arg(QString::fromLatin1(it.key())));
        const auto actual = written.spans.value(it.key());
        if (expected != written.original(actual) || actual.count != columns.state.counts.value(it->countIndex, int(it->count)))
            return fail(error, QStringLiteral("融合几何写回改变了未请求的列：%1").arg(QString::fromLatin1(it.key())));
    }
    if (!Capture(candidate.bytes, current.key, candidate.frame, error) || !same(candidate.frame.deltas, request.deltas))
        return fail(error, QStringLiteral("融合几何写回内容不一致"));
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("融合几何写回失败，原输出保持不变")); }
}
