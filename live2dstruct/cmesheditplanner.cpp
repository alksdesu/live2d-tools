#include "cmesheditplanner.h"
#include "opengl/cuvgeometry.h"
#include <QMap>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace live2dstruct {
namespace {
using API = CMeshEditPlanner;
using Edge = API::Edge;
using Code = API::Code;
using Source = CMocMeshStructureBuilder::VertexSource;
using Key = quint64;
struct Rejected {};
Key key(int a, int b) { return (quint64(std::min(a, b)) << 32) | quint32(std::max(a, b)); }
Edge edge(Key value) { return {int(value >> 32), int(value & 0xffffffff)}; }
int orient(QPointF a, QPointF b, QPointF c) { return CUvGeometry::Orientation(a, b, c); }
bool samePoint(QPointF a, QPointF b) { return a.x() == b.x() && a.y() == b.y(); }
bool between(QPointF p, QPointF a, QPointF b)
{
    return p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) &&
           p.y() >= std::min(a.y(), b.y()) && p.y() <= std::max(a.y(), b.y());
}
bool onSegment(QPointF p, QPointF a, QPointF b) { return between(p, a, b) && orient(a, b, p) == 0; }
bool properCross(QPointF a, QPointF b, QPointF c, QPointF d)
{
    return orient(a, b, c) * orient(a, b, d) < 0 && orient(c, d, a) * orient(c, d, b) < 0;
}
bool intersects(QPointF a, QPointF b, QPointF c, QPointF d)
{
    return properCross(a, b, c, d) || onSegment(a, c, d) || onSegment(b, c, d) ||
           onSegment(c, a, b) || onSegment(d, a, b);
}

class Expansion
{
public:
    void add(double value)
    {
        std::vector<double> next;
        next.reserve(values.size() + 1);
        for (double component : values) {
            const double sum = value + component;
            const double shifted = sum - value;
            const double error = (value - (sum - shifted)) + (component - shifted);
            if (error != 0) next.push_back(error);
            value = sum;
        }
        if (value != 0) next.push_back(value);
        values.swap(next);
    }
    void product(double a, double b) { const double value = a * b; add(std::fma(a, b, -value)); add(value); }
    void multiply(const Expansion& a, const Expansion& b, double sign)
    {
        for (double first : a.values) for (double second : b.values) product(first * sign, second);
    }
    double estimate() const { double result = 0; for (double value : values) result += value; return result; }
    int sign() const { return values.empty() ? 0 : (values.back() > 0 ? 1 : -1); }
private:
    std::vector<double> values;
};
Expansion determinant(QPointF a, QPointF b, QPointF c)
{
    Expansion result;
    result.product(a.x(), b.y()); result.product(-a.y(), b.x());
    result.product(b.x(), c.y()); result.product(-b.y(), c.x());
    result.product(c.x(), a.y()); result.product(-c.y(), a.x());
    return result;
}
int inCircle(QPointF a, QPointF b, QPointF c, QPointF d)
{
    const double ax = a.x() - d.x(), ay = a.y() - d.y();
    const double bx = b.x() - d.x(), by = b.y() - d.y();
    const double cx = c.x() - d.x(), cy = c.y() - d.y();
    const double al = ax * ax + ay * ay, bl = bx * bx + by * by, cl = cx * cx + cy * cy;
    const std::array<double, 6> terms{al * bx * cy, -al * by * cx, -bl * ax * cy,
                                     bl * ay * cx, cl * ax * by, -cl * ay * bx};
    double value = 0, magnitude = 0;
    for (double term : terms) { value += term; magnitude += std::abs(term); }
    if (std::abs(value) > 128 * std::numeric_limits<double>::epsilon() * magnitude) return value > 0 ? 1 : -1;
    const auto lift = [](QPointF p) { Expansion result; result.product(p.x(), p.x()); result.product(p.y(), p.y()); return result; };
    Expansion result;
    result.multiply(lift(a), determinant(b, c, d), 1);
    result.multiply(lift(b), determinant(a, c, d), -1);
    result.multiply(lift(c), determinant(a, b, d), 1);
    result.multiply(lift(d), determinant(a, b, c), -1);
    return result.sign();
}
struct Face {
    std::array<int, 3> vertices;
    int source = -1;
    bool active = true;
};
struct Adjacent { int face; int opposite; int direction; };
using Adjacency = std::map<Key, QVector<Adjacent>>;
struct Box {
    double left, right, top, bottom;
    bool overlaps(const Box& other) const { return top <= other.bottom && other.top <= bottom && left <= other.right && other.left <= right; }
};
Box bounds(QPointF a, QPointF b)
{
    return {std::min(a.x(), b.x()), std::max(a.x(), b.x()), std::min(a.y(), b.y()), std::max(a.y(), b.y())};
}

