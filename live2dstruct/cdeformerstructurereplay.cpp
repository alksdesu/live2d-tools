#include "cdeformerstructurereplay.h"
#include "ckeyformbindinggrid.h"
#include "csectiontable.h"
#include "csectionvalidation.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace live2dstruct {
namespace {
using Raw = CMocDeformerStructureBuilder;
using Codec = CMocParameterStructureBuilder;
using Owner = Raw::Owner;
using Impact = CDeformerStructureReplay::Impact;
constexpr int MaximumCount = 1000000;
constexpr double Pi = 3.14159265358979323846;

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}
bool canceled(CExportControl* control, QString* error)
{
    if (!control || !control->IsCancellationRequested())
        return false;
    control->Checkpoint();
    fail(error, control->Error().isEmpty() ? QStringLiteral("变形器结构操作已取消") : control->Error());
    return true;
}
bool resizeSnapshot(const QByteArray& base, const CDeformerStructureRequest& request, QString* error)
{
    if (request.sourceDigest.size() != 32 || request.expectedAuthoringDigest.size() != 32
        || QCryptographicHash::hash(base, QCryptographicHash::Sha256) != request.expectedAuthoringDigest)
        return fail(error, QStringLiteral("Warp 网格请求与当前作者模型快照不一致"));
    return true;
}
bool range(qint64 begin, qint64 count, qint64 size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}
QByteArray integer(int value) { QByteArray bytes(4, Qt::Uninitialized); qToLittleEndian(qint32(value), bytes.data()); return bytes; }
QByteArray floating(float value) { quint32 bits; std::memcpy(&bits, &value, 4); QByteArray bytes(4, Qt::Uninitialized); qToLittleEndian(bits, bytes.data()); return bytes; }
float checkedFloat(double value)
{
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) throw std::range_error("Nonfinite transformed geometry");
    return float(value);
}

struct Affine
{
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
    bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && x == 0 && y == 0; }
    std::array<double, 2> point(double px, double py, bool delta = false) const
    { return {a * px + b * py + (delta ? 0 : x), c * px + d * py + (delta ? 0 : y)}; }
    Affine then(const Affine& outer) const
    {
        const auto origin = outer.point(x, y);
        return {outer.a * a + outer.b * c, outer.a * b + outer.b * d,
                outer.c * a + outer.d * c, outer.c * b + outer.d * d, origin[0], origin[1]};
    }
    bool inverse(Affine& result) const
    {
        const double determinant = a * d - b * c;
        if (!std::isfinite(determinant) || determinant == 0) return false;
        Affine value{d / determinant, -b / determinant, -c / determinant, a / determinant, 0, 0};
        const auto origin = value.point(-x, -y); value.x = origin[0]; value.y = origin[1];
        for (double field : {value.a, value.b, value.c, value.d, value.x, value.y}) if (!std::isfinite(field)) return false;
        result = value; return true;
    }
};
Affine rotation(double angle, double scale, bool reflectX, bool reflectY, double x, double y)
{
    const double radians = std::remainder(angle, 360.0) * Pi / 180;
    const double sx = scale * (reflectX ? -1 : 1), sy = scale * (reflectY ? -1 : 1);
    return {std::cos(radians) * sx, -std::sin(radians) * sy, std::sin(radians) * sx, std::cos(radians) * sy, x, y};
}
double directionAngle(const Affine& value) { return std::atan2(-value.d, -value.b) * 180 / Pi + 90; }

class Columns
{
public:
    Columns(const QByteArray& base, const CSectionTable& table) : bytes(base)
    {
        for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            span.width = CSectionValidation::StorageWidth(span);
            spans.insert(QByteArray(span.name), span); counts.insert(span.countIndex, int(span.count));
        }
    }
    const CSectionValidation::ColumnSpan& span(const QByteArray& name) const
    { const auto found = spans.constFind(name); if (found == spans.cend()) throw std::range_error("Unknown column"); return *found; }
    bool present(const QByteArray& name) const { const auto found = spans.constFind(name); return found != spans.cend() && found->offset; }
    int count(const QByteArray& name) const { return counts.value(span(name).countIndex); }
    QByteArray original(const QByteArray& name) const
    {
        const auto& column = span(name);
        if (!column.offset) return {};
        if (!range(column.offset, column.count * column.width, bytes.size())) throw std::range_error("Invalid source column");
        return bytes.mid(column.offset, column.count * column.width);
    }
    QByteArray data(const QByteArray& name) const
    { const auto found = state.columns.constFind(span(name).slot); return found == state.columns.cend() ? original(name) : *found; }
    int sourceInt(const QByteArray& name, int row) const
    {
        const auto& column = span(name);
        if (column.width != 4 || !column.offset || !range(row, 1, column.count)) throw std::range_error("Invalid source cell");
        return qFromLittleEndian<qint32>(bytes.constData() + column.offset + qint64(row) * 4);
    }
    QByteArray& edit(const QByteArray& name)
    {
        const int slot = span(name).slot; auto found = state.columns.find(slot);
        if (found == state.columns.end()) found = state.columns.insert(slot, original(name));
        return found.value();
    }
    void set(const QByteArray& name, int row, const QByteArray& cell)
    {
        const auto& column = span(name); auto& payload = edit(name);
        if (cell.size() != column.width || !range(row, 1, payload.size() / column.width)) throw std::range_error("Invalid destination cell");
        std::memcpy(payload.data() + qint64(row) * column.width, cell.constData(), column.width);
    }
    int clone(const QByteArray& group, int begin, int length)
    {
        int countIndex = -1, first = -1;
        for (auto it = spans.cbegin(); it != spans.cend(); ++it) {
            if (!it.key().startsWith(group + '.')) continue;
            const auto& column = *it; countIndex = column.countIndex; first = counts.value(countIndex);
            if (!range(begin, length, column.count) || length < 1 || length > MaximumCount - first)
                throw std::range_error("Keyform clone exceeds row budget");
            if (!column.offset && column.optional) continue;
            auto& payload = edit(it.key());
            if (payload.size() != qint64(first) * column.width || !range(column.offset, column.count * column.width, bytes.size())) throw std::range_error("Incomplete row group");
            payload.append(bytes.constData() + column.offset + qint64(begin) * column.width, qint64(length) * column.width);
        }
        if (countIndex < 0) throw std::range_error("Unknown row group");
        state.counts[countIndex] = counts[countIndex] = first + length;
        return first;
    }
    int appendPositions(const QVector<float>& values)
    {
        const auto& column = span("keyform_positions.xys"); const int first = counts.value(column.countIndex);
        if (values.size() > MaximumCount - first) throw std::range_error("Position pool exceeds budget");
        auto& payload = edit("keyform_positions.xys");
        for (float value : values) payload.append(floating(value));
        state.counts[column.countIndex] = counts[column.countIndex] = first + values.size();
        return first;
    }
    Codec::ColumnState state;
