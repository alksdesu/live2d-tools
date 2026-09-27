#include "cvertexselection.h"
#include "opengl/cuvgeometry.h"
#include "std/cexportcontrol.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace live2dstruct {
namespace {
using API = CVertexSelection;
constexpr int MaximumVertices = 1000000;
constexpr int MaximumEdges = 3000000;
bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}
bool finite(QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y()) && std::abs(point.x()) <= 1e100
        && std::abs(point.y()) <= 1e100;
}
bool same(QPointF a, QPointF b)
{
    return a.x() == b.x() && a.y() == b.y();
}
bool combineValid(API::Combine combine)
{
    return combine == API::Combine::Replace || combine == API::Combine::Add || combine == API::Combine::Subtract;
}
bool validSoft(const API::Soft& soft)
{
    return std::isfinite(soft.radius) && soft.radius >= 0 && soft.radius <= 1000000
        && (soft.distance == API::Distance::AlongEdges || soft.distance == API::Distance::ConnectedEuclidean
            || soft.distance == API::Distance::AllEuclidean)
        && (soft.falloff == API::Falloff::Linear || soft.falloff == API::Falloff::Smooth);
}
bool validBrush(const API::Brush& brush)
{
    return std::isfinite(brush.radius) && brush.radius > 0 && brush.radius <= 1000000 && std::isfinite(brush.strength)
        && brush.strength >= 0 && brush.strength <= 1 && std::isfinite(brush.hardness) && brush.hardness >= 0
        && brush.hardness <= 1;
}
bool validLimits(const API::Limits& limits)
{
    return limits.maximumVertices >= 0 && limits.maximumVertices <= MaximumVertices && limits.maximumEdges >= 0
        && limits.maximumEdges <= MaximumEdges && limits.maximumPathPoints >= 0 && limits.maximumPathPoints <= 65536
        && limits.maximumStrokeSegments >= 0 && limits.maximumStrokeSegments <= 65536 && limits.maximumOperations > 0
        && limits.maximumOperations <= 512LL * 1024 * 1024 && limits.maximumBytes > 0
        && limits.maximumBytes <= 512LL * 1024 * 1024;
}
API::State owned(const API::State& state)
{
    API::State result;
    result.hard.reserve(state.hard.size());
    for (int index : state.hard)
        result.hard.append(index);
    if (state.painted) {
        result.painted.emplace();
        result.painted->reserve(state.painted->size());
        for (double weight : *state.painted)
            result.painted->append(weight);
    }
    return result;
}
bool freeze(const API::Geometry& geometry, const API::State& state, const API::Limits& limits, API::Geometry& result,
    API::State& selection, QString* error)
{
    if (!validLimits(limits) || geometry.points.size() > limits.maximumVertices
        || geometry.edges.size() > limits.maximumEdges
        || geometry.points.size() * 160LL + geometry.edges.size() * 128LL > limits.maximumBytes)
        return fail(error, QStringLiteral("顶点选择的输入超过资源预算"));
    if (!API::Validate(state, geometry.points.size(), error))
        return false;
    API::Geometry candidate;
    candidate.points.reserve(geometry.points.size());
    candidate.edges.reserve(geometry.edges.size());
    for (auto point : geometry.points) {
        if (!finite(point))
            return fail(error, QStringLiteral("选择坐标超出有限范围"));
        candidate.points.append(point);
    }
    for (auto edge : geometry.edges) {
        if (edge.a < 0 || edge.b < 0 || edge.a >= geometry.points.size() || edge.b >= geometry.points.size())
            return fail(error, QStringLiteral("选择邻接边索引越界"));
        if (edge.a == edge.b)
            continue;
        if (edge.a > edge.b)
            std::swap(edge.a, edge.b);
        candidate.edges.append(edge);
    }
    auto selected = owned(state);
    result = std::move(candidate);
    selection = std::move(selected);
    return true;
}
struct Rejected
{
};
class Work
{
public:
    Work(const API::Limits& limits, CExportControl* control, QString* error, qint64 initial = 0)
        : limits(limits)
        , control(control)
        , error(error)
        , operations(initial)
        , checks(initial)
    {
    }
    void tick(qint64 count = 1)
    {
        if (count < 0 || count > limits.maximumOperations - operations)
            reject(QStringLiteral("选择计算超过工作量限制"));
        operations += count;
        check();
    }
    void check()
    {
        if (++checks > limits.maximumOperations)
            reject(QStringLiteral("选择计算超过比较工作量限制"));
        if (control && !control->Checkpoint())
            reject(control->Error().isEmpty() ? QStringLiteral("选择操作已取消") : control->Error());
    }
    void report(const QString& detail)
    {
        tick();
        if (control && !control->Report(CExportControl::Phase::Preparing, operations, limits.maximumOperations, detail))
            reject(control->Error().isEmpty() ? QStringLiteral("选择操作已取消") : control->Error());
    }
    [[noreturn]] void reject(const QString& message)
    {
        fail(error, message);
        throw Rejected { };
    }
    const API::Limits& limits;
    CExportControl* control;
    QString* error;
    qint64 operations = 0;
    qint64 checks = 0;
};
double falloff(double distance, double radius, API::Falloff curve)
{
    if (distance >= radius || radius <= 0)
        return 0;
    const double u = 1 - std::clamp(distance / radius, 0.0, 1.0);
    return curve == API::Falloff::Linear ? u : u * u * (3 - 2 * u);
}
using Neighbor = QPair<int, double>;
using Graph = QVector<QVector<Neighbor>>;
Graph graph(API::Geometry& geometry, Work& work)
{
    int levels = 0;
    for (qsizetype count = geometry.edges.size(); count > 1; count = (count + 1) / 2)
        ++levels;
    work.tick(geometry.edges.size() * qint64(levels + 1) * 2);
    std::sort(geometry.edges.begin(), geometry.edges.end(), [&](const auto& a, const auto& b) {
        work.check();
        return a.a != b.a ? a.a < b.a : a.b < b.b;
    });
    geometry.edges.erase(std::unique(geometry.edges.begin(), geometry.edges.end()), geometry.edges.end());
    Graph result(geometry.points.size());
    for (const auto& edge : geometry.edges) {
        work.tick();
        const auto a = geometry.points[edge.a], b = geometry.points[edge.b];
        const double length = std::hypot(a.x() - b.x(), a.y() - b.y());
        if (!std::isfinite(length))
            work.reject(QStringLiteral("邻接边长度超出有限范围"));
        result[edge.a].append({ edge.b, length });
        result[edge.b].append({ edge.a, length });
    }
    return result;
}
class NearestSeeds
{
public:
    NearestSeeds(const QVector<QPointF>& points, QVector<int> seeds, Work& work)
        : points(points)
        , work(work)
    {
        nodes.reserve(seeds.size());
        root = build(seeds, 0, seeds.size(), 0);
    }
    double distance(QPointF point, double radius)
    {
        double result = radius;
        QVector<int> pending;
        if (root >= 0)
            pending.append(root);
        while (!pending.isEmpty()) {
            work.tick();
            const auto& node = nodes[pending.takeLast()];
            const double dx = point.x() < node.left ? node.left - point.x()
                : point.x() > node.right            ? point.x() - node.right
                                                    : 0;
            const double dy = point.y() < node.top ? node.top - point.y()
                : point.y() > node.bottom          ? point.y() - node.bottom
                                                   : 0;
            if (std::hypot(dx, dy) >= result)
                continue;
            const auto seed = points[node.vertex];
            result = std::min(result, std::hypot(point.x() - seed.x(), point.y() - seed.y()));
            if (node.second >= 0)
                pending.append(node.second);
            if (node.first >= 0)
                pending.append(node.first);
        }
        return result;
    }

private:
    struct Node
    {
        int vertex, first = -1, second = -1;
        double left, right, top, bottom;
    };
    int build(QVector<int>& seeds, int begin, int end, int axis)
    {
        if (begin == end)
            return -1;
        work.tick(qint64(end - begin) * 2);
        const int middle = begin + (end - begin) / 2;
        std::nth_element(seeds.begin() + begin, seeds.begin() + middle, seeds.begin() + end, [&](int a, int b) {
            work.check();
            const auto x = points[a], y = points[b];
            const double primaryX = axis ? x.y() : x.x(), primaryY = axis ? y.y() : y.x();
            if (primaryX != primaryY)
                return primaryX < primaryY;
            const double secondaryX = axis ? x.x() : x.y(), secondaryY = axis ? y.x() : y.y();
            return secondaryX != secondaryY ? secondaryX < secondaryY : a < b;
        });
        const auto point = points[seeds[middle]];
        const int index = nodes.size();
        nodes.append({ seeds[middle], -1, -1, point.x(), point.x(), point.y(), point.y() });
        const int first = build(seeds, begin, middle, 1 - axis), second = build(seeds, middle + 1, end, 1 - axis);
        auto& node = nodes[index];
        node.first = first;
        node.second = second;
        for (int child : { first, second })
            if (child >= 0) {
                const auto& box = nodes[child];
                node.left = std::min(node.left, box.left);
                node.right = std::max(node.right, box.right);
                node.top = std::min(node.top, box.top);
                node.bottom = std::max(node.bottom, box.bottom);
            }
        return index;
    }
    const QVector<QPointF>& points;
    Work& work;
    QVector<Node> nodes;
    int root = -1;
};
API::Influence influence(API::Geometry& geometry, const API::State& state, const API::Soft& settings, Work& work)
{
    API::Influence result;
    result.weights.fill(0, geometry.points.size());
    if (state.painted) {
        result.source = API::Source::Painted;
        for (int i = 0; i < state.painted->size(); ++i) {
            work.tick();
            result.weights[i] = state.painted->at(i);
        }
    } else {
        result.source = settings.enabled ? API::Source::Distance : API::Source::Hard;
        for (int vertex : state.hard)
            result.weights[vertex] = 1;
        if (settings.enabled && settings.radius > 0 && !state.hard.isEmpty()
            && state.hard.size() < geometry.points.size()) {
            if (settings.distance == API::Distance::AlongEdges) {
                const auto adjacency = graph(geometry, work);
                QVector<double> distances(geometry.points.size(), std::numeric_limits<double>::infinity());
                using Entry = std::pair<double, int>;
                std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
                for (int vertex : state.hard) {
                    distances[vertex] = 0;
                    queue.emplace(0, vertex);
                }
                while (!queue.empty()) {
                    work.tick();
                    const auto [distance, vertex] = queue.top();
                    queue.pop();
                    if (distance != distances[vertex])
                        continue;
                    if (distance >= settings.radius)
                        break;
                    for (const auto& next : adjacency[vertex]) {
                        work.tick();
                        const double candidate = distance + next.second;
                        if (candidate < settings.radius && candidate < distances[next.first]) {
                            distances[next.first] = candidate;
                            queue.emplace(candidate, next.first);
                        }
                    }
                }
                for (int i = 0; i < distances.size(); ++i) {
                    work.tick();
                    if (result.weights[i] != 1)
                        result.weights[i] = falloff(distances[i], settings.radius, settings.falloff);
                }
            } else {
                QVector<int> component(geometry.points.size(), 0);
                QVector<QVector<int>> seeds;
                if (settings.distance == API::Distance::ConnectedEuclidean) {
                    const auto adjacency = graph(geometry, work);
                    component.fill(-1);
                    int nextId = 0;
                    for (int root = 0; root < component.size(); ++root)
                        if (component[root] < 0) {
                            QVector<int> pending { root };
                            component[root] = nextId;
                            while (!pending.isEmpty()) {
                                work.tick();
                                const int vertex = pending.takeLast();
                                for (const auto& next : adjacency[vertex]) {
                                    work.tick();
                                    if (component[next.first] < 0) {
                                        component[next.first] = nextId;
                                        pending.append(next.first);
                                    }
                                }
                            }
                            ++nextId;
                        }
                    seeds.resize(nextId);
                } else
                    seeds.resize(1);
                for (int vertex : state.hard)
                    seeds[component[vertex]].append(vertex);
                QVector<QVector<int>> members(seeds.size());
                for (int vertex = 0; vertex < geometry.points.size(); ++vertex)
                    if (result.weights[vertex] != 1)
                        members[component[vertex]].append(vertex);
                for (int group = 0; group < seeds.size(); ++group) {
                    work.tick();
                    if (seeds[group].isEmpty() || members[group].isEmpty())
                        continue;
                    NearestSeeds nearest(geometry.points, std::move(seeds[group]), work);
                    for (int vertex : members[group]) {
                        work.tick();
                        const double distance = nearest.distance(geometry.points[vertex], settings.radius);
                        result.weights[vertex] = falloff(distance, settings.radius, settings.falloff);
                    }
                }
            }
        }
    }
    for (int index = 0; index < result.weights.size(); ++index) {
        work.tick();
        if (result.weights[index] > 0)
            result.affected.append(index);
    }
    result.operations = work.operations;
    return result;
}
double segmentDistance(QPointF point, QPointF first, QPointF last)
{
    if (same(first, last))
        return std::hypot(point.x() - first.x(), point.y() - first.y());
    const auto edge = last - first, delta = point - first;
    const double length = std::hypot(edge.x(), edge.y());
    const QPointF unit(edge.x() / length, edge.y() / length);
    const double along = delta.x() * unit.x() + delta.y() * unit.y();
    if (along <= 0)
        return std::hypot(delta.x(), delta.y());
    if (along >= length)
        return std::hypot(point.x() - last.x(), point.y() - last.y());
    return std::abs(delta.x() * unit.y() - delta.y() * unit.x());
}
double coverage(double distance, const API::Brush& brush)
{
    if (brush.hardness == 1)
        return distance <= brush.radius ? 1 : 0;
    const double t = distance / brush.radius;
    if (t >= 1)
        return 0;
    if (t <= brush.hardness)
        return 1;
    const double u = (1 - t) / (1 - brush.hardness);
    return u * u * (3 - 2 * u);
}
bool fields(const QJsonObject& object, const QStringList& names)
{
    if (object.size() != names.size())
        return false;
    for (const auto& name : names)
        if (!object.contains(name))
            return false;
    return true;
}
bool integer(const QJsonValue& value, int& result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > 10 || std::floor(number) != number)
        return false;
    result = int(number);
    return true;
}
}