class Planner
{
public:
    Planner(const API::Input& input, API::Diagnostic* diagnostic) : input(input), diagnostic(diagnostic)
    {
        if (input.artMeshId.isEmpty() || input.artMeshId.contains(QChar::Null) || input.positions.isEmpty() ||
            input.positions.size() % 2 || input.positions.size() / 2 > CMocMeshStructureBuilder::MaximumVertices ||
            input.indices.isEmpty() || input.indices.size() % 3 || input.indices.size() > 3000000 ||
            input.protectedEdges.size() > input.indices.size()) reject(Code::InvalidInput, "网格名称、顶点数或索引数组无效");
        for (qsizetype i = 0; i < input.positions.size(); i += 2) {
            const float x = input.positions[i], y = input.positions[i + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) reject(Code::NonFinite, "参考顶点包含非有限值", {int(i / 2)});
            points.append(QPointF(x, y)); sources.append(Source{{int(i / 2)}, {1}}); alive.append(true);
        }
        for (qsizetype i = 0; i < input.indices.size(); i += 3) {
            Face face{{input.indices[i], input.indices[i + 1], input.indices[i + 2]}, int(i / 3)};
            for (int vertex : face.vertices) if (vertex >= points.size()) reject(Code::InvalidInput, "三角索引越界", {vertex}, {int(i / 3)});
            faces.append(face);
        }
        for (const auto& constraint : input.protectedEdges) {
            if (constraint.a < 0 || constraint.b < 0 || constraint.a >= points.size() || constraint.b >= points.size() || constraint.a == constraint.b)
                reject(Code::InvalidConstraint, "保护边的顶点无效", {}, {}, {constraint});
            protectedEdges.insert(key(constraint.a, constraint.b));
        }
        validate();
    }