private:
    const QByteArray& bytes;
    QMap<QByteArray, CSectionValidation::ColumnSpan> spans;
    QMap<int, int> counts;
};

QByteArray group(Owner::Type type) { return type == Owner::ArtMesh ? "art_meshes" : type == Owner::Warp ? "warp_deformers" : "rotation_deformers"; }
QByteArray formGroup(Owner::Type type) { return type == Owner::ArtMesh ? "art_mesh_keyforms" : type == Owner::Warp ? "warp_deformer_keyforms" : "rotation_deformer_keyforms"; }
QByteArray blendGroup(Owner::Type type) { return type == Owner::ArtMesh ? "blend_shapes_art_meshes" : type == Owner::Warp ? "blend_shapes_warp_deformers" : "blend_shapes_rotation_deformers"; }
struct Shape { int row, begin, count; };
struct Catalog
{
    QHash<QString, int> deformers, meshes;
    std::array<QHash<int, QVector<Shape>>, 3> shapes;
    QVector<QVector<int>> children;
    explicit Catalog(const CSectionTable& table)
    {
        children.resize(table.deformers.size());
        for (int i = 0; i < table.deformers.size(); ++i) {
            deformers.insert(table.deformers[i]->ids, i); const int parent = table.deformers[i]->parent_deformer_indices;
            if (parent >= 0 && parent < children.size()) children[parent].append(i);
        }
        for (int i = 0; i < table.art_meshes.size(); ++i) meshes.insert(table.art_meshes[i]->ids, i);
        const auto add = [&](Owner::Type type, const auto& sources) {
            for (int i = 0; i < sources.size(); ++i) { const auto* item = sources[i]; shapes[int(type)][item->target_indices].append({i, item->blend_shape_keyform_binding_sources_begin_indices, item->blend_shape_keyform_binding_sources_counts}); }
        };
        add(Owner::ArtMesh, table.blend_shapes_art_meshes); add(Owner::Warp, table.blend_shapes_warp_deformers); add(Owner::Rotation, table.blend_shapes_rotation_deformers);
    }
    int index(const Owner& owner) const { return owner.type == Owner::ArtMesh ? meshes.value(owner.id, -1) : deformers.value(owner.id, -1); }
    QVector<Shape> blends(Owner::Type type, int specific) const { return shapes[int(type)].value(specific); }
};
Owner owner(const CDeformers& node) { return {node.types == 0 ? Owner::Warp : Owner::Rotation, node.ids}; }

struct Migration
{
    Owner owner;
    Affine coordinates;
    double angle = 0, scale = 1;
    bool changes() const { return !coordinates.identity() || angle != 0 || scale != 1; }
};
struct Plan { Impact impact; QVector<Migration> migrations; };