struct CVertexSelection::Stroke::Data
{
    QVector<QPointF> points;
    State before, current;
    QVector<double> baseline, maximumCoverage;
    Brush brush;
    Combine combine = Combine::Replace;
    QPointF last;
    Limits limits;
    int segments = 0;
    qint64 operations = 0;
    void update(QPointF point, Work& work)
    {
        for (int index = 0; index < points.size(); ++index) {
            work.tick();
            maximumCoverage[index]
                = std::max(maximumCoverage[index], coverage(segmentDistance(points[index], last, point), brush));
        }
        State candidate;
        candidate.painted.emplace();
        candidate.painted->reserve(points.size());
        bool changed = false;
        for (int index = 0; index < points.size(); ++index) {
            work.tick();
            const double amount = brush.strength * maximumCoverage[index];
            const double weight = combine == Combine::Replace
                ? amount
                : std::clamp(baseline[index] + (combine == Combine::Add ? amount : -amount), 0.0, 1.0);
            candidate.painted->append(weight);
            changed |= weight != baseline[index];
        }
        current = changed && brush.strength != 0 ? std::move(candidate) : owned(before);
        last = point;
        operations = work.operations;
    }
};

bool CVertexSelection::Soft::operator==(const Soft& other) const
{
    return enabled == other.enabled && radius == other.radius && distance == other.distance && falloff == other.falloff;
}
bool CVertexSelection::Brush::operator==(const Brush& other) const
{
    return radius == other.radius && strength == other.strength && hardness == other.hardness;
}
bool CVertexSelection::Settings::operator==(const Settings& other) const
{
    return tool == other.tool && combine == other.combine && soft == other.soft && brush == other.brush;
}
bool CVertexSelection::State::operator==(const State& other) const
{
    return hard == other.hard && painted == other.painted;
}

