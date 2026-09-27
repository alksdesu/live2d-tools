#include "cgeometryobjectpairs.h"
#include "cmodeldocument.h"
#include "cmeshtopology.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
using Pairs = CGeometryObjectPairs;
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool textValid(const QString& value)
{
    return !value.isEmpty() && value.size() <= Pairs::MaximumTextLength && !value.contains(QChar::Null);
}
bool sameFloat(float first, float second)
{
    return std::memcmp(&first, &second, sizeof(float)) == 0;
}
bool validType(Pairs::Type type)
{
    return type == Pairs::Type::ArtMesh || type == Pairs::Type::Warp || type == Pairs::Type::Rotation;
}
QString objectKey(const Pairs::Object& object)
{
    return QString::number(int(object.type)) + QLatin1Char('|') + object.id;
}
template<int Size> using Polygon = std::array<int, Size>;
template<int Size> Polygon<Size> canonical(Polygon<Size> polygon)
{
    auto result = polygon;
    for (int offset = 1; offset < Size; ++offset) {
        std::rotate(polygon.begin(), polygon.begin() + 1, polygon.end());
        if (polygon < result) result = polygon;
    }
    return result;
}
template<int Size> void normalizePolygons(QVector<Polygon<Size>>& polygons)
{
    for (auto& polygon : polygons) polygon = canonical<Size>(polygon);
    std::sort(polygons.begin(), polygons.end());
}
QVector<Polygon<3>> triangles(const QVector<quint16>& indices)
{
    QVector<Polygon<3>> result;
    result.reserve(indices.size() / 3);
    for (qsizetype index = 0; index < indices.size(); index += 3)
        result.append({indices[index], indices[index + 1], indices[index + 2]});
    return result;
}
QVector<Polygon<4>> quads(const Pairs::Endpoint& endpoint)
{
    QVector<Polygon<4>> result;
    result.reserve(endpoint.rows * endpoint.columns);
    const int stride = endpoint.columns + 1;
    for (int row = 0; row < endpoint.rows; ++row)
        for (int column = 0; column < endpoint.columns; ++column) {
            const int start = row * stride + column;
            result.append({start, start + 1, start + stride + 1, start + stride});
        }
    return result;
}
template<int Size> bool sameMappedPolygons(QVector<Polygon<Size>> first, QVector<Polygon<Size>> second,
                                          const QVector<int>& mapping, bool reversed)
{
    if (first.size() != second.size()) return false;
    for (auto& polygon : first) {
        for (auto& vertex : polygon) vertex = mapping[vertex];
        if (reversed) std::reverse(polygon.begin(), polygon.end());
    }
    normalizePolygons<Size>(first); normalizePolygons<Size>(second);
    return first == second;
}
bool permutation(const QVector<int>& values, int size)
{
    if (size < 0 || values.size() != size) return false;
    QVector<quint8> seen(size, 0);
    for (const auto value : values) {
        if (value < 0 || value >= size || seen[value]) return false;
        seen[value] = 1;
    }
    return true;
}
bool partialMapping(const QVector<int>& values, int before, int after)
{
    if (values.size() != before || after < 0 || after > Pairs::MaximumMappings) return false;
    QVector<quint8> seen(after, 0);
    for (const auto value : values) {
        if (value < -1 || value >= after || (value >= 0 && seen[value])) return false;
        if (value >= 0) seen[value] = 1;
    }
    return true;
}
QVector<int> identity(int count)
{
    QVector<int> result; result.reserve(count);
    for (int index = 0; index < count; ++index) result.append(index);
    return result;
}
bool endpoint(Pairs::Endpoint& value, qint64& budget, QString* error)
{
    if (!validType(value.object.type) || !textValid(value.object.id) || value.vertexCount < 0 ||
        value.vertexCount > Pairs::MaximumVertices || value.keyformCount < 1 || value.keyformCount > Pairs::MaximumMappings)
        return fail(error, QStringLiteral("配对对象类型、标识或数量无效"));
    budget += value.vertexCount + value.triangles.size();
    if (budget > Pairs::MaximumMappings) return fail(error, QStringLiteral("配对数据超过总量限制"));
    if (value.object.type == Pairs::Type::ArtMesh) {
        if (value.rows != 0 || value.columns != 0 || value.vertexCount < 1 || value.vertexCount > CMeshTopology::IndexCapacity ||
            value.triangles.isEmpty() || value.triangles.size() % 3 != 0)
            return fail(error, QStringLiteral("ArtMesh 配对拓扑无效"));
        for (const auto index : value.triangles)
            if (index >= value.vertexCount) return fail(error, QStringLiteral("配对三角索引越界"));
        auto normalized = triangles(value.triangles); normalizePolygons<3>(normalized);
        value.triangles.clear(); value.triangles.reserve(normalized.size() * 3);
        for (const auto& triangle : normalized) for (const auto index : triangle) value.triangles.append(quint16(index));
    } else if (value.object.type == Pairs::Type::Warp) {
        if (!value.triangles.isEmpty() || value.rows < 1 || value.columns < 1 ||
            (qint64(value.rows) + 1) * (qint64(value.columns) + 1) != value.vertexCount)
            return fail(error, QStringLiteral("Warp 配对网格行列无效"));
    } else if (value.vertexCount != 0 || value.rows != 0 || value.columns != 0 || !value.triangles.isEmpty())
        return fail(error, QStringLiteral("Rotation 配对不能包含顶点映射"));
    qint64 expected = 1;
    QSet<QString> parameters;
    for (int index = 0; index < value.axes.size(); ++index) {
        const auto& axis = value.axes[index];
        if (axis.index != index || !textValid(axis.parameterId) || parameters.contains(axis.parameterId) ||
            axis.keys.isEmpty() || (axis.parameterType != 0 && axis.parameterType != 1))
            return fail(error, QStringLiteral("配对参数轴缺失、重复或顺序无效"));
        if (axis.keys.size() > Pairs::MaximumMappings - budget || axis.keys.size() > Pairs::MaximumMappings / expected)
            return fail(error, QStringLiteral("配对关键形矩阵超过限制"));
        parameters.insert(axis.parameterId);
        for (int key = 0; key < axis.keys.size(); ++key)
            if (!std::isfinite(axis.keys[key]) || (key > 0 && !(axis.keys[key] > axis.keys[key - 1])))
                return fail(error, QStringLiteral("配对参数关键值必须有限且严格递增"));
        expected *= axis.keys.size(); budget += axis.keys.size();
        if (budget > Pairs::MaximumMappings) return fail(error, QStringLiteral("配对数据超过总量限制"));
    }
    if (expected != value.keyformCount) return fail(error, QStringLiteral("配对关键形数量与参数轴不符"));
    return true;
}
bool pairValid(Pairs::Pair& pair, qint64& budget, QString* error)
{
    if (!textValid(pair.id) || pair.first.object == pair.second.object || pair.first.object.type != pair.second.object.type)
        return fail(error, QStringLiteral("几何配对需要两个不同的同类型对象"));
    if (!endpoint(pair.first, budget, error) || !endpoint(pair.second, budget, error)) return false;
    const auto x = pair.lineDirection.x(), y = pair.lineDirection.y();
    if (!std::isfinite(pair.linePoint.x()) || !std::isfinite(pair.linePoint.y()) || !std::isfinite(x) || !std::isfinite(y) ||
        (x == 0 && y == 0)) return fail(error, QStringLiteral("镜像直线必须有限且方向非零"));
    pair.linePoint = QPointF(pair.linePoint.x() == 0 ? 0 : pair.linePoint.x(), pair.linePoint.y() == 0 ? 0 : pair.linePoint.y());
    pair.lineDirection = QPointF(x == 0 ? 0 : x, y == 0 ? 0 : y);
    const auto lx = pair.rotationLocalX, ly = pair.rotationLocalY;
    if (!std::isfinite(lx.x()) || !std::isfinite(lx.y()) || !std::isfinite(ly.x()) || !std::isfinite(ly.y()))
        return fail(error, QStringLiteral("Rotation 局部基对应必须有限"));
    if (pair.first.object.type == Pairs::Type::Rotation) {
        const double xx = lx.x() * lx.x() + lx.y() * lx.y(), yy = ly.x() * ly.x() + ly.y() * ly.y();
        const double xy = lx.x() * ly.x() + lx.y() * ly.y();
        if (!std::isfinite(xx) || !std::isfinite(yy) || !std::isfinite(xy) || std::abs(xx - 1) > Pairs::RotationFrameTolerance ||
            std::abs(yy - 1) > Pairs::RotationFrameTolerance || std::abs(xy) > Pairs::RotationFrameTolerance)
            return fail(error, QStringLiteral("Rotation 局部基对应必须为正交单位矩阵"));
    } else if (lx.x() != 1 || lx.y() != 0 || ly.x() != 0 || ly.y() != 1)
        return fail(error, QStringLiteral("只有 Rotation 配对可以设置局部基对应"));
    pair.rotationLocalX = QPointF(lx.x() == 0 ? 0 : lx.x(), lx.y() == 0 ? 0 : lx.y());
    pair.rotationLocalY = QPointF(ly.x() == 0 ? 0 : ly.x(), ly.y() == 0 ? 0 : ly.y());
    if (pair.first.vertexCount != pair.second.vertexCount ||
        !permutation(pair.firstVertexToSecondVertex, pair.first.vertexCount))
        return fail(error, QStringLiteral("配对顶点必须具有完整的一一对应"));
    const auto type = pair.first.object.type;
    if (type == Pairs::Type::ArtMesh && !sameMappedPolygons<3>(triangles(pair.first.triangles), triangles(pair.second.triangles),
        pair.firstVertexToSecondVertex, pair.reversedWinding)) return fail(error, QStringLiteral("配对三角连接或绕序不对应"));
    if (type == Pairs::Type::Warp && !sameMappedPolygons<4>(quads(pair.first), quads(pair.second),
        pair.firstVertexToSecondVertex, pair.reversedWinding)) return fail(error, QStringLiteral("配对 Warp 单元或绕序不对应"));
    if (type == Pairs::Type::Rotation && pair.reversedWinding)
        return fail(error, QStringLiteral("Rotation 配对没有三角绕序"));
    if (pair.first.axes.size() != pair.second.axes.size() || pair.axes.size() != pair.first.axes.size())
        return fail(error, QStringLiteral("配对参数轴必须完整对应"));
    QVector<quint8> seen(pair.second.axes.size(), 0);
    std::sort(pair.axes.begin(), pair.axes.end(), [](const auto& first, const auto& second) { return first.firstAxis < second.firstAxis; });
    for (int index = 0; index < pair.axes.size(); ++index) {
        const auto& mapping = pair.axes[index];
        if (mapping.firstAxis != index || mapping.secondAxis < 0 || mapping.secondAxis >= seen.size() || seen[mapping.secondAxis])
            return fail(error, QStringLiteral("配对参数轴重复或越界"));
        seen[mapping.secondAxis] = 1;
        const auto& first = pair.first.axes[index]; const auto& second = pair.second.axes[mapping.secondAxis];
        if (first.parameterType != second.parameterType || first.keys.size() != second.keys.size() ||
            !permutation(mapping.firstKeyToSecondKey, first.keys.size()))
            return fail(error, QStringLiteral("配对关键值必须具有完整的一一对应"));
        budget += mapping.firstKeyToSecondKey.size();
    }
    budget += pair.firstVertexToSecondVertex.size();
    return budget <= Pairs::MaximumMappings || fail(error, QStringLiteral("配对数据超过总量限制"));
}
int objectCount(const CModelDocument& document, const Pairs::Object& object)
{
    int count = 0;
    if (object.type == Pairs::Type::ArtMesh) {
        for (const auto& mesh : document.Drawables()) if (mesh.id == object.id) ++count;
    } else for (const auto& deformer : document.Deformers())
        if (deformer.id == object.id && ((object.type == Pairs::Type::Warp && deformer.type == CModelDeformer::Warp) ||
                                       (object.type == Pairs::Type::Rotation && deformer.type == CModelDeformer::Rotation))) ++count;
    return count;
}
bool objectExists(const CModelDocument& document, const Pairs::Object& object)
{
    return objectCount(document, object) == 1;
}
bool sameGeometry(const Pairs::Endpoint& first, const Pairs::Endpoint& second)
{
    return first.object == second.object && first.vertexCount == second.vertexCount && first.rows == second.rows &&
           first.columns == second.columns && first.triangles == second.triangles;
}
bool compose(const QVector<int>& pairMap, const QVector<int>& firstRemap, const QVector<int>& secondRemap,
             int newCount, QVector<int>& result)
{
    QVector<int> candidate(newCount, -1);
    for (int index = 0; index < pairMap.size(); ++index) {
        const auto first = firstRemap[index], second = secondRemap[pairMap[index]];
        if ((first < 0) != (second < 0)) return false;
        if (first >= 0) candidate[first] = second;
    }
    if (!permutation(candidate, newCount)) return false;
    result = std::move(candidate); return true;
}
bool fields(const QJsonObject& object, QStringList keys)
{
    std::sort(keys.begin(), keys.end()); return object.keys() == keys;
}
bool integer(const QJsonValue& value, int& result, int minimum, int maximum)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) return false;
    result = int(number); return true;
}
QJsonArray intArray(const QVector<int>& values)
{
    QJsonArray result; for (const auto value : values) result.append(value); return result;
}
bool readInts(const QJsonValue& value, QVector<int>& result, int maximum, qint64& budget)
{
    if (!value.isArray()) return false;
    const auto array = value.toArray(); budget += array.size();
    if (budget > Pairs::MaximumMappings) return false;
    QVector<int> candidate; candidate.reserve(array.size());
    for (const auto item : array) { int number = -1; if (!integer(item, number, 0, maximum)) return false; candidate.append(number); }
    result = std::move(candidate); return true;
}
QString floatBits(float value)
{
    quint32 bits = 0; std::memcpy(&bits, &value, sizeof(bits));
    return QString::number(bits, 16).rightJustified(8, QLatin1Char('0'));
}
bool readFloat(const QJsonValue& value, float& result)
{
    if (!value.isString()) return false;
    const auto string = value.toString();
    if (string.size() != 8 || !std::all_of(string.cbegin(), string.cend(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    })) return false;
    bool ok = false; const quint32 bits = string.toUInt(&ok, 16);
    if (!ok) return false;
    float candidate = 0; std::memcpy(&candidate, &bits, sizeof(bits));
    if (!std::isfinite(candidate)) return false;
    result = candidate; return true;
}
QJsonObject endpointJson(const Pairs::Endpoint& endpoint)
{
    QJsonArray axes, indices;
    for (const auto index : endpoint.triangles) indices.append(int(index));
    for (const auto& axis : endpoint.axes) {
        QJsonArray keys; for (const auto value : axis.keys) keys.append(floatBits(value));
        axes.append(QJsonObject{{"index", axis.index}, {"parameterId", axis.parameterId}, {"keys", keys}, {"parameterType", axis.parameterType}});
    }
    return {{"type", Pairs::TypeName(endpoint.object.type)}, {"id", endpoint.object.id}, {"vertexCount", endpoint.vertexCount},
            {"rows", endpoint.rows}, {"columns", endpoint.columns}, {"triangles", indices}, {"axes", axes}, {"keyformCount", endpoint.keyformCount}};
}
bool readEndpoint(const QJsonValue& value, Pairs::Endpoint& result, qint64& budget)
{
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    if (!fields(object, {"type", "id", "vertexCount", "rows", "columns", "triangles", "axes", "keyformCount"}) ||
        !object["type"].isString() || !object["id"].isString() || !object["axes"].isArray()) return false;
    const auto type = object["type"].toString();
    if (type == QLatin1String("artmesh")) result.object.type = Pairs::Type::ArtMesh;
    else if (type == QLatin1String("warp")) result.object.type = Pairs::Type::Warp;
    else if (type == QLatin1String("rotation")) result.object.type = Pairs::Type::Rotation;
    else return false;
    result.object.id = object["id"].toString();
    if (!integer(object["vertexCount"], result.vertexCount, 0, Pairs::MaximumVertices) ||
        !integer(object["rows"], result.rows, 0, Pairs::MaximumVertices) ||
        !integer(object["columns"], result.columns, 0, Pairs::MaximumVertices) ||
        !integer(object["keyformCount"], result.keyformCount, 1, Pairs::MaximumMappings)) return false;
    QVector<int> indices;
    if (!readInts(object["triangles"], indices, CMeshTopology::MaximumIndex, budget)) return false;
    for (const auto index : indices) result.triangles.append(quint16(index));
    const auto axes = object["axes"].toArray(); budget += axes.size();
    if (budget > Pairs::MaximumMappings) return false;
    for (const auto axisValue : axes) {
        if (!axisValue.isObject()) return false;
        const auto row = axisValue.toObject(); Pairs::Axis axis;
        if (!fields(row, {"index", "parameterId", "keys", "parameterType"}) || !row["parameterId"].isString() ||
            !row["keys"].isArray() || !integer(row["index"], axis.index, 0, Pairs::MaximumMappings) ||
            !integer(row["parameterType"], axis.parameterType, 0, 1)) return false;
        axis.parameterId = row["parameterId"].toString(); const auto keys = row["keys"].toArray(); budget += keys.size();
        if (budget > Pairs::MaximumMappings) return false;
        for (const auto key : keys) { float number = 0; if (!readFloat(key, number)) return false; axis.keys.append(number); }
        result.axes.append(std::move(axis));
    }
    return true;
}
bool readPoint(const QJsonValue& value, QPointF& result)
{
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    if (array.size() != 2 || !array[0].isDouble() || !array[1].isDouble() ||
        !std::isfinite(array[0].toDouble()) || !std::isfinite(array[1].toDouble())) return false;
    result = QPointF(array[0].toDouble(), array[1].toDouble()); return true;
}
}