class Geometry
{
public:
    Geometry(const CSectionTable& source, const Catalog& ids, const Columns& raw) : table(source), catalog(ids), columns(raw)
    {
        bindings.Build(table);
        for (const auto* parameter : table.parameters) parameters.insert(parameter->ids, parameter);
    }
    bool block(const QString& text) { reason = text; return false; }
    bool coverage(const Owner& target)
    {
        const int index = catalog.index(target);
        const int binding = target.type == Owner::ArtMesh ? table.art_meshes[index]->keyform_binding_sources_indices : table.deformers[index]->keyform_binding_sources_indices;
        const auto type = target.type == Owner::ArtMesh ? CKeyformBindingGrid::ArtMesh : target.type == Owner::Warp ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
        const int specific = target.type == Owner::ArtMesh ? index : table.deformers[index]->specific_sources_indices;
        const int formCount = columns.sourceInt(group(target.type) + ".keyform_sources_counts", specific);
        const QPair<int, int> key{binding, formCount};
        if (checkedBindings.contains(key)) return true;
        const auto description = bindings.Describe(type, target.id);
        if (!description.axesValid || description.expectedKeyformCount != description.keyformCount || description.keyformCount < 1)
            return block(QStringLiteral("%1 的普通关键形绑定结构无效").arg(target.id));
        for (const auto& axis : description.axes) {
            const auto parameter = parameters.constFind(axis.parameterId);
            if (axis.parameterType != 0 || axis.keys.isEmpty() || parameter == parameters.cend() ||
                (*parameter)->min_values < axis.keys.first() || (*parameter)->max_values > axis.keys.last())
                return block(QStringLiteral("%1 的参数 %2 未由普通轴覆盖完整范围，无法证明所有姿态保形").arg(target.id, axis.parameterId));
        }
        checkedBindings.insert(key);
        return true;
    }
    bool coverage(int index) { return coverage(owner(*table.deformers[index])); }
    bool positions(int row, int vertices, QVector<float>& out) const
    {
        if (!table.keyform_positions || !range(row, 1, table.warp_deformer_keyforms.size())) return false;
        const int begin = table.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices;
        if (vertices < 1 || !range(begin, qint64(vertices) * 2, table.keyform_positions->xys.size())) return false;
        out = QVector<float>(table.keyform_positions->xys.cbegin() + begin, table.keyform_positions->xys.cbegin() + begin + vertices * 2);
        return std::all_of(out.cbegin(), out.cend(), [](float value) { return std::isfinite(value); });
    }
    static bool affineGrid(const QVector<float>& p, int rows, int cols, Affine& result)
    {
        if (rows < 1 || cols < 1 || qint64(p.size()) != (qint64(rows) + 1) * (qint64(cols) + 1) * 2) return false;
        Affine map{double(p[cols * 2]) - p[0], double(p[rows * (cols + 1) * 2]) - p[0],
                   double(p[cols * 2 + 1]) - p[1], double(p[rows * (cols + 1) * 2 + 1]) - p[1], p[0], p[1]};
        for (int y = 0; y <= rows; ++y) for (int x = 0; x <= cols; ++x) {
            const auto expected = map.point(double(x) / cols, double(y) / rows); const int at = (y * (cols + 1) + x) * 2;
            if (!std::isfinite(expected[0]) || !std::isfinite(expected[1]) || float(expected[0]) != p[at] || float(expected[1]) != p[at + 1]) return false;
        }
        result = map; return true;
    }
    bool blendPositionsZero(int specific)
    {
        const auto* warp = table.warp_deformers[specific];
        for (const auto& shape : catalog.blends(Owner::Warp, specific)) {
            if (!range(shape.begin, shape.count, table.blend_shape_keyform_bindings.size())) return block(QStringLiteral("Warp BlendShape 绑定范围无效"));
            for (int i = 0; i < shape.count; ++i) {
                const auto* binding = table.blend_shape_keyform_bindings[shape.begin + i];
                if (!range(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, table.warp_deformer_keyforms.size())) return block(QStringLiteral("Warp BlendShape 关键形范围无效"));
                for (int k = 0; k < binding->keyform_sources_blend_shape_counts; ++k) {
                    QVector<float> values;
                    if (!positions(binding->keyform_sources_blend_shape_indices + k, warp->vertex_counts, values) ||
                        std::any_of(values.cbegin(), values.cend(), [](float value) { return value != 0; }))
                        return block(QStringLiteral("祖先 Warp 含非零 BlendShape 位移，尚未证明其全姿态仿射范围"));
                }
            }
        }
        return true;
    }
    bool affineParent(int parent)
    {
        double lowerScale = 1, upperScale = 1;
        for (int index = parent; index >= 0; index = table.deformers[index]->parent_deformer_indices) {
            const auto* node = table.deformers[index]; const int specific = node->specific_sources_indices;
            if (!coverage(index)) return false;
            if (node->types == 1) {
                auto bounds = rotationBounds.constFind(specific);
                if (bounds == rotationBounds.cend()) {
                    if (!catalog.blends(Owner::Rotation, specific).isEmpty()) return block(QStringLiteral("祖先 Rotation %1 的 BlendShape 缩放范围尚未验证").arg(node->ids));
                    const auto* rotation = table.rotation_deformers[specific];
                    if (!range(rotation->keyform_sources_begin_indices, rotation->keyform_sources_counts, table.rotation_deformer_keyforms.size())) return block(QStringLiteral("Rotation 关键形范围无效"));
                    if (!std::isfinite(rotation->base_angles)) return block(QStringLiteral("祖先 Rotation %1 的基础角度无效").arg(node->ids));
                    double minimum = std::numeric_limits<double>::max(), maximum = 0; int sign = 0;
                    for (int k = 0; k < rotation->keyform_sources_counts; ++k) {
                        const auto* form = table.rotation_deformer_keyforms[rotation->keyform_sources_begin_indices + k];
                        if (!std::isfinite(form->angles) || !std::isfinite(form->origin_x) || !std::isfinite(form->origin_y)) return block(QStringLiteral("祖先 Rotation %1 的角度或原点无效").arg(node->ids));
                        const int current = form->scales < 0 ? -1 : 1;
                        if (!std::isfinite(form->scales) || form->scales == 0 || (sign && sign != current)) return block(QStringLiteral("祖先 Rotation %1 的缩放为零或跨零").arg(node->ids));
                        sign = current; minimum = std::min(minimum, std::abs(double(form->scales))); maximum = std::max(maximum, std::abs(double(form->scales)));
                    }
                    rotationBounds.insert(specific, {minimum, maximum});
                    bounds = rotationBounds.constFind(specific);
                }
                const double minimum = bounds->first, maximum = bounds->second;
                lowerScale *= minimum; upperScale *= maximum;
                if (lowerScale < std::numeric_limits<float>::denorm_min() || upperScale > std::numeric_limits<float>::max()) return block(QStringLiteral("祖先累计缩放超出 float 可表达范围"));
            } else {
                if (affineWarps.contains(specific)) continue;
                const auto* warp = table.warp_deformers[specific];
                if (!range(warp->keyform_sources_begin_indices, warp->keyform_sources_counts, table.warp_deformer_keyforms.size()) || !blendPositionsZero(specific)) return false;
                std::array<double, 4> low{INFINITY, INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY, -INFINITY};
                for (int k = 0; k < warp->keyform_sources_counts; ++k) {
                    QVector<float> p; Affine map;
                    if (!positions(warp->keyform_sources_begin_indices + k, warp->vertex_counts, p) || !affineGrid(p, warp->rows, warp->columns, map))
                        return block(QStringLiteral("祖先 Warp %1 的控制格不是可精确表达的仿射格").arg(node->ids));
                    const std::array<double, 4> values{map.a, map.b, map.c, map.d};
                    for (int n = 0; n < 4; ++n) { low[n] = std::min(low[n], values[n]); high[n] = std::max(high[n], values[n]); }
                }
                const auto product = [&](int a, int b) { const std::array<double, 4> values{low[a] * low[b], low[a] * high[b], high[a] * low[b], high[a] * high[b]}; return std::pair{*std::min_element(values.begin(), values.end()), *std::max_element(values.begin(), values.end())}; };
                const auto ad = product(0, 3), bc = product(1, 2);
                if (!(ad.first - bc.second > 0 || ad.second - bc.first < 0)) return block(QStringLiteral("祖先 Warp %1 的普通形范围可能退化，不能证明全姿态可逆").arg(node->ids));
                affineWarps.insert(specific);
            }
        }
        return true;
    }
    bool scalarSign(int parent, int& sign)
    {
        sign = 1;
        for (int index = parent; index >= 0; index = table.deformers[index]->parent_deformer_indices) {
            const auto* node = table.deformers[index]; if (node->types == 0) continue;
            const auto* r = table.rotation_deformers[node->specific_sources_indices];
            sign *= table.rotation_deformer_keyforms[r->keyform_sources_begin_indices]->scales < 0 ? -1 : 1;
        }
        return true;
    }
    bool fixedParent(int parent, Affine& map, double& scalar)
    {
        QVector<int> chain;
        for (int i = parent; i >= 0; i = table.deformers[i]->parent_deformer_indices) chain.append(i);
        std::reverse(chain.begin(), chain.end());
        map = {}; scalar = 1; bool root = true;
        for (int index : chain) {
            const auto* node = table.deformers[index]; const int specific = node->specific_sources_indices;
            if (node->types == 0) {
                const auto* w = table.warp_deformers[specific]; QVector<float> first; Affine local;
                auto fixed = fixedWarps.constFind(specific);
                if (fixed == fixedWarps.cend()) {
                    if (!positions(w->keyform_sources_begin_indices, w->vertex_counts, first) || !affineGrid(first, w->rows, w->columns, local)) return false;
                    for (int k = 1; k < w->keyform_sources_counts; ++k) { QVector<float> p; if (!positions(w->keyform_sources_begin_indices + k, w->vertex_counts, p) || p != first) return block(QStringLiteral("祖先 Warp %1 的坐标随参数改变，Rotation 包装尚不能用固定迁移").arg(node->ids)); }
                    fixedWarps.insert(specific, local);
                    fixed = fixedWarps.constFind(specific);
                }
                map = fixed.value().then(map);
            } else {
                const auto* r = table.rotation_deformers[specific]; const auto* first = table.rotation_deformer_keyforms[r->keyform_sources_begin_indices];
                if (!fixedRotations.contains(specific)) {
                    for (int k = 1; k < r->keyform_sources_counts; ++k) { const auto* f = table.rotation_deformer_keyforms[r->keyform_sources_begin_indices + k]; if (f->angles != first->angles || f->origin_x != first->origin_x || f->origin_y != first->origin_y || f->scales != first->scales || f->is_reflect_x != first->is_reflect_x || f->is_reflect_y != first->is_reflect_y) return block(QStringLiteral("祖先 Rotation %1 随参数改变，尚不能解析此 Warp 父级的固定迁移").arg(node->ids)); }
                    fixedRotations.insert(specific);
                }
                const auto origin = map.point(first->origin_x, first->origin_y); const double angle = first->angles + double(r->base_angles) + (root ? 0 : directionAngle(map));
                scalar *= first->scales; map = rotation(angle, scalar, first->is_reflect_x, first->is_reflect_y, origin[0], origin[1]);
            }
            root = false;
        }
        return true;
    }
    QString reason;
private:
    const CSectionTable& table;
    const Catalog& catalog;
    const Columns& columns;
    CKeyformBindingGrid bindings;
    QHash<QString, const CParameters*> parameters;
    QSet<QPair<int, int>> checkedBindings;
    QHash<int, QPair<double, double>> rotationBounds;
    QSet<int> affineWarps;
    QHash<int, Affine> fixedWarps;
    QSet<int> fixedRotations;
};