bool CVertexSelection::Validate(const State& state, int vertexCount, QString* error)
{
    if (vertexCount < 0 || vertexCount > MaximumVertices || state.hard.size() > vertexCount
        || (state.painted && (!state.hard.isEmpty() || state.painted->size() != vertexCount)))
        return fail(error, QStringLiteral("硬选区与刷选权重的模式或数量不匹配"));
    QVector<quint8> seen(vertexCount, 0);
    for (int vertex : state.hard) {
        if (vertex < 0 || vertex >= vertexCount || seen[vertex])
            return fail(error, QStringLiteral("硬选顶点索引无效或重复"));
        seen[vertex] = 1;
    }
    if (state.painted)
        for (double weight : *state.painted)
            if (!std::isfinite(weight) || weight < 0 || weight > 1)
                return fail(error, QStringLiteral("刷选权重必须是 0 至 1 的有限值"));
    if (error)
        error->clear();
    return true;
}
bool CVertexSelection::ValidateSettings(const Settings& settings, QString* error)
{
    if ((settings.tool != Tool::Move && settings.tool != Tool::Rectangle && settings.tool != Tool::Lasso
            && settings.tool != Tool::Brush)
        || !combineValid(settings.combine) || !validSoft(settings.soft) || !validBrush(settings.brush))
        return fail(error, QStringLiteral("选择工具、距离衰减或笔刷设置无效"));
    if (error)
        error->clear();
    return true;
}
bool CVertexSelection::Remap(
    const State& state, int oldCount, int newCount, const QVector<int>& oldToNew, State& result, QString* error)