    [[noreturn]] void reject(Code code, const char* text, QVector<int> vertices = {}, QVector<int> triangles = {}, QVector<Edge> edges = {})
    {
        if (diagnostic) *diagnostic = {code, QString::fromUtf8(text), std::move(vertices), std::move(triangles), std::move(edges)};
        throw Rejected{};
    }
    void tick(qint64 amount = 1)
    {
        if (amount < 0 || amount > API::MaximumWork - work) reject(Code::LimitExceeded, "局部拓扑计算超过工作量限制");
        work += amount;
    }
    Adjacency adjacency()
    {
        Adjacency result;
        for (int i = 0; i < faces.size(); ++i) {
            if (!faces[i].active) continue;
            tick();
            const auto& v = faces[i].vertices;
            for (int side = 0; side < 3; ++side) {
                const int a = v[side], b = v[(side + 1) % 3];
                result[key(a, b)].append({i, v[(side + 2) % 3], a < b ? 1 : -1});
            }
        }
        return result;
    }
    void validate()
    {
        std::set<std::array<int, 3>> unique;
        QVector<QVector<int>> incident(points.size());
        int activeCount = 0;
        for (int i = 0; i < faces.size(); ++i) {
            const auto& face = faces[i]; if (!face.active) continue;
            tick(); ++activeCount;
            auto sorted = face.vertices; std::sort(sorted.begin(), sorted.end());
            for (int v : sorted) if (v < 0 || v >= points.size() || !alive[v]) reject(Code::InvalidInput, "三角形引用已删除或无效顶点", {v}, {i});
            if (sorted[0] == sorted[1] || sorted[1] == sorted[2] || orientation(face.vertices) == 0)
                reject(Code::Degenerate, "参考姿态包含退化三角形", {}, {i});
            if (!unique.insert(sorted).second) reject(Code::DuplicateTriangle, "网格包含重复三角形", {}, {i});
            for (int v : face.vertices) incident[v].append(i);
        }
        if (!activeCount) reject(Code::EmptyTopology, "操作后没有可提交的三角形");
        const auto adj = adjacency();
        for (const auto& item : adj) {
            const auto& list = item.second; const Edge e = edge(item.first);
            if (list.size() > 2) reject(Code::NonManifold, "一条边连接了超过两个面", {}, {}, {e});
            if (list.size() == 2) {
                if (list[0].direction == list[1].direction) reject(Code::InconsistentWinding, "相邻三角形的绕序不一致", {}, {list[0].face, list[1].face}, {e});
                if (orient(points[e.a], points[e.b], points[list[0].opposite]) == orient(points[e.a], points[e.b], points[list[1].opposite]))
                    reject(Code::Intersection, "相邻三角形在参考姿态中重叠", {}, {list[0].face, list[1].face}, {e});
            }
        }
        for (Key constraint : protectedEdges) if (!adj.count(constraint)) reject(Code::InvalidConstraint, "保护边不在现有三角网格中", {}, {}, {edge(constraint)});
        for (int v = 0; v < incident.size(); ++v) {
            if (incident[v].isEmpty()) continue;
            std::map<int, QVector<int>> link;
            for (int faceIndex : incident[v]) {
                const auto& face = faces[faceIndex].vertices;
                const int at = int(std::find(face.begin(), face.end(), v) - face.begin());
                const int a = face[(at + 1) % 3], b = face[(at + 2) % 3];
                link[a].append(b); link[b].append(a);
            }
            int ends = 0;
            for (const auto& item : link) {
                tick();
                if (item.second.size() == 1) ++ends;
                else if (item.second.size() != 2) reject(Code::NonManifold, "顶点邻域不是单一环或边界链", {v});
            }
            if (ends != 0 && ends != 2) reject(Code::NonManifold, "顶点邻域具有多个边界分支", {v});
            QSet<int> visited;
            QVector<int> pending{link.begin()->first};
            while (!pending.isEmpty()) {
                const int current = pending.takeLast(); if (visited.contains(current)) continue;
                visited.insert(current); tick();
                for (int next : link.at(current)) if (!visited.contains(next)) pending.append(next);
            }
            if (visited.size() != qsizetype(link.size())) reject(Code::NonManifold, "多个不相连的面扇共享同一顶点", {v});
        }
        struct BoundedEdge { Edge e; Box box; };
        QVector<BoundedEdge> edges; edges.reserve(adj.size());
        for (const auto& item : adj) { const Edge e = edge(item.first); edges.append({e, bounds(points[e.a], points[e.b])}); }
        std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) { return a.box.left < b.box.left; });
        for (int i = 0; i < edges.size(); ++i) for (int j = i + 1; j < edges.size() && edges[j].box.left <= edges[i].box.right; ++j) {
            tick(); if (!edges[i].box.overlaps(edges[j].box)) continue;
            const Edge a = edges[i].e, b = edges[j].e;
            const int shared = a.a == b.a || a.a == b.b ? a.a : (a.b == b.a || a.b == b.b ? a.b : -1);
            if (shared >= 0) {
                const int first = a.a == shared ? a.b : a.a, second = b.a == shared ? b.b : b.a;
                if (onSegment(points[first], points[shared], points[second]) || onSegment(points[second], points[shared], points[first]))
                    reject(Code::Intersection, "网格包含重叠边或 T 形连接", {shared}, {}, {a, b});
            } else if (intersects(points[a.a], points[a.b], points[b.a], points[b.b]))
                reject(Code::Intersection, "网格边在非共享顶点处相交", {}, {}, {a, b});
        }
        struct BoundedFace { int index; Box box; };
        QVector<BoundedFace> boxes; boxes.reserve(activeCount);
        for (int i = 0; i < faces.size(); ++i) if (faces[i].active) {
            const auto& v = faces[i].vertices; Box box = bounds(points[v[0]], points[v[1]]);
            box.left = std::min(box.left, points[v[2]].x()); box.right = std::max(box.right, points[v[2]].x());
            box.top = std::min(box.top, points[v[2]].y()); box.bottom = std::max(box.bottom, points[v[2]].y()); boxes.append({i, box});
        }
        std::sort(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) { return a.box.left < b.box.left; });
        for (int i = 0; i < boxes.size(); ++i) for (int j = i + 1; j < boxes.size() && boxes[j].box.left < boxes[i].box.right; ++j) {
            tick(); if (!boxes[i].box.overlaps(boxes[j].box)) continue;
            const auto& a = faces[boxes[i].index].vertices; const auto& b = faces[boxes[j].index].vertices;
            for (int v : a) if (strictlyInside(points[v], b)) reject(Code::Intersection, "参考姿态中有相互覆盖的三角形", {v}, {boxes[i].index, boxes[j].index});
            for (int v : b) if (strictlyInside(points[v], a)) reject(Code::Intersection, "参考姿态中有相互覆盖的三角形", {v}, {boxes[i].index, boxes[j].index});
        }
    }
    int orientation(const std::array<int, 3>& v) const { return orient(points[v[0]], points[v[1]], points[v[2]]); }
    bool strictlyInside(QPointF p, const std::array<int, 3>& v) const
    {
        const int sign = orientation(v);
        return orient(points[v[0]], points[v[1]], p) == sign && orient(points[v[1]], points[v[2]], p) == sign && orient(points[v[2]], points[v[0]], p) == sign;
    }
    void removeFace(int index)
    {
        if (faces[index].source >= 0) affected.insert(faces[index].source);
        faces[index].active = false;
    }
    void appendFace(int a, int b, int c, int winding)
    {
        if (faces.size() >= 1000000) reject(Code::LimitExceeded, "三角形数量超过限制");
        const int sign = orient(points[a], points[b], points[c]);
        if (!sign) reject(Code::PrecisionLoss, "新三角形在浮点坐标中退化", {a, b, c});
        if (sign != winding) std::swap(b, c);
        faces.append(Face{{a, b, c}});
    }
    int appendVertex(const QVector<int>& vertices, QVector<double> weights)
    {
        if (points.size() >= CMocMeshStructureBuilder::MaximumVertices) reject(Code::LimitExceeded, "新顶点超过 MOC 索引范围");
        double sum = 0;
        for (double weight : weights) {
            if (!std::isfinite(weight) || weight < 0) reject(Code::PrecisionLoss, "无法计算有限凸插值权重");
            sum += weight;
        }
        if (!std::isfinite(sum) || sum <= 0 || std::abs(sum - 1) > CMocMeshStructureBuilder::WeightTolerance)
            reject(Code::PrecisionLoss, "顶点插值权重之和无效");
        QMap<int, double> combined;
        for (int i = 0; i < weights.size(); ++i) {
            weights[i] /= sum;
            for (int j = 0; j < sources[vertices[i]].indices.size(); ++j) {
                tick(); combined[sources[vertices[i]].indices[j]] += weights[i] * sources[vertices[i]].weights[j];
            }
        }
        Source source; sum = 0;
        for (auto it = combined.cbegin(); it != combined.cend(); ++it) if (it.value() > 0) { source.indices.append(it.key()); source.weights.append(it.value()); sum += it.value(); }
        for (double& weight : source.weights) weight /= sum;
        if (source.indices.isEmpty() || source.indices.size() > 1000000 - sourceReferences) reject(Code::LimitExceeded, "顶点来源权重超过限制");
        sourceReferences += source.indices.size();
        double xy[2]{};
        for (int axis = 0; axis < 2; ++axis) {
            double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
            for (int j = 0; j < source.indices.size(); ++j) {
                const double value = input.positions[source.indices[j] * 2 + axis];
                xy[axis] += value * source.weights[j]; minimum = std::min(minimum, value); maximum = std::max(maximum, value);
            }
            xy[axis] = float(std::clamp(xy[axis], minimum, maximum));
        }
        const QPointF point(xy[0], xy[1]);
        for (int vertex : vertices) if (samePoint(point, points[vertex])) reject(Code::PrecisionLoss, "新点在浮点精度下与原顶点重合", {vertex});
        const int index = points.size(); points.append(point); sources.append(std::move(source)); alive.append(true); inserted.append(index); return index;
    }
    int locate(QPointF point, int seed = -1)
    {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || std::abs(point.x()) > std::numeric_limits<float>::max() || std::abs(point.y()) > std::numeric_limits<float>::max())
            reject(Code::NonFinite, "插点坐标不在有限浮点范围内");
        const auto contains = [&](int index) { const auto& v = faces[index].vertices; return CUvGeometry::ContainsTriangle(point, points[v[0]], points[v[1]], points[v[2]]); };
        if (seed >= 0) {
            if (seed >= faces.size() || !faces[seed].active || !contains(seed)) reject(Code::InvalidSelection, "指定三角形不包含插点", {}, {seed});
            return seed;
        }
        if (seed < -1) reject(Code::InvalidSelection, "三角形编号无效");
        int found = -1;
        for (int i = 0; i < faces.size(); ++i) if (faces[i].active) {
            tick(); if (!contains(i)) continue;
            if (found >= 0 && strictlyInside(point, faces[i].vertices)) reject(Code::AmbiguousLocation, "插点命中了多个面", {}, {found, i});
            found = i;
        }
        if (found < 0) reject(Code::OutsideMesh, "插点位于现有网格外部或洞中");
        return found;
    }
    void splitEdge(int a, int b, int vertex)
    {
        const auto adj = adjacency(); const auto found = adj.find(key(a, b));
        if (found == adj.end()) reject(Code::InvalidSelection, "待拆分的边不存在", {}, {}, {{a, b}});
        for (const auto& item : found->second) {
            const auto old = faces[item.face]; const int winding = orientation(old.vertices);
            removeFace(item.face); appendFace(a, vertex, item.opposite, winding); appendFace(vertex, b, item.opposite, winding);
        }
        if (protectedEdges.erase(key(a, b))) { protectedEdges.insert(key(a, vertex)); protectedEdges.insert(key(vertex, b)); }
    }
    int insert(QPointF point, int seed, int existing = -1)
    {
        const int selectedFace = locate(point, seed);
        const auto face = faces[selectedFace].vertices;
        if (existing < 0) {
            int coincident = -1;
            for (int vertex = 0; vertex < points.size(); ++vertex) if (alive[vertex] && samePoint(point, points[vertex])) {
                if (coincident >= 0) reject(Code::AmbiguousLocation, "插点位置对应多个原顶点", {coincident, vertex});
                coincident = vertex;
            }
            if (coincident >= 0) { selected = {coincident}; return coincident; }
        }
        for (int vertex : face) if (samePoint(point, points[vertex])) {
            if (existing >= 0 && existing != vertex) reject(Code::PrecisionLoss, "孤立点与网格顶点重合", {existing, vertex});
            selected = {vertex}; return vertex;
        }
        for (int i = 0; i < 3; ++i) {
            const int a = face[i], b = face[(i + 1) % 3];
            if (!onSegment(point, points[a], points[b])) continue;
            const double dx = points[b].x() - points[a].x(), dy = points[b].y() - points[a].y();
            const double t = std::abs(dx) >= std::abs(dy) ? (point.x() - points[a].x()) / dx : (point.y() - points[a].y()) / dy;
            const int vertex = existing >= 0 ? existing : appendVertex({a, b}, {1 - t, t});
            splitEdge(a, b, vertex); selected = {vertex}; return vertex;
        }
        const double total = determinant(points[face[0]], points[face[1]], points[face[2]]).estimate();
        QVector<double> weights{determinant(point, points[face[1]], points[face[2]]).estimate() / total,
                                determinant(points[face[0]], point, points[face[2]]).estimate() / total,
                                determinant(points[face[0]], points[face[1]], point).estimate() / total};
        const int vertex = existing >= 0 ? existing : appendVertex({face[0], face[1], face[2]}, weights);
        const int winding = orientation(face); removeFace(selectedFace);
        for (int i = 0; i < 3; ++i) appendFace(face[i], face[(i + 1) % 3], vertex, winding);
        selected = {vertex}; return vertex;
    }
    void triangulate(QVector<int> polygon, int winding)
    {
        if (polygon.size() < 3) return;
        while (polygon.size() > 3) {
            bool clipped = false;
            for (int i = 0; i < polygon.size(); ++i) {
                tick(); const int a = polygon[(i + polygon.size() - 1) % polygon.size()], b = polygon[i], c = polygon[(i + 1) % polygon.size()];
                if (orient(points[a], points[b], points[c]) != winding) continue;
                bool blocked = false;
                for (int v : polygon) {
                    tick(); if (v == a || v == b || v == c) continue;
                    if (CUvGeometry::ContainsTriangle(points[v], points[a], points[b], points[c])) { blocked = true; break; }
                }
                if (blocked) continue;
                for (int j = 0; j < polygon.size(); ++j) {
                    tick(); const int u = polygon[j], v = polygon[(j + 1) % polygon.size()];
                    if (u == a || u == c || v == a || v == c) continue;
                    if (intersects(points[a], points[c], points[u], points[v])) { blocked = true; break; }
                }
                if (blocked) continue;
                appendFace(a, b, c, winding); polygon.removeAt(i); clipped = true; break;
            }
            if (!clipped) reject(Code::InvalidCavity, "局部空腔不能形成无交叉三角剖分", polygon);
        }
        if (orientation({polygon[0], polygon[1], polygon[2]}) != winding) reject(Code::InvalidCavity, "局部空腔退化或绕序无效", polygon);
        appendFace(polygon[0], polygon[1], polygon[2], winding);
    }
    void deleteVertex(int vertex, API::BoundaryPolicy policy)
    {
        for (Key value : protectedEdges) { const Edge e = edge(value); if (e.a == vertex || e.b == vertex) reject(Code::ProtectedEdge, "不能删除保护边的端点", {vertex}, {}, {e}); }
        QVector<int> incident; std::map<int, int> next; QSet<int> incoming;
        int winding = 0;
        for (int i = 0; i < faces.size(); ++i) if (faces[i].active) {
            tick(); const auto& v = faces[i].vertices;
            const auto found = std::find(v.begin(), v.end(), vertex); if (found == v.end()) continue;
            const int at = int(found - v.begin()), a = v[(at + 1) % 3], b = v[(at + 2) % 3];
            if (next.count(a) || incoming.contains(b)) reject(Code::NonManifold, "删除点的邻域存在分支", {vertex});
            next[a] = b; incoming.insert(b); incident.append(i); winding = orientation(v);
        }
        if (incident.isEmpty()) { alive[vertex] = false; return; }
        int start = -1;
        for (const auto& item : next) if (!incoming.contains(item.first)) {
            if (start >= 0) reject(Code::InvalidCavity, "删除点具有多个边界空腔", {vertex}); start = item.first;
        }
        const bool boundary = start >= 0;
        if (!boundary) start = next.begin()->first;
        QVector<int> polygon; QSet<int> visited;
        int current = start;
        while (!visited.contains(current)) {
            tick(); polygon.append(current); visited.insert(current);
            const auto found = next.find(current); if (found == next.end()) break;
            current = found->second;
        }
        if (polygon.size() != qsizetype(next.size()) + (boundary ? 1 : 0) || (!boundary && current != start))
            reject(Code::InvalidCavity, "删除点的空腔不是单一闭环或边界链", {vertex});
        if (boundary) {
            const auto adj = adjacency();
            std::map<int, int> outline;
            for (const auto& item : adj) if (item.second.size() == 1) {
                Edge e = edge(item.first); if (item.second.first().direction < 0) std::swap(e.a, e.b);
                outline[e.a] = e.b;
            }
            int count = 0, cursor = vertex;
            do {
                tick(); const auto found = outline.find(cursor);
                if (found == outline.end() || ++count > qsizetype(outline.size())) reject(Code::InvalidCavity, "边界环无法闭合", {vertex});
                cursor = found->second;
            } while (cursor != vertex);
            if (count <= 3) reject(Code::BoundaryChange, "删除该点会消除边界环或封闭网格洞", {vertex});
            const bool unchanged = onSegment(points[vertex], points[polygon.first()], points[polygon.last()]);
            if (!unchanged && policy == API::BoundaryPolicy::PreserveBoundary) reject(Code::BoundaryChange, "删除该点会改变网格轮廓", {vertex});
            boundaryChanged = boundaryChanged || !unchanged;
        }
        for (int index : incident) removeFace(index);
        alive[vertex] = false;
        triangulate(polygon, winding);
        for (int v : polygon) selected.append(v);
        validate();
    }
    void embed(int vertex)
    {
        for (const auto& face : faces) if (face.active && std::find(face.vertices.begin(), face.vertices.end(), vertex) != face.vertices.end()) return;
        insert(points[vertex], -1, vertex);
    }
    bool interval(int index, QPointF first, QPointF second, double& low, double& high)
    {
        low = 0; high = 1;
        const auto& v = faces[index].vertices; const int winding = orientation(v);
        for (int i = 0; i < 3; ++i) {
            const QPointF a = points[v[i]], b = points[v[(i + 1) % 3]];
            const double start = determinant(a, b, first).estimate() * winding;
            const double end = determinant(a, b, second).estimate() * winding;
            if (start < 0 && end < 0) return false;
            if (start < 0) low = std::max(low, start / (start - end));
            if (end < 0) high = std::min(high, start / (start - end));
            if (high <= low) return false;
        }
        return high > low;
    }
    void connectPair(int first, int second)
    {
        auto adj = adjacency();
        if (adj.count(key(first, second))) { protectedEdges.insert(key(first, second)); return; }
        QSet<int> removed;
        QVector<QPair<double, double>> coverage;
        for (int i = 0; i < faces.size(); ++i) if (faces[i].active) {
            tick(); double low, high;
            if (interval(i, points[first], points[second], low, high)) { removed.insert(i); coverage.append({low, high}); }
        }
        if (removed.isEmpty()) reject(Code::OutsideMesh, "连线没有经过现有三角网格", {first, second});
        std::sort(coverage.begin(), coverage.end());
        double covered = 0;
        for (const auto& span : coverage) {
            if (span.first > covered) reject(Code::OutsideMesh, "连线经过网格外部或洞", {first, second});
            covered = std::max(covered, span.second);
        }
        if (covered < 1) reject(Code::OutsideMesh, "连线没有完整位于网格内", {first, second});
        std::map<int, int> boundary;
        int winding = 0;
        for (const auto& item : adj) {
            int inside = 0; const Adjacent* selectedSide = nullptr;
            for (const auto& side : item.second) if (removed.contains(side.face)) { ++inside; selectedSide = &side; }
            if (inside == 2 && protectedEdges.count(item.first)) reject(Code::ProtectedEdge, "连线穿过保护边", {}, {}, {edge(item.first)});
            if (inside != 1) continue;
            Edge e = edge(item.first); if (selectedSide->direction < 0) std::swap(e.a, e.b);
            if (boundary.count(e.a)) reject(Code::InvalidCavity, "连线经过分叉空腔", {first, second});
            boundary[e.a] = e.b; winding = orientation(faces[selectedSide->face].vertices);
        }
        if (!boundary.count(first) || !boundary.count(second)) reject(Code::OutsideMesh, "连线端点不在同一可三角化空腔上", {first, second});
        QVector<int> loop; QSet<int> visited; int current = first;
        while (!visited.contains(current)) {
            tick(); loop.append(current); visited.insert(current);
            const auto found = boundary.find(current); if (found == boundary.end()) reject(Code::InvalidCavity, "连线空腔的边界不闭合");
            current = found->second;
        }
        if (current != first || loop.size() != qsizetype(boundary.size()) || !loop.contains(second))
            reject(Code::OutsideMesh, "连线穿过洞、边界或不连通分量", {first, second});
        const int divide = loop.indexOf(second);
        QVector<int> left = loop.mid(0, divide + 1), right = loop.mid(divide); right.append(first);
        for (int i : removed) removeFace(i);
        triangulate(left, winding); triangulate(right, winding); protectedEdges.insert(key(first, second));
    }
    void connect(int first, int second, API::CrossingPolicy policy)
    {
        checkVertex(first); checkVertex(second);
        if (first == second) reject(Code::InvalidSelection, "连边需要两个不同顶点", {first});
        embed(first); embed(second); validate();
        QVector<QPair<double, Key>> crossings;
        const QPointF a = points[first], b = points[second];
        for (Key value : protectedEdges) {
            tick(); const Edge e = edge(value);
            if (!properCross(a, b, points[e.a], points[e.b])) continue;
            if (policy == API::CrossingPolicy::RejectProtected) reject(Code::ProtectedEdge, "连线穿过保护边", {}, {}, {e});
            const double u = determinant(points[e.a], points[e.b], a).estimate();
            const double v = determinant(points[e.a], points[e.b], b).estimate();
            crossings.append({u / (u - v), value});
        }
        std::sort(crossings.begin(), crossings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        QVector<QPair<double, int>> chain{{0, first}, {1, second}};
        for (const auto& crossing : crossings) {
            const Edge e = edge(crossing.second);
            const double u = determinant(a, b, points[e.a]).estimate(), v = determinant(a, b, points[e.b]).estimate();
            const double t = u / (u - v);
            const int added = appendVertex({e.a, e.b}, {1 - t, t}); splitEdge(e.a, e.b, added); chain.append({crossing.first, added});
        }
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        QSet<int> used;
        for (const auto& face : faces) if (face.active) for (int v : face.vertices) used.insert(v);
        for (int v : used) if (v != first && v != second && !inserted.contains(v) && onSegment(points[v], a, b)) {
            tick(); const double t = std::abs(dx) >= std::abs(dy) ? (points[v].x() - a.x()) / dx : (points[v].y() - a.y()) / dy;
            chain.append({t, v});
        }
        std::sort(chain.begin(), chain.end(), [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second < b.second); });
        for (int i = 1; i < chain.size(); ++i) connectPair(chain[i - 1].second, chain[i].second);
        selected.clear(); for (const auto& item : chain) selected.append(item.second);
    }
    void retriangulate(const QVector<int>& selection)
    {
        if (selection.isEmpty() || selection.size() > faces.size()) reject(Code::InvalidSelection, "重剖分三角形选择数量无效");
        QSet<int> region;
        for (int index : selection) {
            if (index < 0 || index >= faces.size()) reject(Code::InvalidSelection, "重剖分三角形编号越界", {}, {index});
            region.insert(index);
        }
        auto adj = adjacency(); std::deque<Key> pending;
        for (const auto& item : adj) if (item.second.size() == 2 && region.contains(item.second[0].face) && region.contains(item.second[1].face)) pending.push_back(item.first);
        const auto eraseFace = [&](int index) {
            const auto& v = faces[index].vertices;
            for (int i = 0; i < 3; ++i) {
                auto found = adj.find(key(v[i], v[(i + 1) % 3]));
                auto& list = found->second;
                list.erase(std::remove_if(list.begin(), list.end(), [&](const Adjacent& a) { return a.face == index; }), list.end());
                if (list.isEmpty()) adj.erase(found);
            }
        };
        const auto addFace = [&](int index) {
            const auto& v = faces[index].vertices;
            for (int i = 0; i < 3; ++i) {
                const Key value = key(v[i], v[(i + 1) % 3]);
                adj[value].append({index, v[(i + 2) % 3], v[i] < v[(i + 1) % 3] ? 1 : -1}); pending.push_back(value);
            }
        };
        while (!pending.empty()) {
            tick(); const Key value = pending.front(); pending.pop_front();
            const auto found = adj.find(value);
            if (found == adj.end() || found->second.size() != 2 || protectedEdges.count(value)) continue;
            const auto sides = found->second;
            if (!region.contains(sides[0].face) || !region.contains(sides[1].face)) continue;
            const Edge diagonal = edge(value); const int a = diagonal.a, b = diagonal.b, c = sides[0].opposite, d = sides[1].opposite;
            if (orient(points[c], points[d], points[a]) * orient(points[c], points[d], points[b]) >= 0 || adj.count(key(c, d))) continue;
            const int circle = inCircle(points[a], points[b], points[c], points[d]) * orient(points[a], points[b], points[c]);
            if (circle < 0 || (circle == 0 && key(c, d) >= value)) continue;
            const int one = sides[0].face, two = sides[1].face, winding = orientation(faces[one].vertices);
            eraseFace(one); eraseFace(two);
            if (faces[one].source >= 0) affected.insert(faces[one].source);
            if (faces[two].source >= 0) affected.insert(faces[two].source);
            faces[one].vertices = {c, d, a}; faces[two].vertices = {d, c, b};
            for (int index : {one, two}) if (orientation(faces[index].vertices) != winding) std::swap(faces[index].vertices[1], faces[index].vertices[2]);
            addFace(one); addFace(two);
        }
        for (int index : selection) for (int vertex : faces[index].vertices) selected.append(vertex);
    }
    void checkVertex(int vertex)
    {
        if (vertex < 0 || vertex >= input.positions.size() / 2 || !alive[vertex]) reject(Code::InvalidSelection, "所选顶点编号无效", {vertex});
    }
    API::Result finish()
    {
        for (int vertex : inserted) {
            double sum = 0;
            for (double weight : sources[vertex].weights) sum += weight;
            double xy[2]{};
            for (int axis = 0; axis < 2; ++axis) {
                double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                for (int i = 0; i < sources[vertex].indices.size(); ++i) {
                    const double value = input.positions[sources[vertex].indices[i] * 2 + axis];
                    xy[axis] += value * (sources[vertex].weights[i] / sum);
                    minimum = std::min(minimum, value); maximum = std::max(maximum, value);
                }
                xy[axis] = float(std::clamp(xy[axis], minimum, maximum));
            }
            points[vertex] = QPointF(xy[0], xy[1]);
        }
        validate(); API::Result result; result.edit.artMeshId = input.artMeshId; result.boundaryChanged = boundaryChanged;
        QVector<int> remap(points.size(), -1); qint64 references = 0;
        for (int i = 0; i < points.size(); ++i) if (alive[i]) {
            remap[i] = result.edit.vertices.size(); result.edit.vertices.append(sources[i]); references += sources[i].indices.size();
            if (references > 1000000) reject(Code::LimitExceeded, "顶点来源总数超过限制");
        }
        result.edit.oldToNew = remap.mid(0, input.positions.size() / 2);
        for (const auto& face : faces) if (face.active) for (int v : face.vertices) result.edit.indices.append(quint16(remap[v]));
        for (Key value : protectedEdges) { const Edge e = edge(value); result.protectedEdges.append({remap[e.a], remap[e.b]}); }
        QSet<int> selectedSet;
        for (int v : selected) if (remap[v] >= 0) selectedSet.insert(remap[v]);
        result.selectedVertices = selectedSet.values(); std::sort(result.selectedVertices.begin(), result.selectedVertices.end());
        for (int v : inserted) if (remap[v] >= 0) result.insertedVertices.append(remap[v]);
        result.affectedTriangles = affected.values(); std::sort(result.affectedTriangles.begin(), result.affectedTriangles.end());
        return result;
    }
    const API::Input& input;
    API::Diagnostic* diagnostic;
    QVector<QPointF> points;
    QVector<Source> sources;
    QVector<bool> alive;
    QVector<Face> faces;
    std::set<Key> protectedEdges;
    QSet<int> affected;
    QVector<int> selected, inserted;
    bool boundaryChanged = false;
    qint64 work = 0, sourceReferences = 0;
};