bool validOwnerGeometry(const CSectionTable& table, const Catalog& ids, const Columns& columns,
                        const Owner& target, QSet<QPair<int, int>>& checkedPositions, QString& reason)
{
    const int index = ids.index(target);
    const int specific = target.type == Owner::ArtMesh ? index : table.deformers[index]->specific_sources_indices;
    const auto definition = group(target.type), forms = formGroup(target.type);
    const int vertices = target.type == Owner::Rotation ? 1 : columns.sourceInt(definition + ".vertex_counts", specific);
    if (vertices < (target.type == Owner::ArtMesh ? 0 : 1) || vertices > MaximumCount / 2) { reason = QStringLiteral("对象 %1 的顶点数量无效").arg(target.id); return false; }
    if (target.type == Owner::Warp) {
        const auto* w = table.warp_deformers[specific];
        if (w->rows < 1 || w->columns < 1 || (qint64(w->rows) + 1) * (qint64(w->columns) + 1) != vertices) { reason = QStringLiteral("Warp %1 的网格尺寸与顶点数不一致").arg(target.id); return false; }
    }
    const auto check = [&](int begin, int count) {
        const int available = target.type == Owner::ArtMesh ? table.art_mesh_keyforms.size() : target.type == Owner::Warp ? table.warp_deformer_keyforms.size() : table.rotation_deformer_keyforms.size();
        if (count < 1 || !range(begin, count, available)) return false;
        for (int k = 0; k < count; ++k) {
            const int row = begin + k;
            const auto finite = [&](const QByteArray& field) { const auto bits = columns.sourceInt(forms + '.' + field, row); float value; std::memcpy(&value, &bits, 4); return std::isfinite(value); };
            if (!finite("opacities")) return false;
            if (target.type == Owner::Rotation) {
                for (const auto* field : {"angles", "origin_x", "origin_y", "scales"}) if (!finite(field)) return false;
            } else {
                if (target.type == Owner::ArtMesh && !finite("draw_orders")) return false;
                const int position = columns.sourceInt(forms + ".keyform_position_sources_begin_indices", row);
                const QPair<int, int> slice{position, vertices * 2};
                if (!table.keyform_positions || !range(position, slice.second, table.keyform_positions->xys.size())) return false;
                if (!checkedPositions.contains(slice)) {
                    for (int n = 0; n < slice.second; ++n) if (!std::isfinite(table.keyform_positions->xys[position + n])) return false;
                    checkedPositions.insert(slice);
                }
            }
        }
        return true;
    };
    if (!check(columns.sourceInt(definition + ".keyform_sources_begin_indices", specific), columns.sourceInt(definition + ".keyform_sources_counts", specific))) {
        reason = QStringLiteral("对象 %1 的普通关键形范围或数值无效").arg(target.id); return false;
    }
    for (const auto& shape : ids.blends(target.type, specific)) {
        if (!range(shape.begin, shape.count, table.blend_shape_keyform_bindings.size())) { reason = QStringLiteral("对象 %1 的 BlendShape 绑定范围无效").arg(target.id); return false; }
        for (int i = 0; i < shape.count; ++i) {
            const auto* binding = table.blend_shape_keyform_bindings[shape.begin + i];
            if (!check(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts)) { reason = QStringLiteral("对象 %1 的 BlendShape 关键形范围或数值无效").arg(target.id); return false; }
        }
    }
    return true;
}