try {
    if (!Validate(state, oldCount, error))
        return false;
    if (newCount < 0 || newCount > MaximumVertices || oldToNew.size() != oldCount)
        return fail(error, QStringLiteral("选区迁移的顶点数量不匹配"));
    QVector<quint8> seen(newCount, 0);
    for (int index : oldToNew) {
        if (index < -1 || index >= newCount || (index >= 0 && seen[index]))
            return fail(error, QStringLiteral("选区顶点身份映射越界或重复"));
        if (index >= 0)
            seen[index] = 1;
    }
    State candidate;
    if (state.painted) {
        candidate.painted = QVector<double>(newCount, 0);
        for (int old = 0; old < oldCount; ++old)
            if (oldToNew[old] >= 0)
                (*candidate.painted)[oldToNew[old]] = state.painted->at(old);
    } else {
        for (int old : state.hard)
            if (oldToNew[old] >= 0)
                candidate.hard.append(oldToNew[old]);
        std::sort(candidate.hard.begin(), candidate.hard.end());
    }
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法迁移顶点选区，原结果保持不变"));
}

bool CVertexSelection::SelectVertices(
    int vertexCount, const QVector<int>& hits, const State& state, Combine combine, State& result, QString* error)
try {
    if (!Validate(state, vertexCount, error))
        return false;
    if (!combineValid(combine) || hits.size() > vertexCount)
        return fail(error, QStringLiteral("硬选区组合请求无效"));
    QVector<quint8> selected(vertexCount, 0), seen(vertexCount, 0);
    if (combine != Combine::Replace)
        for (int index : state.hard)
            selected[index] = 1;
    for (int index : hits) {
        if (index < 0 || index >= vertexCount || seen[index])
            return fail(error, QStringLiteral("命中顶点索引无效或重复"));
        seen[index] = 1;
        selected[index] = combine != Combine::Subtract;
    }
    State candidate;
    for (int index = 0; index < vertexCount; ++index)
        if (selected[index])
            candidate.hard.append(index);
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法更新硬选区，原结果保持不变"));
}