template<class Operation> bool execute(const API::Input& input, API::Result& result, API::Diagnostic* diagnostic, Operation operation)
{
    try {
        Planner planner(input, diagnostic); operation(planner); auto candidate = planner.finish(); result = std::move(candidate);
        if (diagnostic) *diagnostic = {};
        return true;
    } catch (const Rejected&) { return false; }
    catch (const std::exception&) {
        if (diagnostic) *diagnostic = {Code::LimitExceeded, QStringLiteral("无法分配网格规划数据")};
        return false;
    }
}
}

bool CMeshEditPlanner::Insert(const Input& input, QPointF point, int seedTriangle, Result& result, Diagnostic* diagnostic)
{
    return execute(input, result, diagnostic, [&](Planner& planner) { planner.insert(point, seedTriangle); });
}
bool CMeshEditPlanner::Delete(const Input& input, const QVector<int>& vertices, BoundaryPolicy policy, Result& result, Diagnostic* diagnostic)
{
    return execute(input, result, diagnostic, [&](Planner& planner) {
        if (vertices.isEmpty() || vertices.size() > input.positions.size() / 2 ||
            (policy != BoundaryPolicy::PreserveBoundary && policy != BoundaryPolicy::ReplaceWithChord)) planner.reject(Code::InvalidSelection, "删除选择或边界策略无效");
        QSet<int> unique;
        for (int vertex : vertices) { planner.checkVertex(vertex); unique.insert(vertex); }
        auto ordered = unique.values(); std::sort(ordered.begin(), ordered.end());
        for (int vertex : ordered) planner.deleteVertex(vertex, policy);
    });
}
bool CMeshEditPlanner::Connect(const Input& input, int first, int second, CrossingPolicy policy, Result& result, Diagnostic* diagnostic)
{
    return execute(input, result, diagnostic, [&](Planner& planner) {
        if (policy != CrossingPolicy::RejectProtected && policy != CrossingPolicy::SplitProtected) planner.reject(Code::InvalidSelection, "连边交叉策略无效");
        planner.connect(first, second, policy);
    });
}
bool CMeshEditPlanner::Retriangulate(const Input& input, const QVector<int>& triangles, Result& result, Diagnostic* diagnostic)
{
    return execute(input, result, diagnostic, [&](Planner& planner) { planner.retriangulate(triangles); });
}
}