bool sameUnitGrid(const QVector<float>& values, int rows, int columns)
{
    if (rows < 1 || columns < 1 || qint64(values.size()) != (qint64(rows) + 1) * (qint64(columns) + 1) * 2) return false;
    for (int y = 0; y <= rows; ++y) for (int x = 0; x <= columns; ++x) {
        const int at = (y * (columns + 1) + x) * 2;
        if (values[at] != float(x) / columns || values[at + 1] != float(y) / rows) return false;
    }
    return true;
}
QVector<float> warpPoints(const Raw::Warp& warp)
{
    if (!warp.positions.isEmpty()) return warp.positions;
    QVector<float> result;
    for (int y = 0; y <= warp.rows; ++y) for (int x = 0; x <= warp.columns; ++x) result << float(x) / warp.columns << float(y) / warp.rows;
    return result;
}
bool rectangle(const QVector<float>& points, int rows, int columns, Affine& map)
{ return Geometry::affineGrid(points, rows, columns, map) && map.b == 0 && map.c == 0 && map.a > 0 && map.d > 0; }

bool neutralRemoval(const CSectionTable& table, const Catalog& ids, const Columns& columns, int index, QString& reason)
{
    const auto* node = table.deformers[index]; const auto type = node->types == 0 ? Owner::Warp : Owner::Rotation;
    const auto prefix = group(type); const int specific = node->specific_sources_indices;
    const int binding = node->keyform_binding_sources_indices;
    const int typedBinding = columns.sourceInt(prefix + ".keyform_binding_sources_indices", specific);
    if (binding < 0 || binding >= table.keyform_bindings.size() || table.keyform_bindings[binding]->parameter_binding_index_sources_counts != 0 ||
        typedBinding < 0 || typedBinding >= table.keyform_bindings.size() || table.keyform_bindings[typedBinding]->parameter_binding_index_sources_counts != 0 ||
        columns.sourceInt(prefix + ".keyform_sources_counts", specific) != 1 || !ids.blends(type, specific).isEmpty()) {
        reason = QStringLiteral("待删变形器 %1 必须是无参数轴、单关键形且无自身 BlendShape 驱动").arg(node->ids); return false;
    }
    if (!node->is_visible || !node->is_enabled) { reason = QStringLiteral("待删变形器 %1 的可见性或启用效果不能直接丢弃").arg(node->ids); return false; }
    const int row = columns.sourceInt(prefix + ".keyform_sources_begin_indices", specific);
    const auto forms = formGroup(type);
    const auto opacityBits = columns.sourceInt(forms + ".opacities", row); float opacity; std::memcpy(&opacity, &opacityBits, 4);
    if (opacity != 1) { reason = QStringLiteral("待删变形器 %1 的不透明度不是中性值 1").arg(node->ids); return false; }
    const auto color = [&](int value, bool multiply) {
        if (multiply) { if (!range(value, 1, table.keyform_multiply_colors.size())) return false; const auto* c = table.keyform_multiply_colors[value]; return c->r == 1 && c->g == 1 && c->b == 1; }
        if (!range(value, 1, table.keyform_screen_colors.size())) return false; const auto* c = table.keyform_screen_colors[value]; return c->r == 0 && c->g == 0 && c->b == 0;
    };
    if (columns.present(prefix + ".keyform_color_sources_begin_indices")) {
        const int at = columns.sourceInt(prefix + ".keyform_color_sources_begin_indices", specific);
        if (!color(at, true) || !color(at, false)) { reason = QStringLiteral("待删变形器 %1 的颜色不是中性值").arg(node->ids); return false; }
    }
    for (bool multiply : {true, false}) {
        const auto name = forms + (multiply ? ".multiply_color_sources_indices" : ".screen_color_sources_indices");
        if (columns.present(name) && !color(columns.sourceInt(name, row), multiply)) { reason = QStringLiteral("待删变形器 %1 的关键形颜色不是中性值").arg(node->ids); return false; }
    }
    return true;
}