bool CVertexSelection::EdgesFromTriangles(
    int vertexCount, const QVector<quint16>& triangles, QVector<Edge>& result, QString* error)
try {
    if (vertexCount < 0 || vertexCount > MaximumVertices || triangles.size() % 3 || triangles.size() > MaximumEdges)
        return fail(error, QStringLiteral("选择邻接的三角数组无效"));
    QVector<Edge> candidate;
    candidate.reserve(triangles.size());
    for (int first = 0; first < triangles.size(); first += 3)
        for (int side = 0; side < 3; ++side) {
            int a = triangles[first + side], b = triangles[first + (side + 1) % 3];
            if (a >= vertexCount || b >= vertexCount)
                return fail(error, QStringLiteral("三角顶点索引越界"));
            if (a == b)
                continue;
            if (a > b)
                std::swap(a, b);
            candidate.append({ a, b });
        }
    std::sort(candidate.begin(), candidate.end(),
        [](const auto& a, const auto& b) { return a.a != b.a ? a.a < b.a : a.b < b.b; });
    candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法创建选择邻接，原结果保持不变"));
}

bool CVertexSelection::SelectPolygon(const QVector<QPointF>& points, const QVector<QPointF>& polygon,
    const State& state, Combine combine, State& result, QString* error, CExportControl* control)
{
    return SelectPolygon(points, polygon, state, combine, result, Limits { }, error, control);
}
bool CVertexSelection::SelectPolygon(const QVector<QPointF>& points, const QVector<QPointF>& polygon,
    const State& state, Combine combine, State& result, const Limits& requested, QString* error,
    CExportControl* control)
