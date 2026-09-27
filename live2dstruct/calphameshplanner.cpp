#include "calphameshplanner.h"
#include "cboundsspatial.h"
#include "cmesheditplanner.h"
#include "opengl/cuvgeometry.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace live2dstruct {
namespace {
using API = CAlphaMeshPlanner;
struct Rejected
{
};
bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}
bool same(QPointF a, QPointF b)
{
    return a.x() == b.x() && a.y() == b.y();
}
quint64 edgeKey(int a, int b)
{
    return quint64(std::min(a, b)) << 32 | quint32(std::max(a, b));
}
double cross(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}
int orient(QPointF a, QPointF b, QPointF c)
{
    return CUvGeometry::Orientation(a, b, c);
}
QByteArray inputDigest(const API::Input& input)
{
    QByteArray metadata;
    QDataStream out(&metadata, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << input.artMeshId << input.pixelCoordinates << input.uvs << input.indices << input.referenceSize
        << input.pinnedVertices << input.repeat << input.sourceToken << input.image.size() << input.limits.maximumPixels
        << input.limits.maximumWorkingBytes << input.limits.maximumOperations << input.limits.maximumVertices
        << input.limits.maximumConstraints;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(metadata);
    const auto image = input.image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(
            QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), qsizetype(image.width()) * 4));
    return hash.result();
}
QByteArray planDigest(const API::Plan& plan)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << plan.edit.artMeshId << plan.edit.oldToNew << plan.edit.indices << qint64(plan.edit.vertices.size());
    for (const auto& source : plan.edit.vertices)
        out << source.indices << source.weights;
    out << plan.pixelCoordinates << qint64(plan.contours.size());
    for (const auto& contour : plan.contours)
        out << contour.points << contour.hole;
    const auto& s = plan.statistics;
    out << s.oldVertices << s.newVertices << s.triangles << s.islands << s.holes << s.preservedPins << s.scannedPixels
        << s.operations << s.boundarySpacingPixels << s.interiorSpacingPixels << s.marginPixels << plan.warnings
        << plan.sourceToken;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool boundedPlan(const API::Plan& plan, const API::Input& input)
{
    if (plan.edit.artMeshId.size() > 4096 || plan.edit.vertices.size() > input.limits.maximumVertices
        || plan.edit.oldToNew.size() != input.pixelCoordinates.size()
        || plan.pixelCoordinates.size() != plan.edit.vertices.size() || plan.edit.indices.size() > 1000000
        || plan.contours.size() > input.limits.maximumVertices || plan.warnings.size() > 64
        || plan.sourceToken.size() > 4096)
        return false;
    qint64 points = 0;
    for (const auto& vertex : plan.edit.vertices)
        if (vertex.indices.size() > 3 || vertex.weights.size() != vertex.indices.size())
            return false;
    for (const auto& contour : plan.contours) {
        points += contour.points.size();
        if (points > input.limits.maximumVertices)
            return false;
    }
    for (const auto& warning : plan.warnings)
        if (warning.size() > 4096)
            return false;
    return true;
}
bool freeze(const API::Input& source, API::Input& result, QString* error)
{
    const auto& limits = source.limits;
    if (source.artMeshId.isEmpty() || source.artMeshId.contains(QChar::Null) || source.artMeshId.size() > 4096
        || source.image.isNull() || source.pixelCoordinates.size() < 3
        || source.pixelCoordinates.size() > CMocMeshStructureBuilder::MaximumVertices
        || source.uvs.size() != source.pixelCoordinates.size() * 2 || source.indices.isEmpty()
        || source.indices.size() % 3 || source.indices.size() > 1000000
        || source.pinnedVertices.size() > source.pixelCoordinates.size() || source.sourceToken.size() > 4096
        || source.referenceSize.width() <= 0 || source.referenceSize.height() <= 0 || limits.maximumPixels <= 0
        || limits.maximumPixels > 64LL * 1024 * 1024 || limits.maximumWorkingBytes <= 0
        || limits.maximumWorkingBytes > 512LL * 1024 * 1024 || limits.maximumOperations <= 0
        || limits.maximumOperations > 512LL * 1024 * 1024 || limits.maximumVertices < 3
        || limits.maximumVertices > CMocMeshStructureBuilder::MaximumVertices || limits.maximumConstraints < 3
        || limits.maximumConstraints > 100000)
        return fail(error, QStringLiteral("自动网格的素材、拓扑或资源限制无效"));
    const qint64 pixels = qint64(source.image.width()) * source.image.height();
    if (pixels > limits.maximumPixels
        || pixels * 8 + source.pixelCoordinates.size() * 128LL + source.indices.size() * 8LL
            > limits.maximumWorkingBytes)
        return fail(error, QStringLiteral("源图像超过自动网格的像素或内存预算"));
    API::Input next;
    next.artMeshId = QString(source.artMeshId.constData(), source.artMeshId.size());
    next.image = source.image.convertToFormat(QImage::Format_RGBA8888).copy();
    if (next.image.isNull())
        return fail(error, QStringLiteral("无法冻结自动网格图像"));
    for (auto point : source.pixelCoordinates)
        next.pixelCoordinates.append(point);
    for (auto value : source.uvs) {
        if (!std::isfinite(value))
            return fail(error, QStringLiteral("原 UV 包含非有限值"));
        next.uvs.append(value);
    }
    for (auto index : source.indices)
        next.indices.append(index);
    for (auto index : source.pinnedVertices)
        next.pinnedVertices.append(index);
    next.referenceSize = source.referenceSize;
    next.repeat = source.repeat;
    next.limits = limits;
    next.sourceToken = QByteArray(source.sourceToken.constData(), source.sourceToken.size());
    result = std::move(next);
    return true;
}
struct Triangle
{
    std::array<int, 3> v;
    QRectF box;
};
class Generator
{
public:
    Generator(const API::Input& input, const API::Settings& settings, QString* error, CExportControl* control)
        : input(input)
        , settings(settings)
        , error(error)
        , control(control)
    {
    }
    [[noreturn]] void reject(const QString& message)
    {
        fail(error, message);
        throw Rejected { };
    }
    void tick(qint64 amount = 1)
    {
        if (amount < 0 || amount > input.limits.maximumOperations - work)
            reject(QStringLiteral("自动网格超过计算工作量限制，请增大间距或减小素材区域"));
        work += amount;
        memory();
        if (control && !control->Checkpoint())
            reject(control->Error().isEmpty() ? QStringLiteral("自动网格已取消") : control->Error());
    }
    qint64 retainedBytes() const
    {
        return qint64(input.image.width()) * input.image.height() * 8 + input.pixelCoordinates.size() * 128LL
            + input.indices.size() * 192LL + qint64(w) * h * 4 + spatialReferences * 32 + contourVertices * 256
            + domain.points.size() * 256LL + domain.constraints.size() * 128LL;
    }
    void memory(qint64 extra = 0)
    {
        if (extra < 0 || extra > input.limits.maximumWorkingBytes - retainedBytes())
            reject(QStringLiteral("自动网格超过内存预算，原网格保持不变"));
    }
    void report(const QString& message)
    {
        tick();
        if (control
            && !control->Report(CExportControl::Phase::Preparing, work, input.limits.maximumOperations, message))
            reject(control->Error().isEmpty() ? QStringLiteral("自动网格已取消") : control->Error());
    }
    void validateSource()
    {
        for (auto p : input.pixelCoordinates) {
            tick();
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || std::abs(p.x()) > 1e9 || std::abs(p.y()) > 1e9)
                reject(QStringLiteral("素材坐标超出有限可计算范围"));
        }
        QSet<int> pins;
        for (int pin : input.pinnedVertices) {
            tick();
            if (pin < 0 || pin >= input.pixelCoordinates.size() || pins.contains(pin))
                reject(QStringLiteral("需保留的接缝顶点无效或重复"));
            pins.insert(pin);
        }
        std::set<std::array<int, 3>> faces;
        std::map<quint64, QVector<int>> edges;
        for (int i = 0; i < input.indices.size(); i += 3) {
            tick();
            Triangle t { { input.indices[i], input.indices[i + 1], input.indices[i + 2] }, { } };
            for (int v : t.v)
                if (v >= input.pixelCoordinates.size())
                    reject(QStringLiteral("原网格三角索引越界"));
            auto sorted = t.v;
            std::sort(sorted.begin(), sorted.end());
            if (sorted[0] == sorted[1] || sorted[1] == sorted[2] || !faces.insert(sorted).second)
                reject(QStringLiteral("原网格包含重复或退化三角形"));
            const auto a = input.pixelCoordinates[t.v[0]], b = input.pixelCoordinates[t.v[1]],
                       c = input.pixelCoordinates[t.v[2]];
            if (!orient(a, b, c))
                reject(QStringLiteral("原网格 UV 存在退化三角形，不能恢复对应关键形"));
            if (!orient(uv(t.v[0]), uv(t.v[1]), uv(t.v[2])))
                reject(QStringLiteral("原始 float UV 三角形退化，无法迁移"));
            t.box = QRectF(QPointF(std::min({ a.x(), b.x(), c.x() }), std::min({ a.y(), b.y(), c.y() })),
                QPointF(std::max({ a.x(), b.x(), c.x() }), std::max({ a.y(), b.y(), c.y() })));
            for (int s = 0; s < 3; ++s) {
                auto& list = edges[edgeKey(t.v[s], t.v[(s + 1) % 3])];
                list.append(triangles.size());
                if (list.size() > 2)
                    reject(QStringLiteral("原网格边连接超过两个面"));
            }
            triangles.append(t);
        }
        QVector<CBoundsSpatial::Box> sourceBoxes;
        sourceBoxes.reserve(triangles.size());
        for (const auto& triangle : triangles)
            sourceBoxes.append(
                { triangle.box.left(), triangle.box.right(), triangle.box.top(), triangle.box.bottom() });
        const auto spend = [&] { tick(); };
        CBoundsSpatial(sourceBoxes, spend).pairs(true, spend, [&](int first, int second) {
            const auto& a = triangles[first];
            const auto& b = triangles[second];
            for (int side = 0; side < 3; ++side)
                for (int other = 0; other < 3; ++other) {
                    tick();
                    const auto p = input.pixelCoordinates[a.v[side]], q = input.pixelCoordinates[a.v[(side + 1) % 3]],
                               r = input.pixelCoordinates[b.v[other]], s = input.pixelCoordinates[b.v[(other + 1) % 3]];
                    if (orient(p, q, r) * orient(p, q, s) < 0 && orient(r, s, p) * orient(r, s, q) < 0)
                        reject(QStringLiteral("原网格 UV 三角形相交，不能唯一迁移"));
                }
            const auto strict = [&](int vertex, const Triangle& t) {
                const auto p = input.pixelCoordinates[vertex];
                int signs[3];
                for (int s = 0; s < 3; ++s)
                    signs[s] = orient(input.pixelCoordinates[t.v[s]], input.pixelCoordinates[t.v[(s + 1) % 3]], p);
                return signs[0] && signs[0] == signs[1] && signs[1] == signs[2];
            };
            for (int v : a.v)
                if (strict(v, b))
                    reject(QStringLiteral("原网格 UV 三角形重叠，不能唯一迁移"));
            for (int v : b.v)
                if (strict(v, a))
                    reject(QStringLiteral("原网格 UV 三角形重叠，不能唯一迁移"));
            const auto center
                = (input.pixelCoordinates[a.v[0]] + input.pixelCoordinates[a.v[1]] + input.pixelCoordinates[a.v[2]])
                / 3;
            int signs[3];
            for (int s = 0; s < 3; ++s)
                signs[s] = orient(input.pixelCoordinates[b.v[s]], input.pixelCoordinates[b.v[(s + 1) % 3]], center);
            if (signs[0] && signs[0] == signs[1] && signs[1] == signs[2])
                reject(QStringLiteral("原网格 UV 三角形覆盖同一区域，不能唯一迁移"));
        });
        for (const auto& item : edges)
            if (item.second.size() == 2) {
                const int a = int(item.first >> 32), b = int(quint32(item.first));
                int sides[2] { };
                for (int j = 0; j < 2; ++j)
                    for (int v : triangles[item.second[j]].v)
                        if (v != a && v != b)
                            sides[j] = orient(
                                input.pixelCoordinates[a], input.pixelCoordinates[b], input.pixelCoordinates[v]);
                if (sides[0] == sides[1])
                    reject(QStringLiteral("原网格 UV 在共享边两侧重叠"));
            }
        sourceEdges = std::move(edges);
        charts.fill(-1, triangles.size());
        QVector<QVector<int>> adjacent(triangles.size());
        for (const auto& item : sourceEdges)
            if (item.second.size() == 2) {
                adjacent[item.second[0]].append(item.second[1]);
                adjacent[item.second[1]].append(item.second[0]);
            }
        int chart = 0;
        for (int index = 0; index < triangles.size(); ++index)
            if (charts[index] < 0) {
                QVector<int> pending { index };
                charts[index] = chart;
                while (!pending.isEmpty()) {
                    tick();
                    const int face = pending.takeLast();
                    for (int next : adjacent[face])
                        if (charts[next] < 0) {
                            charts[next] = chart;
                            pending.append(next);
                        }
                }
                ++chart;
            }
    }
    QPointF uv(int vertex) const { return { input.uvs[vertex * 2], input.uvs[vertex * 2 + 1] }; }
    bool weights(QPointF p, const Triangle& t, std::array<double, 3>& result) const
    {
        const auto a = input.pixelCoordinates[t.v[0]], b = input.pixelCoordinates[t.v[1]],
                   c = input.pixelCoordinates[t.v[2]];
        const double d = cross(b - a, c - a);
        result = { cross(b - p, c - p) / d, cross(c - p, a - p) / d, cross(a - p, b - p) / d };
        for (double value : result)
            if (!std::isfinite(value) || value < -1e-10 || value > 1 + 1e-10)
                return false;
        double sum = 0;
        for (auto& value : result) {
            value = std::clamp(value, 0.0, 1.0);
            sum += value;
        }
        for (auto& value : result)
            value /= sum;
        return true;
    }
    int owner(QPointF p)
    {
        int found = -1;
        const auto candidates = spatial.isEmpty() ? QVector<int> { } : spatial.value(int(std::floor(p.x() / 64)));
        for (int position = 0; position < (spatial.isEmpty() ? triangles.size() : candidates.size()); ++position) {
            const int index = spatial.isEmpty() ? position : candidates[position];
            tick();
            const auto& t = triangles[index];
            if (p.x() < t.box.left() - 1e-8 || p.x() > t.box.right() + 1e-8 || p.y() < t.box.top() - 1e-8
                || p.y() > t.box.bottom() + 1e-8)
                continue;
            std::array<double, 3> w;
            if (!weights(p, t, w))
                continue;
            if (found >= 0 && std::all_of(w.begin(), w.end(), [](double v) { return v > 1e-10; }))
                reject(QStringLiteral("素材点命中多个原三角形"));
            if (found < 0)
                found = index;
        }
        return found;
    }
    bool coveredCell(int x, int y)
    {
        const QRectF cell(x, y, 1, 1);
        const auto candidates = spatial.value(int(std::floor(double(x) / 64)));
        for (int index : candidates) {
            tick();
            const auto& t = triangles[index];
            if (!t.box.intersects(cell))
                continue;
            const auto polygon = CUvGeometry::ClipTriangle(
                input.pixelCoordinates[t.v[0]], input.pixelCoordinates[t.v[1]], input.pixelCoordinates[t.v[2]], cell);
            double area = 0;
            for (size_t i = 0; i < polygon.size(); ++i)
                area += cross(polygon[i] - cell.topLeft(), polygon[(i + 1) % polygon.size()] - cell.topLeft());
            if (area != 0)
                return true;
        }
        return false;
    }
    void makeMask(API::Plan& plan)
    {
        const double scale = std::max(input.referenceSize.width(), input.referenceSize.height()) / 1024.0;
        spacing = settings.interiorSpacing * scale;
        boundarySpacing = settings.boundarySpacing * scale;
        const double requestedMargin = settings.margin * scale;
        if (!std::isfinite(spacing) || !std::isfinite(boundarySpacing) || !std::isfinite(requestedMargin)
            || spacing < 1e-6 || boundarySpacing < 1e-6 || requestedMargin > 1000000)
            reject(QStringLiteral("自动网格的像素间距或边界余量超出范围"));
        const int radius = int(std::ceil(requestedMargin));
        QRectF bounds = triangles.first().box;
        for (const auto& t : triangles)
            bounds = bounds.united(t.box);
        const double left = std::floor(bounds.left()) - radius - 2, top = std::floor(bounds.top()) - radius - 2;
        const double right = std::ceil(bounds.right()) + radius + 2, bottom = std::ceil(bounds.bottom()) + radius + 2;
        const double width = right - left, height = bottom - top;
        if (width <= 0 || height <= 0 || width > 1000000 || height > 1000000
            || width * height > input.limits.maximumPixels)
            reject(QStringLiteral("原 UV 覆盖区域超过自动网格的像素预算"));
        origin = QPoint(int(left), int(top));
        w = int(width);
        h = int(height);
        const qint64 area = qint64(w) * h;
        memory();
        for (int i = 0; i < triangles.size(); ++i) {
            const auto& box = triangles[i].box;
            for (int col = int(std::floor(box.left() / 64)); col <= int(std::floor(box.right() / 64)); ++col) {
                tick();
                ++spatialReferences;
                memory();
                spatial[col].append(i);
            }
        }
        mask = QByteArray(area, 0);
        qint64 occupied = 0;
        for (int y = 1; y < h - 1; ++y) {
            tick(w);
            const qint64 sy = qint64(origin.y()) + y;
            int py = int((sy % input.image.height() + input.image.height()) % input.image.height());
            const auto* row = input.image.constScanLine(py);
            for (int x = 1; x < w - 1; ++x) {
                const qint64 sx = qint64(origin.x()) + x;
                if (!input.repeat && (sx < 0 || sy < 0 || sx >= input.image.width() || sy >= input.image.height()))
                    continue;
                const int px = int((sx % input.image.width() + input.image.width()) % input.image.width());
                if (row[px * 4 + 3] > settings.alphaThreshold && coveredCell(int(sx), int(sy))) {
                    mask[qsizetype(y) * w + x] = 1;
                    ++occupied;
                }
            }
        }
        if (!occupied)
            reject(QStringLiteral("阈值以上没有可生成网格的像素，原网格保持不变"));
        originalMask = mask;
        if (radius) {
            QByteArray temp(area, 0), dilated(area, 0);
            for (int y = 0; y < h; ++y) {
                int count = 0;
                for (int x = 0; x < w; ++x) {
                    tick();
                    if (x + radius < w)
                        count += mask[qsizetype(y) * w + x + radius];
                    if (x == 0)
                        for (int k = 0; k < radius && k < w; ++k) {
                            tick();
                            count += mask[qsizetype(y) * w + k];
                        }
                    if (x - radius - 1 >= 0)
                        count -= mask[qsizetype(y) * w + x - radius - 1];
                    temp[qsizetype(y) * w + x] = count > 0;
                }
            }
            for (int x = 0; x < w; ++x) {
                int count = 0;
                for (int y = 0; y < h; ++y) {
                    tick();
                    if (y + radius < h)
                        count += temp[qsizetype(y + radius) * w + x];
                    if (y == 0)
                        for (int k = 0; k < radius && k < h; ++k) {
                            tick();
                            count += temp[qsizetype(k) * w + x];
                        }
                    if (y - radius - 1 >= 0)
                        count -= temp[qsizetype(y - radius - 1) * w + x];
                    dilated[qsizetype(y) * w + x] = count > 0;
                }
            }
            mask.swap(dilated);
        }
        for (int x = 0; x < w; ++x) {
            mask[x] = 0;
            mask[qsizetype(h - 1) * w + x] = 0;
        }
        for (int y = 0; y < h; ++y) {
            mask[qsizetype(y) * w] = 0;
            mask[qsizetype(y) * w + w - 1] = 0;
        }
        plan.statistics.scannedPixels = area;
        plan.statistics.boundarySpacingPixels = boundarySpacing;
        plan.statistics.interiorSpacingPixels = spacing;
        plan.statistics.marginPixels = radius;
        if (radius)
            plan.warnings.append(
                QStringLiteral("边界余量使用整像素方形扩张；距离较近的区域可能合并，细小孔洞可能闭合"));
    }
    bool filled(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && mask[qsizetype(y) * w + x]; }
    bool filled(QPointF p) const
    {
        const double x = std::floor(p.x()) - origin.x(), y = std::floor(p.y()) - origin.y();
        return x >= 0 && y >= 0 && x < w && y < h && filled(int(x), int(y));
    }
    bool inContours(QPointF point, const QVector<API::Contour>& contours)
    {
        bool inside = false;
        for (const auto& contour : contours)
            for (int i = 0, j = contour.points.size() - 1; i < contour.points.size(); j = i++) {
                tick();
                const auto a = contour.points[j], b = contour.points[i];
                if ((a.y() > point.y()) != (b.y() > point.y())
                    && point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
                    inside = !inside;
            }
        return inside;
    }
    QVector<QPointF> simplifyRing(const QVector<QPointF>& points, double tolerance)
    {
        int farthest = 1;
        double distance = 0;
        for (int i = 1; i < points.size(); ++i) {
            tick();
            const double value = std::hypot(points[i].x() - points[0].x(), points[i].y() - points[0].y());
            if (value > distance) {
                distance = value;
                farthest = i;
            }
        }
        QVector<QPointF> chain = points;
        chain.append(points[0]);
        QVector<bool> keep(chain.size(), false);
        keep[0] = keep[farthest] = keep[chain.size() - 1] = true;
        QVector<QPair<int, int>> pending { { 0, farthest }, { farthest, int(chain.size() - 1) } };
        while (!pending.isEmpty()) {
            const auto span = pending.takeLast();
            const auto a = chain[span.first], b = chain[span.second], delta = b - a;
            const double length = std::hypot(delta.x(), delta.y());
            double maximum = tolerance;
            int index = -1;
            for (int i = span.first + 1; i < span.second; ++i) {
                tick();
                const double d = std::abs(cross(delta, chain[i] - a)) / length;
                if (d > maximum) {
                    maximum = d;
                    index = i;
                }
            }
            if (index >= 0) {
                keep[index] = true;
                pending.append({ span.first, index });
                pending.append({ index, span.second });
            }
        }
        QVector<QPointF> result;
        for (int i = 0; i < points.size(); ++i)
            if (keep[i])
                result.append(points[i]);
        return result.size() >= 3 ? result : points;
    }
    bool preservesAlpha(const QVector<API::Contour>& candidate, const QVector<API::Contour>& original)
    {
        struct Segment
        {
            QPointF a, b;
            int ring, index, count;
        };
        QVector<Segment> segments;
        for (int ring = 0; ring < candidate.size(); ++ring) {
            const auto& points = candidate[ring].points;
            double area = 0;
            for (int i = 0; i < points.size(); ++i) {
                tick();
                area += cross(points[i] - points[0], points[(i + 1) % points.size()] - points[0]);
                segments.append({ points[i], points[(i + 1) % points.size()], ring, i, int(points.size()) });
            }
            if (area == 0 || (area < 0) != original[ring].hole)
                return false;
        }
        for (int i = 0; i < segments.size(); ++i)
            for (int j = i + 1; j < segments.size(); ++j) {
                tick();
                const auto &a = segments[i], &b = segments[j];
                if (a.ring == b.ring
                    && (a.index == b.index || ((a.index + 1) % a.count) == b.index
                        || ((b.index + 1) % b.count) == a.index))
                    continue;
                if (std::max(a.a.x(), a.b.x()) < std::min(b.a.x(), b.b.x())
                    || std::max(b.a.x(), b.b.x()) < std::min(a.a.x(), a.b.x())
                    || std::max(a.a.y(), a.b.y()) < std::min(b.a.y(), b.b.y())
                    || std::max(b.a.y(), b.b.y()) < std::min(a.a.y(), a.b.y()))
                    continue;
                if (orient(a.a, a.b, b.a) * orient(a.a, a.b, b.b) <= 0
                    && orient(b.a, b.b, a.a) * orient(b.a, b.b, a.b) <= 0)
                    return false;
            }
        for (const auto& edge : segments) {
            const int left = std::max(0, int(std::floor(std::min(edge.a.x(), edge.b.x()))) - origin.x()),
                      right = std::min(w - 1, int(std::floor(std::max(edge.a.x(), edge.b.x()))) - origin.x());
            const int top = std::max(0, int(std::floor(std::min(edge.a.y(), edge.b.y()))) - origin.y()),
                      bottom = std::min(h - 1, int(std::floor(std::max(edge.a.y(), edge.b.y()))) - origin.y());
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x) {
                    tick();
                    if (!originalMask[qsizetype(y) * w + x])
                        continue;
                    double low = 0, high = 1;
                    for (int axis = 0; axis < 2; ++axis) {
                        const double first = axis ? edge.a.y() : edge.a.x(),
                                     delta = (axis ? edge.b.y() : edge.b.x()) - first,
                                     minimum = axis ? origin.y() + y : origin.x() + x;
                        if (delta == 0) {
                            if (first <= minimum || first >= minimum + 1) {
                                high = -1;
                                break;
                            }
                        } else {
                            double a = (minimum - first) / delta, b = (minimum + 1 - first) / delta;
                            if (a > b)
                                std::swap(a, b);
                            low = std::max(low, a);
                            high = std::min(high, b);
                        }
                    }
                    if (high > low) {
                        const auto p = edge.a + (edge.b - edge.a) * ((low + high) / 2);
                        if (p.x() > origin.x() + x && p.x() < origin.x() + x + 1 && p.y() > origin.y() + y
                            && p.y() < origin.y() + y + 1)
                            return false;
                    }
                }
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                tick();
                if (originalMask[qsizetype(y) * w + x]
                    && !inContours({ origin.x() + x + .5, origin.y() + y + .5 }, candidate))
                    return false;
            }
        for (const auto& contour : original)
            if (contour.hole) {
                const auto a = contour.points[0], b = contour.points[1], delta = b - a;
                const double length = std::hypot(delta.x(), delta.y());
                const auto point = (a + b) / 2 + QPointF(delta.y(), -delta.x()) * (.1 / length);
                if (inContours(point, candidate))
                    return false;
            }
        return true;
    }
    void simplify(API::Plan& plan)
    {
        double tolerance = std::min(plan.statistics.marginPixels * .75, boundarySpacing * .15);
        const auto original = plan.contours;
        for (int attempt = 0; tolerance > 0 && attempt < 6; ++attempt, tolerance *= .5) {
            auto candidate = original;
            bool changed = false;
            for (auto& contour : candidate) {
                auto points = simplifyRing(contour.points, tolerance);
                changed |= points.size() != contour.points.size();
                contour.points = std::move(points);
            }
            if (!changed)
                return;
            if (preservesAlpha(candidate, original)) {
                plan.contours = std::move(candidate);
                return;
            }
        }
    }
    void contours(API::Plan& plan)
    {
        using IPoint = std::pair<int, int>;
        struct Segment
        {
            IPoint a, b;
            bool used = false;
        };
        QVector<Segment> segments;
        std::map<IPoint, QVector<int>> outgoing;
        const auto add = [&](IPoint a, IPoint b) {
            memory((segments.size() + 1) * 256LL);
            if (segments.size() >= input.limits.maximumConstraints * 16LL)
                reject(QStringLiteral("透明轮廓过于复杂，超过边界工作量限制"));
            outgoing[a].append(segments.size());
            segments.append({ a, b });
        };
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                tick();
                if (!filled(x, y))
                    continue;
                if (!filled(x, y - 1))
                    add({ x, y }, { x + 1, y });
                if (!filled(x + 1, y))
                    add({ x + 1, y }, { x + 1, y + 1 });
                if (!filled(x, y + 1))
                    add({ x + 1, y + 1 }, { x, y + 1 });
                if (!filled(x - 1, y))
                    add({ x, y + 1 }, { x, y });
            }
        const auto direction = [](IPoint a, IPoint b) {
            return b.first > a.first ? 0 : b.second > a.second ? 1 : b.first < a.first ? 2 : 3;
        };
        for (int start = 0; start < segments.size(); ++start)
            if (!segments[start].used) {
                QVector<QPointF> loop;
                int current = start;
                for (;;) {
                    tick();
                    auto& edge = segments[current];
                    if (edge.used)
                        reject(QStringLiteral("透明轮廓无法闭合"));
                    edge.used = true;
                    loop.append(QPointF(origin.x() + edge.a.first, origin.y() + edge.a.second));
                    if (edge.b == segments[start].a)
                        break;
                    const int dir = direction(edge.a, edge.b);
                    int selected = -1, best = -1;
                    for (int next : outgoing[edge.b])
                        if (!segments[next].used) {
                            int turn = (direction(segments[next].a, segments[next].b) - dir + 4) % 4;
                            const int rank = turn == 1 ? 3 : turn == 0 ? 2 : turn == 3 ? 1 : 0;
                            if (rank > best) {
                                best = rank;
                                selected = next;
                            }
                        }
                    if (selected < 0)
                        reject(QStringLiteral("透明轮廓出现开放边界"));
                    current = selected;
                }
                API::Contour contour;
                double area = 0;
                for (int i = 0; i < loop.size(); ++i) {
                    tick();
                    const auto prev = loop[(i + loop.size() - 1) % loop.size()], p = loop[i],
                               next = loop[(i + 1) % loop.size()];
                    area += cross(p, next);
                    if (orient(prev, p, next))
                        contour.points.append(p);
                }
                if (contour.points.size() < 3 || area == 0)
                    reject(QStringLiteral("透明轮廓退化，无法生成三角形"));
                contour.hole = area < 0;
                contourVertices += contour.points.size();
                memory(segments.size() * 256LL);
                plan.contours.append(std::move(contour));
            }
        for (const auto& contour : plan.contours) {
            if (contour.hole)
                ++plan.statistics.holes;
            else
                ++plan.statistics.islands;
        }
    }
    int addPoint(QPointF p)
    {
        tick();
        const std::pair<double, double> key { p.x(), p.y() };
        const auto found = pointIds.find(key);
        if (found != pointIds.end())
            return found->second;
        if (domain.points.size() >= input.limits.maximumVertices)
            reject(QStringLiteral("自动网格超过顶点限制，请增大点间距或降低轮廓复杂度"));
        const int index = domain.points.size();
        memory(256);
        domain.points.append(p);
        pointIds.emplace(key, index);
        return index;
    }
    void addEdge(int a, int b)
    {
        if (a == b)
            return;
        const auto key = edgeKey(a, b);
        if (!constraintIds.insert(key).second)
            return;
        if (domain.constraints.size() >= input.limits.maximumConstraints)
            reject(QStringLiteral("自动网格超过约束边数量限制"));
        memory(128);
        domain.constraints.append({ a, b });
    }
    void makeDomain(const API::Plan& plan)
    {
        QVector<int> originals;
        for (auto p : input.pixelCoordinates)
            originals.append(addPoint(p));
        for (const auto& item : sourceEdges)
            addEdge(originals[int(item.first >> 32)], originals[int(quint32(item.first))]);
        for (const auto& contour : plan.contours)
            for (int i = 0; i < contour.points.size(); ++i) {
                tick();
                const auto a = contour.points[i], b = contour.points[(i + 1) % contour.points.size()];
                const double length = std::hypot(b.x() - a.x(), b.y() - a.y()),
                             parts = std::ceil(length / boundarySpacing);
                if (parts > input.limits.maximumVertices)
                    reject(QStringLiteral("边界点间距过小，超过顶点限制"));
                int previous = addPoint(a);
                const int count = std::max(1, int(parts));
                for (int j = 1; j <= count; ++j) {
                    int next = addPoint(j == count ? b : a + (b - a) * (double(j) / count));
                    addEdge(previous, next);
                    previous = next;
                }
            }
        const double rowStep = spacing * std::sqrt(3.0) / 2;
        const double rows = std::ceil(h / rowStep), columns = std::ceil(w / spacing);
        if (rows * columns > input.limits.maximumOperations - work)
            reject(QStringLiteral("内部点间距过小，超过采样预算"));
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x) {
                tick();
                const QPointF p(origin.x() + (x + .5 + (y % 2) * .5) * spacing, origin.y() + (y + .5) * rowStep);
                if (filled(p) && owner(p) >= 0)
                    addPoint(p);
            }
        domain.maximumVertices = input.limits.maximumVertices;
        domain.maximumWork = input.limits.maximumOperations - work;
        domain.maximumWorkingBytes = input.limits.maximumWorkingBytes - retainedBytes();
    }
    CMocMeshStructureBuilder::VertexSource source(QPointF p, int triangle)
    {
        for (int vertex : triangles[triangle].v)
            if (same(p, input.pixelCoordinates[vertex]))
                return { { vertex }, { 1 } };
        std::array<double, 3> values;
        if (!weights(p, triangles[triangle], values))
            reject(QStringLiteral("新顶点位于原 UV 支持域之外，不能迁移关键形"));
        CMocMeshStructureBuilder::VertexSource result;
        for (int i = 0; i < 3; ++i)
            if (values[i] > 0) {
                result.indices.append(triangles[triangle].v[i]);
                result.weights.append(values[i]);
            }
        return result;
    }
    QPointF quantizedPoint(const CMocMeshStructureBuilder::VertexSource& source, int triangle, QPointF ideal)
    {
        double values[2] { };
        double sum = 0;
        for (double weight : source.weights)
            sum += weight;
        for (int axis = 0; axis < 2; ++axis) {
            if (source.indices.size() == 1) {
                values[axis] = input.uvs[source.indices[0] * 2 + axis];
                continue;
            }
            double low = std::numeric_limits<double>::infinity(), high = -low;
            for (int i = 0; i < source.indices.size(); ++i) {
                const double value = input.uvs[source.indices[i] * 2 + axis];
                low = std::min(low, value);
                high = std::max(high, value);
                values[axis] += value * (source.weights[i] / sum);
            }
            values[axis] = float(std::clamp(values[axis], low, high));
        }
        const QPointF point(values[0], values[1]);
        const auto& t = triangles[triangle];
        const auto a = uv(t.v[0]), b = uv(t.v[1]), c = uv(t.v[2]);
        const double determinant = cross(b - a, c - a);
        const std::array<double, 3> weightsOut { cross(b - point, c - point) / determinant,
            cross(c - point, a - point) / determinant, cross(a - point, b - point) / determinant };
        QPointF actual;
        for (int i = 0; i < 3; ++i)
            actual += input.pixelCoordinates[t.v[i]] * weightsOut[i];
        if (!std::isfinite(actual.x()) || !std::isfinite(actual.y())
            || std::hypot(actual.x() - ideal.x(), actual.y() - ideal.y()) > .01)
            reject(QStringLiteral("MOC float UV 量化误差超过 0.01 素材像素，请先调整 UV 或素材配准"));
        outputUvs.append(point);
        return actual;
    }
    void output(const CMeshEditPlanner::ConstrainedResult& mesh, API::Plan& plan)
    {
        QVector<std::array<int, 3>> kept;
        QVector<int> owners;
        for (int i = 0; i < mesh.indices.size(); i += 3) {
            tick();
            std::array<int, 3> v { mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2] };
            const auto p = (mesh.points[v[0]] + mesh.points[v[1]] + mesh.points[v[2]]) / 3;
            if (!inContours(p, plan.contours))
                continue;
            const int triangle = owner(p);
            if (triangle < 0)
                continue;
            for (int vertex : v) {
                std::array<double, 3> weightsOut;
                if (!weights(mesh.points[vertex], triangles[triangle], weightsOut))
                    reject(QStringLiteral("约束剖分跨越原三角边，不能安全迁移"));
            }
            if (orient(input.pixelCoordinates[triangles[triangle].v[0]],
                    input.pixelCoordinates[triangles[triangle].v[1]], input.pixelCoordinates[triangles[triangle].v[2]])
                < 0)
                std::swap(v[1], v[2]);
            kept.append(v);
            owners.append(triangle);
        }
        if (kept.isEmpty())
            reject(QStringLiteral("透明轮廓与原 UV 覆盖域没有可生成网格的交集"));
        plan.edit.artMeshId = input.artMeshId;
        plan.edit.oldToNew.fill(-1, input.pixelCoordinates.size());
        QMap<quint64, int> remap;
        const auto append = [&](int vertex, int triangle) {
            const quint64 key = quint64(vertex) << 32 | quint32(charts[triangle]);
            const auto found = remap.constFind(key);
            if (found != remap.cend())
                return *found;
            auto s = source(mesh.points[vertex], triangle);
            QPointF p;
            for (int j = 0; j < s.indices.size(); ++j)
                p += input.pixelCoordinates[s.indices[j]] * s.weights[j];
            const int index = plan.edit.vertices.size();
            if (index >= input.limits.maximumVertices)
                reject(QStringLiteral("输出顶点超过 MOC 上限"));
            if (s.indices.size() == 1 && s.weights[0] == 1 && plan.edit.oldToNew[s.indices[0]] < 0)
                plan.edit.oldToNew[s.indices[0]] = index;
            const auto actual = quantizedPoint(s, triangle, p);
            outputOrigins.append(p);
            plan.edit.vertices.append(std::move(s));
            plan.pixelCoordinates.append(actual);
            remap.insert(key, index);
            return index;
        };
        for (int i = 0; i < kept.size(); ++i) {
            std::array<int, 3> v;
            for (int j = 0; j < 3; ++j)
                v[j] = append(kept[i][j], owners[i]);
            if (!orient(plan.pixelCoordinates[v[0]], plan.pixelCoordinates[v[1]], plan.pixelCoordinates[v[2]]))
                reject(QStringLiteral("迁移权重量化后新三角形退化"));
            const auto& original = triangles[owners[i]].v;
            if (orient(outputUvs[v[0]], outputUvs[v[1]], outputUvs[v[2]])
                != orient(uv(original[0]), uv(original[1]), uv(original[2])))
                reject(QStringLiteral("MOC float UV 量化后新三角形翻转或退化"));
            for (int vertex : v)
                plan.edit.indices.append(quint16(vertex));
        }
        validateQuantized(plan);
        splitPointContacts(plan);
        outputContours(plan);
        for (int pin : input.pinnedVertices) {
            tick();
            if (plan.edit.oldToNew[pin] < 0) {
                if (plan.edit.vertices.size() >= input.limits.maximumVertices)
                    reject(QStringLiteral("保留接缝顶点后超过顶点限制"));
                plan.edit.oldToNew[pin] = plan.edit.vertices.size();
                plan.edit.vertices.append({ { pin }, { 1 } });
                plan.pixelCoordinates.append(input.pixelCoordinates[pin]);
            }
        }
        plan.statistics.oldVertices = input.pixelCoordinates.size();
        plan.statistics.newVertices = plan.edit.vertices.size();
        plan.statistics.triangles = plan.edit.indices.size() / 3;
        plan.statistics.preservedPins = input.pinnedVertices.size();
        plan.warnings.append(QStringLiteral(
            "保留原 UV "
            "三角边；较粗预设不保证少于原网格顶点。全部关键形按原三角插值，父级非线性变形后的外观可能变化"));
        if (!input.pinnedVertices.isEmpty())
            plan.warnings.append(
                QStringLiteral("保留原 Glue 端点及权重；新增顶点不会自动建立 Glue 配对，需检查接缝形变"));
    }
    void validateQuantized(const API::Plan& plan)
    {
        std::map<std::pair<double, double>, int> positions;
        for (int index = 0; index < outputUvs.size(); ++index) {
            tick();
            const auto p = outputUvs[index];
            const auto inserted = positions.emplace(std::make_pair(p.x(), p.y()), index);
            if (!inserted.second && !same(outputOrigins[index], outputOrigins[inserted.first->second]))
                reject(QStringLiteral("不同素材顶点在 float UV 量化后重合"));
        }
        QVector<Triangle> faces;
        for (int index = 0; index < plan.edit.indices.size(); index += 3) {
            tick();
            Triangle t { { plan.edit.indices[index], plan.edit.indices[index + 1], plan.edit.indices[index + 2] },
                { } };
            const auto a = outputUvs[t.v[0]], b = outputUvs[t.v[1]], c = outputUvs[t.v[2]];
            if (!orient(a, b, c))
                reject(QStringLiteral("MOC float UV 量化后产生退化三角形"));
            t.box = QRectF(QPointF(std::min({ a.x(), b.x(), c.x() }), std::min({ a.y(), b.y(), c.y() })),
                QPointF(std::max({ a.x(), b.x(), c.x() }), std::max({ a.y(), b.y(), c.y() })));
            faces.append(t);
        }
        std::sort(faces.begin(), faces.end(), [&](const auto& a, const auto& b) {
            tick();
            return a.box.left() < b.box.left();
        });
        for (int i = 0; i < faces.size(); ++i)
            for (int j = i + 1; j < faces.size() && faces[j].box.left() <= faces[i].box.right(); ++j) {
                tick();
                const auto &a = faces[i], &b = faces[j];
                if (a.box.bottom() < b.box.top() || b.box.bottom() < a.box.top())
                    continue;
                for (int side = 0; side < 3; ++side)
                    for (int other = 0; other < 3; ++other) {
                        tick();
                        const auto p = outputUvs[a.v[side]], q = outputUvs[a.v[(side + 1) % 3]],
                                   r = outputUvs[b.v[other]], s = outputUvs[b.v[(other + 1) % 3]];
                        if (orient(p, q, r) * orient(p, q, s) < 0 && orient(r, s, p) * orient(r, s, q) < 0)
                            reject(QStringLiteral("MOC float UV 量化后网格边交叉"));
                    }
                const auto strict = [&](int vertex, const Triangle& t) {
                    const auto p = outputUvs[vertex];
                    int signs[3];
                    for (int s = 0; s < 3; ++s)
                        signs[s] = orient(outputUvs[t.v[s]], outputUvs[t.v[(s + 1) % 3]], p);
                    return signs[0] && signs[0] == signs[1] && signs[1] == signs[2];
                };
                for (int vertex : a.v)
                    if (strict(vertex, b))
                        reject(QStringLiteral("MOC float UV 量化后三角形重叠"));
                for (int vertex : b.v)
                    if (strict(vertex, a))
                        reject(QStringLiteral("MOC float UV 量化后三角形重叠"));
            }
    }
    void outputContours(API::Plan& plan)
    {
        struct Boundary
        {
            int first = -1;
            int second = -1;
            int count = 0;
        };
        std::map<quint64, Boundary> edges;
        for (int i = 0; i < plan.edit.indices.size(); i += 3) {
            tick();
            std::array<int, 3> vertices { plan.edit.indices[i], plan.edit.indices[i + 1], plan.edit.indices[i + 2] };
            if (orient(plan.pixelCoordinates[vertices[0]], plan.pixelCoordinates[vertices[1]],
                    plan.pixelCoordinates[vertices[2]])
                < 0)
                std::swap(vertices[1], vertices[2]);
            for (int side = 0; side < 3; ++side) {
                const int a = vertices[side], b = vertices[(side + 1) % 3];
                auto& edge = edges[edgeKey(a, b)];
                if (edge.count && (edge.count > 1 || edge.first != b || edge.second != a))
                    reject(QStringLiteral("生成网格的相邻面绕序或边数无效"));
                if (!edge.count) {
                    edge.first = a;
                    edge.second = b;
                }
                ++edge.count;
            }
        }
        std::map<int, int> next;
        for (const auto& item : edges)
            if (item.second.count == 1) {
                const auto& edge = item.second;
                if (!next.emplace(edge.first, edge.second).second)
                    reject(QStringLiteral("生成轮廓含未分离的分叉顶点"));
            }
        QVector<API::Contour> contours;
        while (!next.empty()) {
            const int first = next.begin()->first;
            int current = first;
            QVector<QPointF> points;
            do {
                tick();
                const auto found = next.find(current);
                if (found == next.end())
                    reject(QStringLiteral("生成轮廓未闭合"));
                points.append(plan.pixelCoordinates[current]);
                current = found->second;
                next.erase(found);
            } while (current != first);
            API::Contour contour;
            double area = 0;
            for (int i = 0; i < points.size(); ++i) {
                const auto previous = points[(i + points.size() - 1) % points.size()], point = points[i],
                           following = points[(i + 1) % points.size()];
                area += cross(point - points[0], following - points[0]);
                if (orient(previous, point, following))
                    contour.points.append(point);
            }
            if (area == 0 || contour.points.size() < 3)
                reject(QStringLiteral("生成轮廓在 UV 量化后退化"));
            contour.hole = area < 0;
            contours.append(std::move(contour));
        }
        plan.contours = std::move(contours);
        plan.statistics.islands = plan.statistics.holes = 0;
        for (const auto& contour : plan.contours) {
            if (contour.hole)
                ++plan.statistics.holes;
            else
                ++plan.statistics.islands;
        }
    }
    void splitPointContacts(API::Plan& plan)
    {
        const int count = plan.edit.vertices.size();
        QVector<QVector<int>> incident(count);
        for (int i = 0; i < plan.edit.indices.size(); ++i)
            incident[plan.edit.indices[i]].append(i / 3);
        for (int vertex = 0; vertex < count; ++vertex) {
            QSet<int> remaining(incident[vertex].cbegin(), incident[vertex].cend());
            bool first = true;
            while (!remaining.isEmpty()) {
                tick();
                QVector<int> pending { *std::min_element(remaining.cbegin(), remaining.cend()) }, component;
                remaining.remove(pending[0]);
                while (!pending.isEmpty()) {
                    const int face = pending.takeLast();
                    component.append(face);
                    QSet<int> neighbors;
                    for (int j = 0; j < 3; ++j) {
                        const int v = plan.edit.indices[face * 3 + j];
                        if (v != vertex)
                            neighbors.insert(v);
                    }
                    auto candidates = remaining.values();
                    std::sort(candidates.begin(), candidates.end());
                    for (int other : candidates) {
                        tick();
                        bool adjacent = false;
                        for (int j = 0; j < 3; ++j)
                            adjacent |= neighbors.contains(plan.edit.indices[other * 3 + j]);
                        if (adjacent) {
                            remaining.remove(other);
                            pending.append(other);
                        }
                    }
                }
                if (first) {
                    first = false;
                    continue;
                }
                const auto& source = plan.edit.vertices[vertex];
                if (source.indices.size() == 1 && source.weights[0] == 1
                    && input.pinnedVertices.contains(source.indices[0]))
                    reject(QStringLiteral(
                        "透明轮廓在 Glue 端点处分成多个区域，复制端点会改变接缝；请调整轮廓余量或手动处理"));
                if (plan.edit.vertices.size() >= input.limits.maximumVertices)
                    reject(QStringLiteral("分离对角接触区域后超过顶点限制"));
                const int next = plan.edit.vertices.size();
                plan.edit.vertices.append(plan.edit.vertices[vertex]);
                plan.pixelCoordinates.append(plan.pixelCoordinates[vertex]);
                for (int face : component)
                    for (int j = 0; j < 3; ++j)
                        if (plan.edit.indices[face * 3 + j] == vertex)
                            plan.edit.indices[face * 3 + j] = quint16(next);
            }
        }
    }
    API::Plan run()
    {
        API::Plan plan;
        report(QStringLiteral("验证原 UV 覆盖域"));
        validateSource();
        report(QStringLiteral("提取透明轮廓"));
        makeMask(plan);
        contours(plan);
        simplify(plan);
        report(QStringLiteral("生成轮廓与原三角约束"));
        makeDomain(plan);
        CMeshEditPlanner::ConstrainedResult mesh;
        CMeshEditPlanner::Diagnostic diagnostic;
        if (!CMeshEditPlanner::TriangulateConstraints(domain, mesh, &diagnostic, control))
            reject(diagnostic.message);
        tick(mesh.operations);
        memory(mesh.points.size() * 1024LL + mesh.indices.size() * 256LL);
        report(QStringLiteral("迁移全部关键形的顶点来源"));
        output(mesh, plan);
        plan.statistics.operations = work;
        plan.sourceToken = input.sourceToken;
        return plan;
    }
    const API::Input& input;
    const API::Settings& settings;
    QString* error;
    CExportControl* control;
    qint64 work = 0;
    qint64 spatialReferences = 0;
    qint64 contourVertices = 0;
    QVector<Triangle> triangles;
    std::map<quint64, QVector<int>> sourceEdges;
    QByteArray mask, originalMask;
    QPoint origin;
    int w = 0, h = 0;
    double spacing = 0, boundarySpacing = 0;
    QVector<QPointF> outputUvs, outputOrigins;
    QVector<int> charts;
    QMap<int, QVector<int>> spatial;
    CMeshEditPlanner::ConstrainedInput domain;
    std::map<std::pair<double, double>, int> pointIds;
    std::set<quint64> constraintIds;
};
}
struct CAlphaMeshPlanner::Plan::Verification
{
    Input input;
    QByteArray planDigest;
};
bool CAlphaMeshPlanner::Settings::operator==(const Settings& other) const
{
    return boundarySpacing == other.boundarySpacing && interiorSpacing == other.interiorSpacing
        && margin == other.margin && alphaThreshold == other.alphaThreshold;
}
CAlphaMeshPlanner::Settings CAlphaMeshPlanner::Preset(Density density)
{
    if (density == Density::Coarse)
        return { 48, 80, 2, 0 };
    if (density == Density::Fine)
        return { 12, 20, 2, 0 };
    return { };
}
bool CAlphaMeshPlanner::ValidateSettings(const Settings& settings, QString* error)
{
    if (!std::isfinite(settings.boundarySpacing) || !std::isfinite(settings.interiorSpacing)
        || !std::isfinite(settings.margin) || settings.boundarySpacing <= 0 || settings.interiorSpacing <= 0
        || settings.boundarySpacing > 4096 || settings.interiorSpacing > 4096 || settings.margin < 0
        || settings.margin > 1024 || settings.alphaThreshold < 0 || settings.alphaThreshold > 255)
        return fail(error, QStringLiteral("点间距必须在 0 至 4096 内，余量为 0 至 1024，透明阈值为 0 至 255"));
    if (error)
        error->clear();
    return true;
}
bool CAlphaMeshPlanner::Build(
    const Input& input, const Settings& settings, Plan& result, QString* error, CExportControl* control)
{
    try {
        const auto frozenSettings = settings;
        Input frozen;
        if (!ValidateSettings(frozenSettings, error) || !freeze(input, frozen, error))
            return false;
        Generator generator(frozen, frozenSettings, error, control);
        auto candidate = generator.run();
        auto verification = std::make_shared<Plan::Verification>();
        verification->input = std::move(frozen);
        verification->planDigest = planDigest(candidate);
        candidate.m_verification = std::move(verification);
        result = std::move(candidate);
        if (error)
            error->clear();
        return true;
    } catch (const Rejected&) {
        return false;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法分配自动网格数据，原网格保持不变"));
    }
}
bool CAlphaMeshPlanner::Validate(const Input& input, const Plan& plan, QString* error)
{
    try {
        if (!plan.m_verification)
            return fail(error, QStringLiteral("自动网格缺少已验证的规划快照"));
        if (!boundedPlan(plan, plan.m_verification->input))
            return fail(error, QStringLiteral("自动网格规划字段超过验证限制"));
        Input frozen;
        if (!freeze(input, frozen, error))
            return false;
        if (inputDigest(frozen) != inputDigest(plan.m_verification->input)
            || planDigest(plan) != plan.m_verification->planDigest)
            return fail(error, QStringLiteral("素材、网格或规划结果已改变，请重新生成"));
        if (error)
            error->clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法验证自动网格快照"));
    }
}
}