bool prepare(const QByteArray& base, const CDeformerPreserveRequest& request, const CSectionTable& table,
             const Catalog& ids, const Columns& columns, Plan& plan, QString* error)
{
    if (request.operation.valueless_by_exception() || request.sourceDigest.size() != 32 ||
        request.expectedAuthoringDigest != QCryptographicHash::hash(base, QCryptographicHash::Sha256))
        return fail(error, QStringLiteral("保形请求的来源或作者快照已改变"));
    const auto* insertion = std::get_if<Raw::InsertRequest>(&request.operation);
    if (!(insertion ? Raw::AnalyzeInsert(base, *insertion, plan.impact.structure, error)
                    : Raw::AnalyzeRemove(base, std::get<Raw::RemoveRequest>(request.operation), plan.impact.structure, error))) return false;
    if (plan.impact.structure.reparented.isEmpty()) return true;
    QSet<QPair<int, int>> checkedPositions;
    for (const auto& child : plan.impact.structure.reparented) {
        QString reason;
        if (!validOwnerGeometry(table, ids, columns, child, checkedPositions, reason)) { plan.impact.blockingReasons.append(reason); return true; }
    }
    plan.impact.numericalLimitations.append(QStringLiteral("额外的 float 插值、方向求解与坐标量化可能产生舍入差异；保留严格 Core 对照，不保证逐位一致"));
    Geometry geometry(table, ids, columns);
    const int parent = plan.impact.structure.parentId.isEmpty() ? -1 : ids.deformers.value(plan.impact.structure.parentId, -1);
    const auto reject = [&](const QString& reason) { plan.impact.blockingReasons.append(reason); return true; };
    Raw::Warp warp; Raw::Rotation rot; bool isWarp = false;
    if (insertion) {
        isWarp = std::holds_alternative<Raw::Warp>(insertion->shape);
        if (isWarp) warp = std::get<Raw::Warp>(insertion->shape); else rot = std::get<Raw::Rotation>(insertion->shape);
    } else {
        const int removed = ids.deformers.value(plan.impact.structure.id, -1); const auto* node = table.deformers[removed];
        QString reason; if (!neutralRemoval(table, ids, columns, removed, reason)) return reject(reason);
        isWarp = node->types == 0;
        if (isWarp) { const auto* w = table.warp_deformers[node->specific_sources_indices]; warp.rows = w->rows; warp.columns = w->columns; warp.quad = w->is_quad_source; if (!geometry.positions(w->keyform_sources_begin_indices, w->vertex_counts, warp.positions)) return reject(QStringLiteral("待删 Warp 坐标范围无效")); }
        else { const auto* r = table.rotation_deformers[node->specific_sources_indices]; const auto* f = table.rotation_deformer_keyforms[r->keyform_sources_begin_indices]; rot = {f->origin_x, f->origin_y, f->angles, r->base_angles, f->scales, f->is_reflect_x, f->is_reflect_y}; }
    }
    Affine coordinates; double angle = 0, scale = 1;
    if (isWarp) {
        const auto points = warpPoints(warp);
        if (parent >= 0 && table.deformers[parent]->types == 0) {
            const auto* p = table.warp_deformers[table.deformers[parent]->specific_sources_indices];
            if (warp.rows == p->rows && warp.columns == p->columns && warp.quad == p->is_quad_source && sameUnitGrid(points, warp.rows, warp.columns)) return true;
        }
        Affine rect;
        if (!rectangle(points, warp.rows, warp.columns, rect)) return reject(QStringLiteral("此 Warp 不是正面积的规则矩形；非线性格的通用保形迁移尚未实现"));
        if (!geometry.affineParent(parent)) return reject(geometry.reason);
        if (insertion) { if (!rect.inverse(coordinates)) return reject(QStringLiteral("Warp 矩形无法反解")); } else coordinates = rect;
    } else {
        if (!std::isfinite(rot.scale) || rot.scale == 0) return reject(QStringLiteral("Rotation 缩放为零，无法证明其方向继承与可逆保形迁移"));
        if (!geometry.affineParent(parent)) return reject(geometry.reason);
        const double alpha = double(rot.angle) + rot.baseAngle;
        int sign = 1; geometry.scalarSign(parent, sign);
        if (parent >= 0 && table.deformers[parent]->types == 1) {
            const auto* r = table.rotation_deformers[table.deformers[parent]->specific_sources_indices]; const auto* first = table.rotation_deformer_keyforms[r->keyform_sources_begin_indices];
            for (int k = 1; k < r->keyform_sources_counts; ++k) { const auto* f = table.rotation_deformer_keyforms[r->keyform_sources_begin_indices + k]; if (f->is_reflect_x != first->is_reflect_x || f->is_reflect_y != first->is_reflect_y) return reject(QStringLiteral("直接父 Rotation 的反射随关键形改变，无法使用固定保形迁移")); }
            const int rx = first->is_reflect_x ? -1 : 1, ry = first->is_reflect_y ? -1 : 1;
            Affine child = rotation(alpha, rot.scale * sign * ry, rot.reflectX, rot.reflectY, 0, 0);
            coordinates = child.then({double(rx), 0, 0, double(ry), rot.originX, rot.originY});
        } else {
            Affine p; double scalar = 1;
            if (parent >= 0 && !geometry.fixedParent(parent, p, scalar)) return reject(geometry.reason.isEmpty() ? QStringLiteral("父级不是已证明的静态仿射坐标系") : geometry.reason);
            const auto origin = p.point(rot.originX, rot.originY);
            const auto child = rotation(alpha + (parent < 0 ? 0 : directionAngle(p)), scalar * rot.scale, rot.reflectX, rot.reflectY, origin[0], origin[1]);
            Affine inverse; if (!p.inverse(inverse)) return reject(QStringLiteral("父仿射坐标系退化")); coordinates = child.then(inverse);
        }
        angle = std::remainder(alpha + (sign * double(rot.scale) * (rot.reflectY ? -1 : 1) < 0 ? 180.0 : 0.0), 360.0);
        scale = rot.scale;
        if (insertion) { Affine inverse; if (!coordinates.inverse(inverse)) return reject(QStringLiteral("Rotation 坐标系无法反解")); coordinates = inverse; angle = -angle; scale = 1.0 / scale; }
    }
    const auto add = [&](const Owner& target, const Affine& transform, double rotationAngle, double rotationScale) {
        Migration migration{target, transform, target.type == Owner::Rotation ? rotationAngle : 0, target.type == Owner::Rotation ? rotationScale : 1};
        if (!migration.changes()) return;
        plan.migrations.append(migration);
        if (!transform.identity() || (target.type == Owner::Rotation && rotationAngle != 0)) plan.impact.coordinateOwners.append(target);
        if (target.type == Owner::Rotation && rotationScale != 1) plan.impact.scalarOwners.append(target);
    };
    for (const auto& child : plan.impact.structure.reparented) add(child, coordinates, angle, scale);
    if (scale != 1) {
        QVector<int> pending;
        for (const auto& child : plan.impact.structure.reparented) if (child.type == Owner::Warp) pending.append(ids.index(child));
        for (int i = 0; i < pending.size(); ++i) for (int child : ids.children[pending[i]]) {
            const auto* node = table.deformers[child];
            if (node->types == 0) pending.append(child); else add(owner(*node), {}, 0, scale);
        }
    }
    for (const auto& migration : plan.migrations) {
        if (!geometry.coverage(migration.owner)) plan.impact.blockingReasons.append(geometry.reason);
        if (migration.owner.type == Owner::Rotation) {
            const int specific = table.deformers[ids.index(migration.owner)]->specific_sources_indices;
            if (!ids.blends(Owner::Rotation, specific).isEmpty())
                plan.impact.blockingReasons.append(QStringLiteral("Rotation %1 的 BlendShape 角度/缩放/反射迁移尚未验证").arg(migration.owner.id));
        }
    }
    return true;
}