try {
    const auto limits = requested;
    if (!validLimits(limits) || polygon.size() > limits.maximumPathPoints
        || polygon.size() * 32LL + points.size() * 160LL > limits.maximumBytes || !combineValid(combine))
        return fail(error, QStringLiteral("套索路径或计算预算无效"));
    Geometry geometry;
    State before;
    if (!freeze({ points, { } }, state, limits, geometry, before, error))
        return false;
    QVector<QPointF> path;
    path.reserve(polygon.size());
    for (auto point : polygon) {
        if (!finite(point))
            return fail(error, QStringLiteral("套索路径包含非有限坐标"));
        if (path.isEmpty() || !same(path.last(), point))
            path.append(point);
    }
    if (path.size() > 1 && same(path.first(), path.last()))
        path.removeLast();
    if (path.size() < 3)
        return fail(error, QStringLiteral("套索需要至少三个不共线的点"));
    Work work(limits, control, error);
    work.report(QStringLiteral("计算套索选区"));
    bool area = false;
    for (int index = 2; index < path.size(); ++index) {
        work.tick();
        area |= CUvGeometry::Orientation(path[0], path[1], path[index]) != 0;
    }
    if (!area)
        return fail(error, QStringLiteral("套索路径共线，未形成区域"));
    QVector<int> hits;
    for (int index = 0; index < geometry.points.size(); ++index) {
        const auto point = geometry.points[index];
        bool inside = false, boundary = false;
        for (int first = 0, last = path.size() - 1; first < path.size(); last = first++) {
            work.tick();
            const auto a = path[last], b = path[first];
            const int side = CUvGeometry::Orientation(a, b, point);
            if (!side && point.x() >= std::min(a.x(), b.x()) && point.x() <= std::max(a.x(), b.x())
                && point.y() >= std::min(a.y(), b.y()) && point.y() <= std::max(a.y(), b.y())) {
                boundary = true;
                break;
            }
            if ((a.y() > point.y()) != (b.y() > point.y()) && ((b.y() > a.y()) ? side > 0 : side < 0))
                inside = !inside;
        }
        if (inside || boundary)
            hits.append(index);
    }
    return SelectVertices(geometry.points.size(), hits, before, combine, result, error);
} catch (const Rejected&) {
    return false;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法计算套索，原选区保持不变"));
}