bool CGeometryObjectPairs::Axis::operator==(const Axis& other) const
{
    if (index != other.index || parameterId != other.parameterId || parameterType != other.parameterType || keys.size() != other.keys.size()) return false;
    for (int key = 0; key < keys.size(); ++key) if (!sameFloat(keys[key], other.keys[key])) return false;
    return true;
}
bool CGeometryObjectPairs::Endpoint::operator==(const Endpoint& other) const
{
    return sameGeometry(*this, other) && axes == other.axes && keyformCount == other.keyformCount;
}
bool CGeometryObjectPairs::Pair::operator==(const Pair& other) const
{
    return id == other.id && first == other.first && second == other.second && firstVertexToSecondVertex == other.firstVertexToSecondVertex &&
        axes == other.axes && linePoint.x() == other.linePoint.x() && linePoint.y() == other.linePoint.y() &&
        lineDirection.x() == other.lineDirection.x() && lineDirection.y() == other.lineDirection.y() &&
        reversedWinding == other.reversedWinding && enabled == other.enabled && requiresReview == other.requiresReview &&
        rotationLocalX.x() == other.rotationLocalX.x() && rotationLocalX.y() == other.rotationLocalX.y() &&
        rotationLocalY.x() == other.rotationLocalY.x() && rotationLocalY.y() == other.rotationLocalY.y();
}
QString CGeometryObjectPairs::TypeName(Type type)
{
    switch (type) { case Type::ArtMesh: return QStringLiteral("artmesh"); case Type::Warp: return QStringLiteral("warp");
                   case Type::Rotation: return QStringLiteral("rotation"); }
    return {};
}
bool CGeometryObjectPairs::Normalize(const State& state, State& result, QString* error) try
{
    if (state.pairs.size() > MaximumPairs || (state.sourceDigest.size() != 32 && (!state.sourceDigest.isEmpty() || !state.pairs.isEmpty())))
        return fail(error, QStringLiteral("几何配对源摘要或数量无效"));
    State candidate = state; qint64 budget = 0; QSet<QString> ids, objects;
    for (auto& pair : candidate.pairs) {
        if (!pairValid(pair, budget, error)) return false;
        const auto first = objectKey(pair.first.object), second = objectKey(pair.second.object);
        if (ids.contains(pair.id) || objects.contains(first) || objects.contains(second))
            return fail(error, QStringLiteral("几何配对标识或对象重复"));
        ids.insert(pair.id); objects.insert(first); objects.insert(second);
    }
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法检查几何配对数据")); }

bool CGeometryObjectPairs::DescribeEndpoint(const CModelDocument& document, const Object& object, Endpoint& result, QString* error) try
{
    if (!document.IsLoaded() || !validType(object.type) || !textValid(object.id) || !objectExists(document, object))
        return fail(error, QStringLiteral("配对对象不存在、类型不符或标识重复"));
    Endpoint candidate; candidate.object = object;
    if (object.type == Type::ArtMesh) {
        for (const auto& mesh : document.Drawables()) if (mesh.id == object.id) candidate.vertexCount = mesh.vertexCount;
        candidate.triangles = document.DrawableIndices(object.id);
    } else if (object.type == Type::Warp) {
        for (const auto& node : document.Deformers()) if (node.id == object.id) {
            candidate.rows = node.rows; candidate.columns = node.columns;
            const qint64 count = (qint64(node.rows) + 1) * (qint64(node.columns) + 1);
            if (count < 1 || count > MaximumVertices) return fail(error, QStringLiteral("配对 Warp 控制点数量无效"));
            candidate.vertexCount = int(count);
        }
    }
    CKeyformGridSnapshot grid;
    if (!document.DescribeKeyformGrid(object.id, TypeName(object.type), grid, error)) return false;
    if (!grid.axesValid || grid.expectedKeyformCount != grid.keyformCount)
        return fail(error, grid.diagnostic.isEmpty() ? QStringLiteral("配对关键形矩阵不完整") : grid.diagnostic);
    candidate.keyformCount = grid.keyformCount;
    for (int index = 0; index < grid.axes.size(); ++index)
        candidate.axes.append({index, grid.axes[index].parameterId, grid.axes[index].keys, grid.axes[index].parameterType});
    qint64 budget = 0; if (!endpoint(candidate, budget, error)) return false;
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取配对对象结构")); }

bool CGeometryObjectPairs::Validate(const State& state, const CModelDocument& document, QString* error) try
{
    State normalized;
    if (!Normalize(state, normalized, error)) return false;
    if (!document.IsLoaded() || (!state.sourceDigest.isEmpty() && state.sourceDigest != document.SourceDigest()))
        return fail(error, QStringLiteral("几何配对与模型来源不匹配"));
    for (const auto& pair : normalized.pairs) {
        if (!objectExists(document, pair.first.object) || !objectExists(document, pair.second.object))
            return fail(error, QStringLiteral("几何配对引用了缺失或重复的对象"));
        if (pair.requiresReview) continue;
        Endpoint first, second;
        if (!DescribeEndpoint(document, pair.first.object, first, error) || !DescribeEndpoint(document, pair.second.object, second, error)) return false;
        if (!(first == pair.first) || !(second == pair.second))
            return fail(error, QStringLiteral("几何配对结构已经变化，需要重新确认"));
    }
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证几何配对")); }

bool CGeometryObjectPairs::ResolveKeyform(const Pair& pair, int keyform, bool firstToSecond, int& result, QString* error) try
{
    auto normalized = pair; qint64 budget = 0;
    if (!pairValid(normalized, budget, error)) return false;
    if (!pair.enabled || pair.requiresReview) return fail(error, QStringLiteral("几何配对已停用或等待重新确认"));
    const auto& source = firstToSecond ? normalized.first : normalized.second;
    const auto& target = firstToSecond ? normalized.second : normalized.first;
    if (keyform < 0 || keyform >= source.keyformCount) return fail(error, QStringLiteral("配对源关键形索引越界"));
    QVector<int> sourceDigits; int remaining = keyform;
    for (const auto& axis : source.axes) { sourceDigits.append(remaining % axis.keys.size()); remaining /= axis.keys.size(); }
    QVector<int> digits(target.axes.size(), -1);
    for (const auto& mapping : normalized.axes) {
        if (firstToSecond) digits[mapping.secondAxis] = mapping.firstKeyToSecondKey[sourceDigits[mapping.firstAxis]];
        else digits[mapping.firstAxis] = mapping.firstKeyToSecondKey.indexOf(sourceDigits[mapping.secondAxis]);
    }
    int resolved = 0, stride = 1;
    for (int axis = 0; axis < digits.size(); ++axis) { resolved += digits[axis] * stride; stride *= target.axes[axis].keys.size(); }
    result = resolved; if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法解析配对关键形")); }

bool CGeometryObjectPairs::Reconcile(const State& state, const CModelDocument& before, const CModelDocument& after,
                                    const QVector<VertexRemap>& vertices, const QVector<AxisRemap>& axes,
                                    State& result, QString* error) try
{
    State normalizedInput;
    if (!Normalize(state, normalizedInput, error) || !Validate(normalizedInput, before, error)) return false;
    if (!after.IsLoaded() || before.SourceDigest() != after.SourceDigest())
        return fail(error, QStringLiteral("结构迁移不能改变几何配对的模型来源"));
    qint64 mappingBudget = vertices.size() + axes.size();
    for (const auto& remap : vertices) mappingBudget += remap.oldToNew.size();
    for (const auto& remap : axes) mappingBudget += remap.oldKeyToNewKey.size();
    if (mappingBudget > MaximumMappings) return fail(error, QStringLiteral("几何配对迁移映射超过总量限制"));
    QHash<QString, Endpoint> oldEndpoints, newEndpoints;
    const auto describe = [&](const Object& object) {
        const auto key = objectKey(object); if (oldEndpoints.contains(key)) return true;
        Endpoint old, next;
        if (!DescribeEndpoint(before, object, old, error) || !DescribeEndpoint(after, object, next, error)) return false;
        oldEndpoints.insert(key, old); newEndpoints.insert(key, next); return true;
    };
    QHash<QString, QVector<int>> vertexMaps;
    QHash<QString, QHash<int, AxisRemap>> axisMaps;
    QHash<QString, QSet<int>> mappedAxes;
    QSet<QString> mappedObjects;
    for (const auto& pair : normalizedInput.pairs) if (!pair.requiresReview) {
        mappedObjects.insert(objectKey(pair.first.object)); mappedObjects.insert(objectKey(pair.second.object));
    }
    for (const auto& remap : vertices) {
        const auto key = objectKey(remap.object);
        if (!mappedObjects.contains(key) || objectCount(after, remap.object) == 0) continue;
        if (vertexMaps.contains(key) || !describe(remap.object) ||
            !partialMapping(remap.oldToNew, oldEndpoints[key].vertexCount, newEndpoints[key].vertexCount))
            return fail(error, QStringLiteral("顶点迁移身份无效、重复或不是存活点映射"));
        vertexMaps.insert(key, remap.oldToNew);
    }
    for (const auto& remap : axes) {
        const auto key = objectKey(remap.object);
        if (!mappedObjects.contains(key) || objectCount(after, remap.object) == 0) continue;
        if (!describe(remap.object)) return false;
        const auto& old = oldEndpoints[key]; const auto& next = newEndpoints[key];
        if (remap.oldAxis < 0 || remap.oldAxis >= old.axes.size() || remap.newAxis < -1 || remap.newAxis >= next.axes.size() ||
            axisMaps[key].contains(remap.oldAxis) || (remap.newAxis >= 0 && mappedAxes[key].contains(remap.newAxis)) ||
            !partialMapping(remap.oldKeyToNewKey, old.axes[remap.oldAxis].keys.size(), remap.newAxis < 0 ? 0 : next.axes[remap.newAxis].keys.size()))
            return fail(error, QStringLiteral("参数轴迁移身份无效、重复或不是存活关键值映射"));
        axisMaps[key].insert(remap.oldAxis, remap); if (remap.newAxis >= 0) mappedAxes[key].insert(remap.newAxis);
    }
    State candidate; candidate.sourceDigest = normalizedInput.sourceDigest;
    for (const auto& pair : normalizedInput.pairs) {
        const int firstCount = objectCount(after, pair.first.object), secondCount = objectCount(after, pair.second.object);
        if (firstCount > 1 || secondCount > 1) return fail(error, QStringLiteral("几何配对迁移后的对象标识重复"));
        if (firstCount == 0 || secondCount == 0) continue;
        auto migrated = pair;
        if (pair.requiresReview) { candidate.pairs.append(migrated); continue; }
        Endpoint first, second;
        if (!DescribeEndpoint(after, pair.first.object, first) || !DescribeEndpoint(after, pair.second.object, second)) {
            migrated.requiresReview = true; candidate.pairs.append(std::move(migrated)); continue;
        }
        const auto firstKey = objectKey(pair.first.object), secondKey = objectKey(pair.second.object);
        const auto vertexRemap = [&](const Endpoint& old, const Endpoint& next, const QString& key, QVector<int>& mapping) {
            const auto found = vertexMaps.constFind(key);
            if (found != vertexMaps.cend()) { mapping = *found; return true; }
            if (!sameGeometry(old, next)) return false;
            mapping = identity(old.vertexCount); return true;
        };
        QVector<int> firstVertices, secondVertices;
        bool valid = first.vertexCount == second.vertexCount &&
            vertexRemap(pair.first, first, firstKey, firstVertices) && vertexRemap(pair.second, second, secondKey, secondVertices) &&
            compose(pair.firstVertexToSecondVertex, firstVertices, secondVertices, first.vertexCount, migrated.firstVertexToSecondVertex);
        const auto axisRemap = [&](const Endpoint& old, const Endpoint& next, const QString& key, int index, AxisRemap& mapping) {
            const auto ownerMaps = axisMaps.constFind(key);
            if (ownerMaps != axisMaps.cend()) {
                const auto item = ownerMaps->constFind(index);
                if (item != ownerMaps->cend()) { mapping = *item; return true; }
            }
            if (index >= next.axes.size() || !(old.axes[index] == next.axes[index])) return false;
            mapping = {old.object, index, index, identity(old.axes[index].keys.size())}; return true;
        };
        migrated.axes.clear();
        for (const auto& mapping : pair.axes) {
            AxisRemap firstAxis, secondAxis;
            if (!valid || !axisRemap(pair.first, first, firstKey, mapping.firstAxis, firstAxis) ||
                !axisRemap(pair.second, second, secondKey, mapping.secondAxis, secondAxis)) { valid = false; break; }
            if (firstAxis.newAxis == -1 && secondAxis.newAxis == -1) continue;
            if (firstAxis.newAxis == -1 || secondAxis.newAxis == -1) { valid = false; break; }
            AxisMapping next{firstAxis.newAxis, secondAxis.newAxis, {}};
            if (first.axes[next.firstAxis].keys.size() != second.axes[next.secondAxis].keys.size() ||
                !compose(mapping.firstKeyToSecondKey, firstAxis.oldKeyToNewKey, secondAxis.oldKeyToNewKey,
                         first.axes[next.firstAxis].keys.size(), next.firstKeyToSecondKey)) { valid = false; break; }
            migrated.axes.append(std::move(next));
        }
        migrated.first = std::move(first); migrated.second = std::move(second);
        qint64 budget = 0;
        if (!valid || !pairValid(migrated, budget, nullptr)) { migrated = pair; migrated.requiresReview = true; }
        candidate.pairs.append(std::move(migrated));
    }
    State normalized;
    if (!Normalize(candidate, normalized, error) || !Validate(normalized, after, error)) return false;
    result = std::move(normalized); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法迁移几何配对，原状态保持不变")); }

bool CGeometryObjectPairs::Reanchor(const State& state, const CModelDocument& before, const CModelDocument& after,
                                   State& result, QString* error) try
{
    State candidate;
    if (!Normalize(state, candidate, error) || !Validate(candidate, before, error) || !after.IsLoaded())
        return fail(error, QStringLiteral("无法重新锚定几何配对来源"));
    for (const auto& pair : candidate.pairs) {
        Endpoint oldFirst, oldSecond, first, second;
        if (!DescribeEndpoint(before, pair.first.object, oldFirst, error) || !DescribeEndpoint(before, pair.second.object, oldSecond, error) ||
            !DescribeEndpoint(after, pair.first.object, first, error) || !DescribeEndpoint(after, pair.second.object, second, error) ||
            !(oldFirst == first) || !(oldSecond == second)) return fail(error, QStringLiteral("导出模型的几何配对目录或结构不一致"));
    }
    candidate.sourceDigest = state.sourceDigest.isEmpty() && state.pairs.isEmpty() ? QByteArray{} : after.SourceDigest();
    if (!Validate(candidate, after, error)) return false;
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法重新锚定几何配对")); }

bool CGeometryObjectPairs::ToJson(const State& state, QJsonObject& result, QString* error) try
{
    State normalized; if (!Normalize(state, normalized, error)) return false;
    QJsonArray pairs;
    for (const auto& pair : normalized.pairs) {
        QJsonArray axes;
        for (const auto& mapping : pair.axes) axes.append(QJsonObject{{"firstAxis", mapping.firstAxis}, {"secondAxis", mapping.secondAxis},
                                                                    {"keys", intArray(mapping.firstKeyToSecondKey)}});
        pairs.append(QJsonObject{{"id", pair.id}, {"first", endpointJson(pair.first)}, {"second", endpointJson(pair.second)},
            {"vertices", intArray(pair.firstVertexToSecondVertex)}, {"axes", axes},
            {"linePoint", QJsonArray{pair.linePoint.x(), pair.linePoint.y()}}, {"lineDirection", QJsonArray{pair.lineDirection.x(), pair.lineDirection.y()}},
            {"rotationLocalX", QJsonArray{pair.rotationLocalX.x(), pair.rotationLocalX.y()}},
            {"rotationLocalY", QJsonArray{pair.rotationLocalY.x(), pair.rotationLocalY.y()}},
            {"reversedWinding", pair.reversedWinding}, {"enabled", pair.enabled}, {"requiresReview", pair.requiresReview}});
    }
    QJsonObject candidate{{"version", 1}, {"sourceDigest", QString::fromLatin1(normalized.sourceDigest.toHex())}, {"pairs", pairs}};
    if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > MaximumBytes)
        return fail(error, QStringLiteral("几何配对 JSON 超过大小限制"));
    result = std::move(candidate); if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法序列化几何配对")); }

bool CGeometryObjectPairs::FromJson(const QJsonObject& object, State& result, QString* error) try
{
    int version = 0;
    if (QJsonDocument(object).toJson(QJsonDocument::Compact).size() > MaximumBytes || !fields(object, {"version", "sourceDigest", "pairs"}) ||
        !integer(object["version"], version, 1, 1) || !object["sourceDigest"].isString() || !object["pairs"].isArray())
        return fail(error, QStringLiteral("几何配对 JSON 版本、大小或字段无效"));
    const auto digest = object["sourceDigest"].toString();
    if ((!digest.isEmpty() && digest.size() != 64) || !std::all_of(digest.cbegin(), digest.cend(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    })) return fail(error, QStringLiteral("几何配对源摘要无效"));
    State candidate; candidate.sourceDigest = QByteArray::fromHex(digest.toLatin1()); const auto rows = object["pairs"].toArray();
    if (rows.size() > MaximumPairs) return fail(error, QStringLiteral("几何配对数量超过限制"));
    qint64 budget = 0;
    for (const auto row : rows) {
        if (!row.isObject()) return fail(error, QStringLiteral("几何配对记录无效"));
        const auto item = row.toObject(); Pair pair;
        if (!fields(item, {"id", "first", "second", "vertices", "axes", "linePoint", "lineDirection", "reversedWinding", "enabled", "requiresReview", "rotationLocalX", "rotationLocalY"}) ||
            !item["id"].isString() || !item["enabled"].isBool() || !item["requiresReview"].isBool() || !item["reversedWinding"].isBool() ||
            !item["axes"].isArray() || !readEndpoint(item["first"], pair.first, budget) || !readEndpoint(item["second"], pair.second, budget) ||
            !readInts(item["vertices"], pair.firstVertexToSecondVertex, MaximumVertices - 1, budget) ||
            !readPoint(item["linePoint"], pair.linePoint) || !readPoint(item["lineDirection"], pair.lineDirection) ||
            !readPoint(item["rotationLocalX"], pair.rotationLocalX) || !readPoint(item["rotationLocalY"], pair.rotationLocalY))
            return fail(error, QStringLiteral("几何配对字段或映射无效"));
        pair.id = item["id"].toString(); pair.enabled = item["enabled"].toBool(); pair.requiresReview = item["requiresReview"].toBool();
        pair.reversedWinding = item["reversedWinding"].toBool(); const auto axes = item["axes"].toArray(); budget += axes.size();
        if (budget > MaximumMappings) return fail(error, QStringLiteral("几何配对参数轴超过限制"));
        for (const auto mapping : axes) {
            if (!mapping.isObject()) return fail(error, QStringLiteral("几何配对参数轴映射无效"));
            const auto axis = mapping.toObject(); AxisMapping value;
            if (!fields(axis, {"firstAxis", "secondAxis", "keys"}) ||
                !integer(axis["firstAxis"], value.firstAxis, 0, MaximumMappings) || !integer(axis["secondAxis"], value.secondAxis, 0, MaximumMappings) ||
                !readInts(axis["keys"], value.firstKeyToSecondKey, MaximumMappings - 1, budget))
                return fail(error, QStringLiteral("几何配对参数轴映射无效"));
            pair.axes.append(std::move(value));
        }
        candidate.pairs.append(std::move(pair));
    }
    return Normalize(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取几何配对 JSON")); }
}