bool migrationBudget(const CSectionTable& table, const Catalog& ids, const Columns& columns, const Plan& plan, QString* error)
{
    QMap<QByteArray, qint64> additions;
    const auto reserve = [&](const QByteArray& name, qint64 count) {
        if (count < 0 || count > MaximumCount - columns.count(name) - additions.value(name)) return false;
        additions[name] += count; return true;
    };
    for (const auto& migration : plan.migrations) {
        const auto type = migration.owner.type; const int index = ids.index(migration.owner);
        const int specific = type == Owner::ArtMesh ? index : table.deformers[index]->specific_sources_indices;
        const auto definition = group(type), forms = formGroup(type);
        if (type != Owner::ArtMesh && !reserve(definition + ".keyform_sources_counts", 1)) return fail(error, QStringLiteral("保形专用源行总量超出范围"));
        const int vertices = type == Owner::Rotation ? 0 : columns.sourceInt(definition + ".vertex_counts", specific);
        QSet<QPair<int, int>> normalPositions, deltaPositions;
        const auto rows = [&](int begin, int count, bool delta) {
            if (!range(begin, count, columns.count(forms + ".opacities")) || count < 1 || !reserve(forms + ".opacities", count)) return false;
            if (type == Owner::Rotation || migration.coordinates.identity()) return true;
            auto& visited = delta ? deltaPositions : normalPositions;
            for (int k = 0; k < count; ++k) {
                const QPair<int, int> slice{columns.sourceInt(forms + ".keyform_position_sources_begin_indices", begin + k), vertices * 2};
                if (!visited.contains(slice)) {
                    if (!reserve("keyform_positions.xys", slice.second)) return false;
                    visited.insert(slice);
                }
            }
            return true;
        };
        if (!rows(columns.sourceInt(definition + ".keyform_sources_begin_indices", specific), columns.sourceInt(definition + ".keyform_sources_counts", specific), false))
            return fail(error, QStringLiteral("保形普通关键形或坐标总量超出范围"));
        for (const auto& shape : ids.blends(type, specific)) {
            if (!range(shape.begin, shape.count, table.blend_shape_keyform_bindings.size()) ||
                (type != Owner::ArtMesh && !reserve(blendGroup(type) + ".target_indices", 1)) ||
                !reserve("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", shape.count)) return fail(error, QStringLiteral("保形 BlendShape 绑定总量超出范围"));
            for (int i = 0; i < shape.count; ++i) {
                const auto* binding = table.blend_shape_keyform_bindings[shape.begin + i];
                if (!rows(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, true)) return fail(error, QStringLiteral("保形 BlendShape 关键形或坐标总量超出范围"));
            }
        }
    }
    return true;
}