bool CVertexSelection::ComputeInfluence(const Geometry& geometry, const State& state, const Soft& settings,
    Influence& result, QString* error, CExportControl* control)
{
    return ComputeInfluence(geometry, state, settings, result, Limits { }, error, control);
}
bool CVertexSelection::ComputeInfluence(const Geometry& geometry, const State& state, const Soft& settings,
    Influence& result, const Limits& requested, QString* error, CExportControl* control)
try {
    const auto limits = requested;
    const auto soft = settings;
    if (!validSoft(soft))
        return fail(error, QStringLiteral("距离衰减设置无效"));
    Geometry frozen;
    State before;
    if (!freeze(geometry, state, limits, frozen, before, error))
        return false;
    Work work(limits, control, error);
    work.report(QStringLiteral("计算顶点影响权重"));
    auto candidate = influence(frozen, before, soft, work);
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const Rejected&) {
    return false;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法计算顶点权重，原结果保持不变"));
}

bool CVertexSelection::Translate(const QVector<float>& baseline, const QVector<double>& weights, QPointF delta,
    QVector<float>& result, QString* error)
try {
    if (baseline.size() % 2 || baseline.size() > MaximumVertices * 2 || weights.size() != baseline.size() / 2
        || !std::isfinite(delta.x()) || !std::isfinite(delta.y()))
        return fail(error, QStringLiteral("加权位移的坐标或权重数量无效"));
    QVector<float> candidate;
    candidate.reserve(baseline.size());
    for (float value : baseline) {
        if (!std::isfinite(value))
            return fail(error, QStringLiteral("起始顶点坐标非有限"));
        candidate.append(value);
    }
    for (int vertex = 0; vertex < weights.size(); ++vertex) {
        const double weight = weights[vertex];
        if (!std::isfinite(weight) || weight < 0 || weight > 1)
            return fail(error, QStringLiteral("顶点影响权重无效"));
        if (weight == 0)
            continue;
        for (int axis = 0; axis < 2; ++axis) {
            const double move = axis ? delta.y() : delta.x();
            if (move == 0)
                continue;
            const double value = double(baseline[vertex * 2 + axis]) + weight * move;
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                return fail(error, QStringLiteral("加权顶点位移超出 float 范围"));
            candidate[vertex * 2 + axis] = float(value);
        }
    }
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法计算加权位移，原结果保持不变"));
}

bool CVertexSelection::BeginBrush(const Geometry& geometry, const State& state, const Soft& soft, const Brush& brush,
    Combine combine, QPointF start, Stroke& result, State& selection, QString* error, CExportControl* control)
{
    return BeginBrush(geometry, state, soft, brush, combine, start, result, selection, Limits { }, error, control);
}
bool CVertexSelection::BeginBrush(const Geometry& geometry, const State& state, const Soft& settings,
    const Brush& brush, Combine combine, QPointF start, Stroke& result, State& selection, const Limits& requested,
    QString* error, CExportControl* control)
try {
    const auto limits = requested;
    const auto frozenBrush = brush;
    const auto soft = settings;
    if (!validBrush(frozenBrush) || !validSoft(soft) || !combineValid(combine) || !finite(start))
        return fail(error, QStringLiteral("笔触设置或起点无效"));
    Geometry frozen;
    State before;
    if (!freeze(geometry, state, limits, frozen, before, error))
        return false;
    Work work(limits, control, error);
    work.report(QStringLiteral("开始权重笔触"));
    auto effective = influence(frozen, before, soft, work);
    auto candidate = std::make_shared<Stroke::Data>();
    candidate->points = std::move(frozen.points);
    candidate->before = std::move(before);
    candidate->baseline = std::move(effective.weights);
    candidate->maximumCoverage.fill(0, candidate->points.size());
    candidate->brush = frozenBrush;
    candidate->combine = combine;
    candidate->last = start;
    candidate->limits = limits;
    candidate->update(start, work);
    auto output = owned(candidate->current);
    Stroke next;
    next.m_data = std::move(candidate);
    selection = std::move(output);
    result = std::move(next);
    if (error)
        error->clear();
    return true;
} catch (const Rejected&) {
    return false;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法开始权重笔触，原结果保持不变"));
}