void migrate(const CSectionTable& table, const Catalog& ids, Columns& columns, const Migration& migration)
{
    const auto type = migration.owner.type; const auto definition = group(type), forms = formGroup(type), blends = blendGroup(type);
    const int index = ids.index(migration.owner);
    int originalSpecific = index, destinationSpecific = index;
    if (type != Owner::ArtMesh) {
        originalSpecific = table.deformers[index]->specific_sources_indices;
        destinationSpecific = columns.clone(definition, originalSpecific, 1);
        columns.set("deformers.specific_sources_indices", index, integer(destinationSpecific));
    }
    const int begin = columns.sourceInt(definition + ".keyform_sources_begin_indices", originalSpecific);
    const int count = columns.sourceInt(definition + ".keyform_sources_counts", originalSpecific);
    const int vertices = type == Owner::Rotation ? 1 : columns.sourceInt(definition + ".vertex_counts", originalSpecific);
    QMap<QPair<int, int>, int> pointCache, deltaCache;
    const auto convertRows = [&](int oldBegin, int rowCount, bool delta) {
        const int destination = columns.clone(forms, oldBegin, rowCount);
        for (int k = 0; k < rowCount; ++k) {
            const int old = oldBegin + k, next = destination + k;
            if (type == Owner::Rotation) {
                const auto* f = table.rotation_deformer_keyforms[old];
                if (!migration.coordinates.identity()) {
                    const auto p = migration.coordinates.point(f->origin_x, f->origin_y, delta);
                    columns.set(forms + ".origin_x", next, floating(checkedFloat(p[0])));
                    columns.set(forms + ".origin_y", next, floating(checkedFloat(p[1])));
                }
                if (!delta && migration.angle != 0) columns.set(forms + ".angles", next, floating(checkedFloat(double(f->angles) + migration.angle)));
                if (migration.scale != 1) columns.set(forms + ".scales", next, floating(checkedFloat(double(f->scales) * migration.scale)));
            } else if (!migration.coordinates.identity() && vertices > 0) {
                const int position = columns.sourceInt(forms + ".keyform_position_sources_begin_indices", old);
                const qint64 length = qint64(vertices) * 2;
                if (vertices < 1 || !table.keyform_positions || !range(position, length, table.keyform_positions->xys.size())) throw std::range_error("Invalid positions");
                auto& cache = delta ? deltaCache : pointCache;
                const QPair<int, int> key{position, int(length)};
                auto found = cache.constFind(key);
                if (found == cache.cend()) {
                    QVector<float> points; points.reserve(length);
                    for (int v = 0; v < vertices; ++v) {
                        const auto p = migration.coordinates.point(table.keyform_positions->xys[position + v * 2], table.keyform_positions->xys[position + v * 2 + 1], delta);
                        points << checkedFloat(p[0]) << checkedFloat(p[1]);
                    }
                    const int appended = columns.appendPositions(points); cache.insert(key, appended); found = cache.constFind(key);
                }
                columns.set(forms + ".keyform_position_sources_begin_indices", next, integer(*found));
            }
        }
        return destination;
    };
    columns.set(definition + ".keyform_sources_begin_indices", destinationSpecific, integer(convertRows(begin, count, false)));
    for (const auto& shape : ids.blends(type, originalSpecific)) {
        if (!range(shape.begin, shape.count, table.blend_shape_keyform_bindings.size())) throw std::range_error("Invalid BlendShape bindings");
        const int newShape = type == Owner::ArtMesh ? shape.row : columns.clone(blends, shape.row, 1);
        if (type != Owner::ArtMesh) columns.set(blends + ".target_indices", newShape, integer(destinationSpecific));
        if (shape.count == 0) continue;
        const int bindings = columns.clone("blend_shape_keyform_bindings", shape.begin, shape.count);
        for (int k = 0; k < shape.count; ++k) {
            const auto* binding = table.blend_shape_keyform_bindings[shape.begin + k];
            const int newRows = convertRows(binding->keyform_sources_blend_shape_indices, binding->keyform_sources_blend_shape_counts, true);
            columns.set("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", bindings + k, integer(newRows));
        }
        columns.set(blends + ".blend_shape_keyform_binding_sources_begin_indices", newShape, integer(bindings));
    }
}

template<class Action> bool execute(const QByteArray& base, const CDeformerPreserveRequest& request, QString* error, Action&& action)
{
    if (error) error->clear();
    try {
        auto reader = CReader::FromBytes(base); CSectionTable table(reader);
        if (!table.IsValid()) return fail(error, QStringLiteral("保形模型快照无效：%1").arg(reader.Error()));
        Catalog ids(table); Columns columns(base, table); Plan plan;
        if (!prepare(base, request, table, ids, columns, plan, error)) return false;
        if (plan.impact.blockingReasons.isEmpty() && !migrationBudget(table, ids, columns, plan, error)) return false;
        return action(table, ids, columns, plan);
    } catch (const std::exception&) { return fail(error, QStringLiteral("保形数据范围、数量、内存或变换结果无效，原输出保持不变")); }
}
}

bool CDeformerStructureReplay::Analyze(const QByteArray& base, const CDeformerPreserveRequest& request, Impact& result,
    QString* error, CExportControl* control)
{
    if (error)
        error->clear();
    if (canceled(control, error))
        return false;
    if (const auto* resize = std::get_if<Raw::ResizeWarpRequest>(&request.operation)) {
        if (!resizeSnapshot(base, request, error))
            return false;
        Impact candidate;
        if (!Raw::AnalyzeResizeWarp(base, *resize, candidate.structure, error, control))
            return false;
        candidate.coordinateOwners.append({ Owner::Warp, candidate.structure.id });
        candidate.numericalLimitations = candidate.structure.warpGrid->limitations;
        result = std::move(candidate);
        return true;
    }
    return execute(base, request, error, [&](const auto&, const auto&, const auto&, Plan& plan) {
        if (canceled(control, error))
            return false;
        result = std::move(plan.impact);
        return true;
    });
}

bool CDeformerStructureReplay::Build(const QByteArray& base, const CDeformerPreserveRequest& request,
    Raw::Result& result, QString* error, CExportControl* control)
{
    if (error)
        error->clear();
    if (canceled(control, error))
        return false;
    if (const auto* resize = std::get_if<Raw::ResizeWarpRequest>(&request.operation)) {
        if (!resizeSnapshot(base, request, error))
            return false;
        return Raw::ResizeWarp(base, *resize, result, error, control);
    }
    return execute(
        base, request, error, [&](const CSectionTable& table, const Catalog& ids, Columns& columns, Plan& plan) {
            if (!plan.impact.blockingReasons.isEmpty())
                return fail(error, plan.impact.blockingReasons.join(QLatin1Char('\n')));
            for (const auto& item : plan.migrations)
                migrate(table, ids, columns, item);
            QByteArray prepared = base;
            if ((!columns.state.columns.isEmpty() || !columns.state.counts.isEmpty())
                && !Codec::BuildColumnState(base, columns.state, prepared, error))
                return false;
            Raw::Result candidate;
            const auto* insertion = std::get_if<Raw::InsertRequest>(&request.operation);
            if (!(insertion ? Raw::Insert(prepared, *insertion, candidate, error)
                            : Raw::Remove(prepared, std::get<Raw::RemoveRequest>(request.operation), candidate, error)))
                return false;
            if (!Codec::CaptureColumnState(base, candidate.bytes, candidate.state, error))
                return false;
            if (canceled(control, error))
                return false;
            result = std::move(candidate);
            return true;
        });
}
}