bool CVertexSelection::AppendBrush(
    const Stroke& stroke, QPointF point, Stroke& result, State& selection, QString* error, CExportControl* control)
try {
    if (!stroke.m_data || !finite(point))
        return fail(error, QStringLiteral("笔触或坐标无效"));
    const auto source = stroke.m_data;
    if (!same(point, source->last) && source->segments >= source->limits.maximumStrokeSegments)
        return fail(error, QStringLiteral("笔触超过路径段数限制，请结束本笔后继续"));
    auto candidate = std::make_shared<Stroke::Data>(*source);
    Work work(candidate->limits, control, error, candidate->operations);
    if (!same(point, source->last))
        ++candidate->segments;
    candidate->update(point, work);
    auto output = owned(candidate->current);
    Stroke next;
    next.m_data = std::move(candidate);
    selection = std::move(output);
    result = std::move(next);
    if (error)
        error->clear();
    return true;
} catch (const Rejected&) {
    return false;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法更新权重笔触，原结果保持不变"));
}
bool CVertexSelection::BrushState(const Stroke& stroke, State& result, QString* error)
try {
    if (!stroke.m_data)
        return fail(error, QStringLiteral("权重笔触尚未开始"));
    auto candidate = owned(stroke.m_data->current);
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法读取笔触状态"));
}
bool CVertexSelection::CancelBrush(const Stroke& stroke, State& result, QString* error)
try {
    if (!stroke.m_data)
        return fail(error, QStringLiteral("权重笔触尚未开始"));
    auto candidate = owned(stroke.m_data->before);
    result = std::move(candidate);
    if (error)
        error->clear();
    return true;
} catch (const std::exception&) {
    return fail(error, QStringLiteral("无法恢复笔触起始状态"));
}

QJsonObject CVertexSelection::ToJson(const Settings& settings)
{
    if (!ValidateSettings(settings))
        return { };
    return { { "version", 1 }, { "tool", int(settings.tool) }, { "combine", int(settings.combine) },
        { "soft",
            QJsonObject { { "enabled", settings.soft.enabled }, { "radius", settings.soft.radius },
                { "distance", int(settings.soft.distance) }, { "falloff", int(settings.soft.falloff) } } },
        { "brush",
            QJsonObject { { "radius", settings.brush.radius }, { "strength", settings.brush.strength },
                { "hardness", settings.brush.hardness } } } };
}
bool CVertexSelection::FromJson(const QJsonObject& object, Settings& result, QString* error)
{
    if (!fields(object, { "version", "tool", "combine", "soft", "brush" }) || object["version"] != QJsonValue(1)
        || !object["soft"].isObject() || !object["brush"].isObject())
        return fail(error, QStringLiteral("选择工具设置格式或版本无效"));
    const auto soft = object["soft"].toObject(), brush = object["brush"].toObject();
    int tool, combine, distance, curve;
    if (!fields(soft, { "enabled", "radius", "distance", "falloff" })
        || !fields(brush, { "radius", "strength", "hardness" }) || !integer(object["tool"], tool)
        || !integer(object["combine"], combine) || !integer(soft["distance"], distance)
        || !integer(soft["falloff"], curve) || !soft["enabled"].isBool() || !soft["radius"].isDouble()
        || !brush["radius"].isDouble() || !brush["strength"].isDouble() || !brush["hardness"].isDouble())
        return fail(error, QStringLiteral("选择工具设置字段无效"));
    Settings candidate;
    candidate.tool = Tool(tool);
    candidate.combine = Combine(combine);
    candidate.soft = { soft["enabled"].toBool(), soft["radius"].toDouble(), Distance(distance), Falloff(curve) };
    candidate.brush = { brush["radius"].toDouble(), brush["strength"].toDouble(), brush["hardness"].toDouble() };
    if (!ValidateSettings(candidate, error))
        return false;
    result = candidate;
    return true;
}
}
