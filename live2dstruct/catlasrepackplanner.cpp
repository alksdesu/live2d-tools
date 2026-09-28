#include "catlasrepackplanner.h"
#include "cartworkreconciler.h"
#include "std/cpngimage.h"
#include "std/cexportcontrol.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QUuid>
#include <QStringView>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <tuple>

namespace live2dstruct {
struct CAtlasRepackPlanner::Plan::Verification
{
    std::shared_ptr<const Snapshot> source;
    Plan plan;
};
struct CAtlasPlanVerification
{
    static bool Check(const CAtlasRepackPlanner::Snapshot& source, const CAtlasRepackPlanner::Plan& plan);
    static bool Capture(const CAtlasRepackPlanner::Snapshot& source, const CAtlasRepackPlanner::Plan& plan,
                        CAtlasRepackPlanner::Snapshot& frozenSource, CAtlasRepackPlanner::Plan& frozenPlan, QString* error);
    static bool Freeze(const CAtlasRepackPlanner::Snapshot& source,
                       std::shared_ptr<const CAtlasRepackPlanner::Snapshot>& result, QString* error);
    static void Seal(std::shared_ptr<const CAtlasRepackPlanner::Snapshot> source, CAtlasRepackPlanner::Plan& plan);
    static void Seal(const CAtlasRepackPlanner::Plan& before, CAtlasRepackPlanner::Plan& plan);
};
namespace {
using Planner = CAtlasRepackPlanner;
using Plan = Planner::Plan;
using Snapshot = Planner::Snapshot;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool checkpoint(CExportControl* control, QString* error)
{
    return !control || control->Checkpoint() || fail(error, control->Error().isEmpty() ? QStringLiteral("已取消图集计算") : control->Error());
}
int repeat(qint64 value, int size) { const auto n = value % size; return int(n < 0 ? n + size : n); }
bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
bool validateFields(const Snapshot& snapshot, const Plan& plan, QString* error);
CArtworkReconciler::Triangles triangles(const Snapshot& snapshot)
{
    CArtworkReconciler::Triangles result;
    for (const auto& mesh : snapshot.meshes) result.insert(mesh.material.id, mesh.triangles);
    return result;
}
bool imageEquals(const QImage& first, const QImage& second)
{
    if (first.size() != second.size() || first.format() != QImage::Format_RGBA8888 || second.format() != QImage::Format_RGBA8888) return false;
    for (int y = 0; y < first.height(); ++y)
        if (std::memcmp(first.constScanLine(y), second.constScanLine(y), size_t(first.width()) * 4)) return false;
    return true;
}
bool unite(const QRect& first, const QRect& second, QRect& result, QString* error)
{
    const qint64 x = std::min(first.x(), second.x()), y = std::min(first.y(), second.y());
    const qint64 right = std::max(qint64(first.x()) + first.width(), qint64(second.x()) + second.width());
    const qint64 bottom = std::max(qint64(first.y()) + first.height(), qint64(second.y()) + second.height());
    if (right - x > std::numeric_limits<int>::max() || bottom - y > std::numeric_limits<int>::max())
        return fail(error, QStringLiteral("重叠素材的联合区域超出整数坐标范围"));
    result = QRect(int(x), int(y), int(right - x), int(bottom - y)); return true;
}
const Planner::Mesh* mesh(const Snapshot& snapshot, const QString& id)
{
    for (const auto& item : snapshot.meshes) if (item.material.id == id) return &item;
    return nullptr;
}
CModelDrawableMaterial* material(Plan& plan, const QString& id)
{
    for (auto& item : plan.state.materials) if (item.id == id) return &item;
    return nullptr;
}
const CModelDrawableMaterial* material(const Plan& plan, const QString& id)
{
    for (const auto& item : plan.state.materials) if (item.id == id) return &item;
    return nullptr;
}
qint64 maximumPixels(const Snapshot& snapshot)
{ return std::min(snapshot.limits.maximumPixels, CTextureWorkspace::MaximumDecodedBytes / 4); }
bool dimensions(const Snapshot& snapshot, QSize size, QString* error)
{
    return (size.width() > 0 && size.height() > 0 && size.width() <= snapshot.limits.maximumDimension &&
        size.height() <= snapshot.limits.maximumDimension && qint64(size.width()) * size.height() <= maximumPixels(snapshot))
        || fail(error, QStringLiteral("图集尺寸超出设备或像素预算"));
}
bool allocation(const Snapshot& snapshot, const Plan& plan, int replacedSlot, QSize size, QString* error)
{
    qint64 pixels = qint64(size.width()) * size.height(); QSet<qint64> images;
    if (pixels > maximumPixels(snapshot)) return fail(error, QStringLiteral("候选图集的总解码像素超出预算"));
    for (auto image = plan.images.cbegin(); image != plan.images.cend(); ++image) {
        if (image.key() == replacedSlot || images.contains(image->cacheKey())) continue;
        images.insert(image->cacheKey()); const qint64 area = qint64(image->width()) * image->height();
        if (area > maximumPixels(snapshot) - pixels) return fail(error, QStringLiteral("候选图集的总解码像素超出预算"));
        pixels += area;
    }
    return true;
}
bool token(const Snapshot& snapshot, QByteArray& result, QString* error)
{
    QByteArray workspace, artwork;
    if (snapshot.sourceDigest.size() != 32 || snapshot.topologyDigest.size() != 32 ||
        snapshot.limits.maximumDimension <= 0 || snapshot.limits.maximumPixels <= 0 || snapshot.limits.maximumOperations <= 0)
        return fail(error, QStringLiteral("图集规划来源或预算无效"));
    if (!CTextureWorkspace::Fingerprint(snapshot.sources, snapshot.workspace, workspace, error) ||
        !CModelArtwork::Fingerprint(snapshot.artwork, artwork, error)) return false;
    QByteArray encoded; QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << snapshot.sourceDigest << snapshot.documentRevision << snapshot.topologyDigest << workspace << artwork
           << snapshot.limits.maximumDimension << snapshot.limits.maximumPixels << snapshot.limits.maximumOperations;
    QSet<QString> ids;
    qint64 values = 0;
    for (const auto& item : snapshot.meshes) {
        if (item.material.id.isEmpty() || ids.contains(item.material.id) || item.material.uvs.size() % 2 ||
            values > CModelMaterialEdit::MaximumValues - item.material.uvs.size())
            return fail(error, QStringLiteral("图集规划的网格目录或 UV 数量无效"));
        ids.insert(item.material.id); values += item.material.uvs.size();
        stream << item.material.id << item.material.textureIndex << item.triangles << item.editable << item.reason;
        for (float uv : item.material.uvs) {
            if (!std::isfinite(uv)) return fail(error, QStringLiteral("图集规划包含无效 UV"));
            quint32 bits; std::memcpy(&bits, &uv, sizeof(bits)); stream << bits;
        }
        if (item.triangles.size() % 3) return fail(error, QStringLiteral("图集网格三角形不完整"));
        for (quint16 index : item.triangles) if (index >= item.material.uvs.size() / 2)
            return fail(error, QStringLiteral("图集网格三角形索引越界"));
    }
    if (stream.status() != QDataStream::Ok) return fail(error, QStringLiteral("无法准备图集来源标识"));
    result = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256); return true;
}
bool support(const QVector<QPointF>& points, int gutter, QRect& result, QString* error)
{
    if (points.isEmpty() || gutter < 1 || gutter > 64) return fail(error, QStringLiteral("请选择有 UV 的素材，边距范围为 1–64 像素"));
    double left = points.first().x(), right = left, top = points.first().y(), bottom = top;
    for (const auto& point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail(error, QStringLiteral("素材像素坐标无效"));
        left = std::min(left, point.x()); right = std::max(right, point.x());
        top = std::min(top, point.y()); bottom = std::max(bottom, point.y());
    }
    left = std::floor(left) - gutter; top = std::floor(top) - gutter;
    right = std::ceil(right) + gutter; bottom = std::ceil(bottom) + gutter;
    const double low = std::numeric_limits<int>::min(), high = std::numeric_limits<int>::max();
    if (left < low || top < low || right > high || bottom > high || right <= left || bottom <= top ||
        right - left > high || bottom - top > high)
        return fail(error, QStringLiteral("素材区域超出整数坐标范围，请先调整 UV"));
    result = QRect(int(left), int(top), int(right - left), int(bottom - top)); return true;
}
bool budget(const Snapshot& snapshot, qint64 added, qint64& total, QString* error)
{
    if (added < 0 || added > snapshot.limits.maximumOperations - total)
        return fail(error, QStringLiteral("图集计算超出操作预算，请分批处理素材"));
    total += added; return true;
}
class SupportCache
{
public:
    SupportCache(const Snapshot& snapshot, const Plan& plan, int gutter, qint64& operations)
        : m_snapshot(snapshot), m_plan(plan), m_gutter(gutter), m_operations(operations) {}
    bool Bounds(const QString& id, QRect& result, QString* error)
    {
        const auto cached = m_meshes.constFind(id);
        if (cached != m_meshes.cend()) { result = *cached; return true; }
        const auto points = m_plan.pixelCoordinates.constFind(id);
        if (points == m_plan.pixelCoordinates.cend()) return fail(error, QStringLiteral("素材缺少像素坐标：%1").arg(id));
        if (!budget(m_snapshot, points->size() + 1, m_operations, error)) return false;
        const auto binding = m_plan.state.artwork.bindings.constFind(id);
        const bool linked = binding != m_plan.state.artwork.bindings.cend() && binding->status == CModelArtwork::BindingStatus::Linked;
        int gutter = m_gutter;
        if (linked) gutter = std::max(gutter, m_plan.state.artwork.placements.value(binding->placementId).gutter);
        QRect bounds;
        if ((!points->isEmpty() || !linked) && !support(*points, gutter, bounds, error)) return false;
        if (linked) {
            auto placement = m_placements.constFind(binding->placementId);
            if (placement == m_placements.cend()) {
                QRect domain;
                if (!budget(m_snapshot, 4, m_operations, error) ||
                    !CArtworkReconciler::SourcePatch(m_plan.state.artwork, id, m_gutter, domain, error)) return false;
                placement = m_placements.insert(binding->placementId, domain);
            }
            if (bounds.isEmpty()) bounds = *placement;
            else if (!unite(bounds, *placement, bounds, error)) return false;
        }
        m_meshes.insert(id, bounds); result = bounds; return true;
    }
private:
    const Snapshot& m_snapshot;
    const Plan& m_plan;
    int m_gutter;
    qint64& m_operations;
    QMap<QString, QRect> m_meshes, m_placements;
};
bool hasSupport(const Plan& plan, const CModelDrawableMaterial& material)
{
    const auto binding = plan.state.artwork.bindings.constFind(material.id);
    return !material.uvs.isEmpty() || (binding != plan.state.artwork.bindings.cend() && binding->status == CModelArtwork::BindingStatus::Linked);
}
bool selected(const Snapshot& snapshot, const Plan& plan, const QStringList& ids, QSet<QString>& result, QString* error)
{
    if (ids.isEmpty() || ids.size() > snapshot.meshes.size()) return fail(error, QStringLiteral("请选择有效数量的素材"));
    QSet<QString> candidate;
    for (const auto& id : ids) {
        if (candidate.contains(id) || !mesh(snapshot, id) || !material(plan, id) || plan.pixelCoordinates.value(id).isEmpty())
            return fail(error, QStringLiteral("素材选择重复、不存在或没有有效 UV：%1").arg(id));
        candidate.insert(id);
    }
    result.swap(candidate); return true;
}
QStringList ownedIds(const QStringList& values)
{
    QStringList result; result.reserve(values.size());
    for (const auto& value : values) result.append(QString(value.constData(), value.size()));
    return result;
}
bool atlasName(const QString& value, QString& result, QString* error)
{
    const auto trimmed = QStringView(value).trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 256 || trimmed.contains(QChar::Null))
        return fail(error, QStringLiteral("图集名称无效"));
    result = trimmed.toString(); return true;
}
bool selectionArea(const Plan& plan, const QStringList& ids, SupportCache& support, QRect& bounds, int& slot, QString* error)
{
    int source = -1; QRect area;
    for (const auto& id : ids) {
        const auto* item = material(plan, id);
        if (source != -1 && item->textureIndex != source) return fail(error, QStringLiteral("跨图集素材请分组移动或使用自动排布"));
        QRect next;
        if (!support.Bounds(id, next, error)) return false;
        if (source == -1) area = next;
        else if (!unite(area, next, area, error)) return false;
        source = item->textureIndex;
    }
    bounds = area; slot = source; return true;
}
bool encode(const QImage& image, QByteArray& result, QString* error)
{
    QByteArray bytes; QBuffer buffer(&bytes);
    if (image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG") || bytes.isEmpty())
        return fail(error, QStringLiteral("无法编码候选图集"));
    result.swap(bytes); return true;
}
bool finish(const Snapshot& snapshot, const Plan& before, Plan candidate, Plan& result, QString* error, CExportControl* control)
{
    if (!checkpoint(control, error)) return false;
    candidate.changedMeshes.clear(); candidate.changedSlots.clear();
    for (auto it = candidate.images.begin(); it != candidate.images.end(); ++it) {
        if (!candidate.state.workspace.atlases.contains(it.key()) || !dimensions(snapshot, it->size(), error)) return false;
        auto& atlas = candidate.state.workspace.atlases[it.key()];
        if (before.images.contains(it.key()) && imageEquals(*it, before.images.value(it.key()))) {
            *it = before.images.value(it.key());
        } else {
            if (!encode(*it, atlas.png, error) || !checkpoint(control, error)) return false;
            const auto baseline = snapshot.workspace.atlases.constFind(it.key());
            if (baseline != snapshot.workspace.atlases.cend() && baseline->size == atlas.size) {
                const auto bytes = baseline->png.isEmpty() ? snapshot.sources.value(baseline->sourceSlot).png : baseline->png;
                QImage initial;
                if (!CPngImage::Decode(bytes, initial, error)) return false;
                if (imageEquals(initial, *it)) {
                    atlas.png = QByteArray(baseline->png.constData(), baseline->png.size());
                    continue;
                }
            }
            const auto source = snapshot.sources.constFind(atlas.sourceSlot);
            if (source != snapshot.sources.cend() && source->size == atlas.size) {
                QImage original;
                if (!CPngImage::Decode(source->png, original, error)) return false;
                if (imageEquals(original, *it)) atlas.png.clear();
            }
        }
    }
    for (const auto& value : candidate.state.materials) {
        const auto* original = mesh(snapshot, value.id);
        if (!original) return fail(error, QStringLiteral("候选图集改变了网格目录"));
        if (value != original->material) {
            if (!original->editable) return fail(error, original->reason.isEmpty() ? QStringLiteral("素材已锁定：%1").arg(value.id) : original->reason);
            candidate.changedMeshes.insert(value.id);
        }
    }
    for (auto it = snapshot.workspace.atlases.cbegin(); it != snapshot.workspace.atlases.cend(); ++it)
        if (!candidate.state.workspace.atlases.contains(it.key()) || candidate.state.workspace.atlases.value(it.key()) != *it)
            candidate.changedSlots.insert(it.key());
    for (auto it = candidate.state.workspace.atlases.cbegin(); it != candidate.state.workspace.atlases.cend(); ++it)
        if (!snapshot.workspace.atlases.contains(it.key())) candidate.changedSlots.insert(it.key());
    if (!CTextureWorkspace::ValidateTransition(snapshot.sources, before.state.workspace, candidate.state.workspace, error) ||
        !validateFields(snapshot, candidate, error) || !checkpoint(control, error)) return false;
    candidate.warnings.clear();
    if (!candidate.changedSlots.isEmpty())
        candidate.warnings.append(QStringLiteral("素材像素不重采样；图集重建后，缩小显示时的边缘采样可能变化。"));
    CAtlasPlanVerification::Seal(before, candidate);
    result = std::move(candidate); return true;
}
bool setPixels(const Snapshot& snapshot, Plan& plan, const QString& id, int slot, QVector<QPointF> points, QString* error)
{
    auto* value = material(plan, id);
    const auto atlas = plan.state.workspace.atlases.constFind(slot);
    const auto* original = mesh(snapshot, id);
    if (!value || !original || atlas == plan.state.workspace.atlases.cend() || points.size() * 2 != value->uvs.size())
        return fail(error, QStringLiteral("素材像素映射缺少图集或网格"));
    QVector<float> uvs; uvs.reserve(value->uvs.size());
    for (const auto& point : points) {
        const float u = float(point.x() / atlas->size.width()), v = float(point.y() / atlas->size.height());
        if (!std::isfinite(u) || !std::isfinite(v)) return fail(error, QStringLiteral("转换后的 UV 超出有限范围"));
        uvs << u << v;
    }
    if (slot == original->material.textureIndex) {
        const auto originalAtlas = snapshot.workspace.atlases.constFind(slot);
        if (originalAtlas != snapshot.workspace.atlases.cend() && originalAtlas->size == atlas->size) {
            bool identity = true;
            for (int index = 0; index < points.size(); ++index)
                identity &= points[index].x() == double(original->material.uvs[index * 2]) * atlas->size.width() &&
                            points[index].y() == double(original->material.uvs[index * 2 + 1]) * atlas->size.height();
            if (identity) {
                uvs = original->material.uvs;
                for (int index = 0; index < points.size(); ++index)
                    points[index] = {double(uvs[index * 2]) * atlas->size.width(), double(uvs[index * 2 + 1]) * atlas->size.height()};
            }
        }
    }
    value->textureIndex = slot; value->uvs.swap(uvs); plan.pixelCoordinates[id] = std::move(points); return true;
}
struct Patch { int slot = -1; QRect source; QStringList ids; };
struct Placement { Patch patch; int slot = -1; QPoint topLeft; int turns = 0; };
QSize rotatedSize(QSize size, int turns) { return turns % 2 ? QSize(size.height(), size.width()) : size; }
QPointF transform(QPointF point, const QRect& source, QPoint destination, int turns)
{
    point -= source.topLeft();
    if (turns == 1) point = {source.height() - point.y(), point.x()};
    if (turns == 2) point = {source.width() - point.x(), source.height() - point.y()};
    if (turns == 3) point = {point.y(), source.width() - point.x()};
    return point + destination;
}
QPoint sourcePixel(int x, int y, const QRect& rect, int turns)
{
    if (turns == 1) return {rect.x() + y, rect.y() + rect.height() - 1 - x};
    if (turns == 2) return {rect.x() + rect.width() - 1 - x, rect.y() + rect.height() - 1 - y};
    if (turns == 3) return {rect.x() + rect.width() - 1 - y, rect.y() + x};
    return {rect.x() + x, rect.y() + y};
}
const uchar* rgba(const QImage& image, qint64 x, qint64 y)
{
    return image.constScanLine(repeat(y, image.height())) + repeat(x, image.width()) * 4;
}
QVector<QPair<int, int>> intervals(qint64 start, int length, int period)
{
    if (length >= period) return {{0, period}};
    const int first = repeat(start, period);
    if (length <= period - first) return {{first, first + length}};
    return {{first, period}, {0, length - (period - first)}};
}
QVector<QRect> wrapped(const QRect& rect, QSize size)
{
    QVector<QRect> result;
    for (const auto& x : intervals(rect.x(), rect.width(), size.width()))
        for (const auto& y : intervals(rect.y(), rect.height(), size.height()))
            result.append({x.first, y.first, x.second - x.first, y.second - y.first});
    return result;
}
bool render(const Snapshot& snapshot, const Plan& plan, const QVector<Placement>& placements,
            const QSet<int>& clearedSlots, int gutter, Plan& candidate,
            qint64& operations, QString* error, CExportControl* control)
{
    QMap<QString, CArtworkReconciler::Rebase> rebases;
    QSet<QString> moved;
    for (const auto& place : placements) for (const auto& id : place.patch.ids) moved.insert(id);
    QMap<int, QVector<QPair<QRect, QString>>> protection;
    SupportCache protectionSupport(snapshot, plan, 1, operations);
    for (const auto& value : plan.state.materials) if (!moved.contains(value.id) && hasSupport(plan, value)) {
        if (!checkpoint(control, error)) return false;
        const auto atlas = plan.state.workspace.atlases.constFind(value.textureIndex);
        if (atlas == plan.state.workspace.atlases.cend()) return fail(error, QStringLiteral("未选素材缺少有效图集"));
        if (clearedSlots.contains(value.textureIndex)) return fail(error, QStringLiteral("重排目标仍有未选素材：%1").arg(value.id));
        QRect bounds;
        if (!protectionSupport.Bounds(value.id, bounds, error)) return false;
        for (const auto& area : wrapped(bounds, atlas->size)) protection[value.textureIndex].append({area, value.id});
    }
    QSet<int> detachedSlots = clearedSlots;
    for (int slot : clearedSlots) {
        const auto size = candidate.state.workspace.atlases.value(slot).size;
        if (!budget(snapshot, qint64(size.width()) * size.height(), operations, error) || !allocation(snapshot, candidate, slot, size, error)) return false;
        QImage empty(size, QImage::Format_RGBA8888);
        if (empty.isNull()) return fail(error, QStringLiteral("无法分配重排图集"));
        empty.fill(Qt::transparent); candidate.images[slot] = std::move(empty);
    }
    for (const auto& place : placements) {
        const auto source = plan.images.constFind(place.patch.slot);
        auto target = candidate.images.find(place.slot);
        const QSize size = rotatedSize(place.patch.source.size(), place.turns);
        if (source == plan.images.cend() || target == candidate.images.end() ||
            place.topLeft.x() < 0 || place.topLeft.y() < 0 ||
            qint64(place.topLeft.x()) + size.width() > target->width() || qint64(place.topLeft.y()) + size.height() > target->height())
            return fail(error, QStringLiteral("素材区域和边距无法放入目标图集"));
        if (!budget(snapshot, qint64(size.width()) * size.height(), operations, error)) return false;
        if (!detachedSlots.contains(place.slot)) {
            if (!budget(snapshot, qint64(target->width()) * target->height(), operations, error) || !allocation(snapshot, candidate, place.slot, target->size(), error)) return false;
            detachedSlots.insert(place.slot);
        }
        QVector<QPair<QRect, QString>> protectedAreas;
        const QRect destination(place.topLeft, size);
        for (const auto& area : protection.value(place.slot)) {
            if (!budget(snapshot, 1, operations, error)) return false;
            if (area.first.intersects(destination)) protectedAreas.append(area);
        }
        for (int y = 0; y < size.height(); ++y) {
            if (!checkpoint(control, error)) return false;
            auto* row = target->scanLine(place.topLeft.y() + y) + place.topLeft.x() * 4;
            for (int x = 0; x < size.width(); ++x) {
                const auto point = sourcePixel(x, y, place.patch.source, place.turns);
                const auto* pixel = rgba(*source, point.x(), point.y());
                auto* out = row + x * 4;
                if (std::memcmp(out, pixel, 4) != 0) for (const auto& area : protectedAreas) {
                    if (!budget(snapshot, 1, operations, error)) return false;
                    if (area.first.contains(place.topLeft + QPoint(x, y)))
                        return fail(error, QStringLiteral("目标像素仍被未选素材 %1 使用，请换位置或将它一起移动").arg(area.second));
                }
                std::memcpy(out, pixel, 4);
            }
        }
        for (const auto& id : place.patch.ids) {
            auto points = plan.pixelCoordinates.value(id);
            if (!budget(snapshot, points.size(), operations, error)) return false;
            for (auto& point : points) point = transform(point, place.patch.source, place.topLeft, place.turns);
            if (!setPixels(snapshot, candidate, id, place.slot, std::move(points), error)) return false;
            const auto origin = transform({}, place.patch.source, place.topLeft, place.turns);
            const auto x = transform({1, 0}, place.patch.source, place.topLeft, place.turns) - origin;
            const auto y = transform({0, 1}, place.patch.source, place.topLeft, place.turns) - origin;
            rebases.insert(id, {QTransform(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y()), place.slot, gutter, place.patch.source});
        }
    }
    return CArtworkReconciler::RebasePlacements(snapshot.artwork, plan.state, candidate.state.workspace, candidate.state.materials,
        triangles(snapshot), rebases, candidate.state.artwork, error) && checkpoint(control, error);
}
}

bool CAtlasPlanVerification::Check(const Planner::Snapshot& source, const Planner::Plan& plan)
{
    if (!plan.m_verification) return false;
    const auto& trusted = plan.m_verification->plan;
    const auto& baseline = *plan.m_verification->source;
    if (source.sourceDigest != baseline.sourceDigest || source.documentRevision != baseline.documentRevision ||
        source.topologyDigest != baseline.topologyDigest || source.sources != baseline.sources || source.workspace != baseline.workspace || source.artwork != baseline.artwork ||
        source.limits.maximumDimension != baseline.limits.maximumDimension || source.limits.maximumPixels != baseline.limits.maximumPixels ||
        source.limits.maximumOperations != baseline.limits.maximumOperations || source.meshes.size() != baseline.meshes.size()) return false;
    for (int index = 0; index < source.meshes.size(); ++index) {
        const auto& first = source.meshes[index]; const auto& second = baseline.meshes[index];
        if (first.material != second.material || first.triangles != second.triangles || first.editable != second.editable || first.reason != second.reason) return false;
    }
    if (plan.state != trusted.state || plan.token != trusted.token || plan.changedMeshes != trusted.changedMeshes ||
        plan.changedSlots != trusted.changedSlots || plan.warnings != trusted.warnings || plan.images.keys() != trusted.images.keys() ||
        plan.pixelCoordinates.keys() != trusted.pixelCoordinates.keys()) return false;
    for (auto image = plan.images.cbegin(); image != plan.images.cend(); ++image)
        if (image->cacheKey() != trusted.images.value(image.key()).cacheKey()) return false;
    for (auto points = plan.pixelCoordinates.cbegin(); points != plan.pixelCoordinates.cend(); ++points) {
        const auto original = trusted.pixelCoordinates.constFind(points.key());
        if (points->size() != original->size()) return false;
        for (int index = 0; index < points->size(); ++index)
            if (!same(points->at(index).x(), original->at(index).x()) || !same(points->at(index).y(), original->at(index).y())) return false;
    }
    return true;
}
bool CAtlasPlanVerification::Capture(const Planner::Snapshot& source, const Planner::Plan& plan,
                                    Planner::Snapshot& frozenSource, Planner::Plan& frozenPlan, QString* error)
{
    if (!Check(source, plan)) return fail(error, QStringLiteral("图集草稿不是当前来源的完整已验证计划"));
    const auto verified = plan.m_verification;
    frozenSource = *verified->source; frozenPlan = verified->plan; frozenPlan.m_verification = verified;
    if (error) error->clear(); return true;
}
bool CAtlasPlanVerification::Freeze(const Planner::Snapshot& source, std::shared_ptr<const Planner::Snapshot>& result, QString* error)
{
    auto baseline = std::make_shared<Planner::Snapshot>(source);
    if (!CTextureWorkspace::Freeze(source.sources, source.workspace, baseline->workspace, error) ||
        !CModelArtwork::Freeze(source.artwork, baseline->artwork, error)) return false;
    baseline->sourceDigest = QByteArray(source.sourceDigest.constData(), source.sourceDigest.size());
    baseline->topologyDigest = QByteArray(source.topologyDigest.constData(), source.topologyDigest.size());
    QHash<QByteArray, QByteArray> blobs;
    for (auto& origin : baseline->sources) {
        origin.path = QString(origin.path.constData(), origin.path.size());
        if (!blobs.contains(origin.digest)) blobs.insert(origin.digest, QByteArray(origin.png.constData(), origin.png.size()));
        origin.png = blobs.value(origin.digest);
        origin.digest = QByteArray(origin.digest.constData(), origin.digest.size());
    }
    for (auto& atlas : baseline->workspace.atlases) {
        if (!atlas.png.isEmpty()) {
            const auto digest = QCryptographicHash::hash(atlas.png, QCryptographicHash::Sha256);
            if (!blobs.contains(digest)) blobs.insert(digest, atlas.png);
            atlas.png = blobs.value(digest);
        }
    }
    for (auto& mesh : baseline->meshes) {
        mesh.material.id = QString(mesh.material.id.constData(), mesh.material.id.size());
        if (!mesh.reason.isNull()) mesh.reason = QString(mesh.reason.constData(), mesh.reason.size());
        mesh.material.uvs = QVector<float>(mesh.material.uvs.cbegin(), mesh.material.uvs.cend());
        mesh.triangles = QVector<quint16>(mesh.triangles.cbegin(), mesh.triangles.cend());
    }
    result = std::move(baseline); return true;
}
void CAtlasPlanVerification::Seal(std::shared_ptr<const Planner::Snapshot> source, Planner::Plan& plan)
{
    auto verified = std::make_shared<Planner::Plan::Verification>(); verified->source = std::move(source); verified->plan = plan;
    verified->plan.m_verification.reset(); plan.m_verification = std::move(verified);
}
void CAtlasPlanVerification::Seal(const Planner::Plan& before, Planner::Plan& plan)
{
    auto verified = std::make_shared<Planner::Plan::Verification>(); verified->source = before.m_verification->source; verified->plan = plan;
    verified->plan.m_verification.reset(); plan.m_verification = std::move(verified);
}

bool CAtlasRepackPlanner::Initialize(const Snapshot& requested, Plan& result, QString* error, CExportControl* control) try
{
    Plan candidate;
    std::shared_ptr<const Snapshot> frozen;
    if (!token(requested, candidate.token, error) || !CAtlasPlanVerification::Freeze(requested, frozen, error)) return false;
    const auto& snapshot = *frozen;
    candidate.state.workspace = snapshot.workspace; candidate.state.artwork = snapshot.artwork;
    if (!checkpoint(control, error)) return false;
    CTextureAssets::State pngs;
    if (!CTextureWorkspace::Effective(snapshot.sources, snapshot.workspace, pngs, error)) return false;
    QHash<QByteArray, QImage> decoded;
    qint64 pixels = 0;
    for (auto it = pngs.cbegin(); it != pngs.cend(); ++it) {
        if (!checkpoint(control, error)) return false;
        const auto size = snapshot.workspace.atlases.value(it.key()).size;
        if (!dimensions(snapshot, size, error)) return false;
        const auto hash = QCryptographicHash::hash(*it, QCryptographicHash::Sha256);
        if (!decoded.contains(hash)) {
            if (qint64(size.width()) * size.height() > maximumPixels(snapshot) - pixels)
                return fail(error, QStringLiteral("图集解码超出像素预算"));
            pixels += qint64(size.width()) * size.height();
            QImage image;
            if (!CPngImage::Decode(*it, image, error) || image.size() != size) return false;
            decoded.insert(hash, image);
        }
        candidate.images.insert(it.key(), decoded.value(hash));
    }
    for (const auto& item : snapshot.meshes) {
        auto material = item.material;
        material.id = QString(item.material.id.constData(), item.material.id.size());
        material.uvs = QVector<float>(item.material.uvs.cbegin(), item.material.uvs.cend());
        candidate.state.materials.append(std::move(material));
        const auto atlas = snapshot.workspace.atlases.constFind(item.material.textureIndex);
        if (!item.material.uvs.isEmpty() && atlas == snapshot.workspace.atlases.cend()) return fail(error, QStringLiteral("素材引用了缺失的图集"));
        QVector<QPointF> points;
        for (int index = 0; index < item.material.uvs.size(); index += 2)
            points.append({double(item.material.uvs[index]) * atlas->size.width(), double(item.material.uvs[index + 1]) * atlas->size.height()});
        candidate.pixelCoordinates.insert(item.material.id, points);
    }
    std::sort(candidate.state.materials.begin(), candidate.state.materials.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (!validateFields(snapshot, candidate, error) || !checkpoint(control, error)) return false;
    CAtlasPlanVerification::Seal(std::move(frozen), candidate);
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备图集编辑快照")); }

namespace {
bool validateFields(const Snapshot& snapshot, const Plan& plan, QString* error)
{
    QByteArray expected;
    if (!token(snapshot, expected, error)) return false;
    if (expected != plan.token || plan.state.materials.size() != snapshot.meshes.size() ||
        plan.images.keys() != plan.state.workspace.atlases.keys() || plan.pixelCoordinates.size() != snapshot.meshes.size())
        return fail(error, QStringLiteral("图集草稿来源或目录已改变"));
    if (!CTextureWorkspace::Validate(snapshot.sources, plan.state.workspace, error) ||
        !CModelMaterialEdit::Validate(plan.state.materials, error) ||
        !CArtworkReconciler::ValidateLinks(plan.state.artwork, plan.state.workspace, plan.state.materials, triangles(snapshot), error)) return false;
    CTextureAssets::State pngs;
    if (!CTextureWorkspace::Effective(snapshot.sources, plan.state.workspace, pngs, error)) return false;
    QMap<QByteArray, QImage> verifiedImages; qint64 pixels = 0;
    for (auto it = plan.images.cbegin(); it != plan.images.cend(); ++it) {
        if (it->isNull() || it->format() != QImage::Format_RGBA8888 || it->size() != plan.state.workspace.atlases.value(it.key()).size)
            return fail(error, QStringLiteral("图集草稿的位图和尺寸不一致"));
        if (!dimensions(snapshot, it->size(), error)) return false;
        const auto& bytes = pngs[it.key()]; const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (!verifiedImages.contains(digest)) {
            const qint64 area = qint64(it->width()) * it->height();
            if (area > maximumPixels(snapshot) - pixels) return fail(error, QStringLiteral("候选图集的总解码像素超出预算"));
            pixels += area; QImage image;
            if (!CPngImage::Decode(bytes, image, error)) return false;
            if (!imageEquals(*it, image)) return fail(error, QStringLiteral("图集草稿的位图与保存 PNG 不一致"));
            verifiedImages.insert(digest, *it);
        }
        if (!imageEquals(*it, verifiedImages.value(digest))) return fail(error, QStringLiteral("图集草稿的位图与保存 PNG 不一致"));
    }
    for (const auto& value : plan.state.materials) {
        const auto* original = mesh(snapshot, value.id);
        const auto atlas = plan.state.workspace.atlases.constFind(value.textureIndex);
        const auto points = plan.pixelCoordinates.constFind(value.id);
        if (!original || original->material.uvs.size() != value.uvs.size() || points == plan.pixelCoordinates.cend() ||
            points->size() * 2 != value.uvs.size() || (!value.uvs.isEmpty() && atlas == plan.state.workspace.atlases.cend()))
            return fail(error, QStringLiteral("图集草稿改变了素材的顶点或目录"));
        for (int index = 0; index < points->size(); ++index)
            if (!std::isfinite(points->at(index).x()) || !std::isfinite(points->at(index).y()) ||
                !same(float(points->at(index).x() / atlas->size.width()), value.uvs[index * 2]) ||
                !same(float(points->at(index).y() / atlas->size.height()), value.uvs[index * 2 + 1]))
                return fail(error, QStringLiteral("图集草稿的像素坐标与 UV 不一致"));
    }
    if (error) error->clear(); return true;
}
}
bool CAtlasRepackPlanner::Validate(const Snapshot& snapshot, const Plan& plan, QString* error) try
{
    if (!CAtlasPlanVerification::Check(snapshot, plan)) return fail(error, QStringLiteral("图集草稿不是当前来源的完整已验证计划"));
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证图集草稿")); }

bool CAtlasRepackPlanner::SelectionBounds(const Snapshot& requestedSnapshot, const Plan& requestedPlan, const QStringList& requestedIds,
                                         int gutter, QRect& bounds, int& slot, QString* error)
try
{
    Snapshot snapshot; Plan plan;
    if (!CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error)) return false;
    QSet<QString> unique;
    if (!selected(snapshot, plan, requestedIds, unique, error)) return false;
    const auto ids = ownedIds(requestedIds);
    qint64 operations = 0; SupportCache support(snapshot, plan, gutter, operations);
    return selectionArea(plan, ids, support, bounds, slot, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法计算素材区域，原结果保持不变")); }

bool CAtlasRepackPlanner::AddAtlas(const Snapshot& requestedSnapshot, const Plan& requestedPlan, const QString& requestedName, QSize size,
                                  Plan& result, QString* error, CExportControl* control) try
{
    Snapshot snapshot; Plan plan; QString name;
    if (!atlasName(requestedName, name, error) ||
        !CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error) ||
        !dimensions(snapshot, size, error) || !checkpoint(control, error)) return false;
    qint64 operations = 0;
    if (!budget(snapshot, qint64(size.width()) * size.height(), operations, error) || !allocation(snapshot, plan, -1, size, error)) return false;
    const int slot = plan.state.workspace.atlases.isEmpty() ? 0 : plan.state.workspace.atlases.lastKey() + 1;
    if (slot >= CTextureAssets::MaximumSlots) return fail(error, QStringLiteral("图集槽数量超出范围"));
    Plan candidate = plan; QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) return fail(error, QStringLiteral("无法分配新图集"));
    image.fill(Qt::transparent);
    candidate.state.workspace.atlases.insert(slot, {QStringLiteral("atlas:") + QUuid::createUuid().toString(QUuid::WithoutBraces), name, -1, size, {}});
    candidate.images.insert(slot, image);
    return finish(snapshot, plan, std::move(candidate), result, error, control);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法创建图集")); }

bool CAtlasRepackPlanner::ResizeAtlas(const Snapshot& requestedSnapshot, const Plan& requestedPlan, int slot, QSize size,
                                     Plan& result, QString* error, CExportControl* control) try
{
    Snapshot snapshot; Plan plan;
    if (!CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error) ||
        !dimensions(snapshot, size, error) || !checkpoint(control, error)) return false;
    const auto original = plan.images.constFind(slot);
    if (original == plan.images.cend()) return fail(error, QStringLiteral("要调整的图集不存在"));
    if (size == original->size()) { result = plan; return true; }
    Plan candidate = plan;
    qint64 operations = 0;
    if (!budget(snapshot, qint64(size.width()) * size.height() * 2, operations, error) || !allocation(snapshot, plan, slot, size, error)) return false;
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) return fail(error, QStringLiteral("无法分配调整后的图集"));
    image.fill(Qt::transparent);
    QByteArray occupied(qsizetype(size.width()) * size.height(), '\0');
    const int copyWidth = std::min(size.width(), original->width()), copyHeight = std::min(size.height(), original->height());
    if (!budget(snapshot, qint64(copyWidth) * copyHeight, operations, error)) return false;
    for (int y = 0; y < copyHeight; ++y) {
        if (!checkpoint(control, error)) return false;
        std::memcpy(image.scanLine(y), original->constScanLine(y), size_t(copyWidth) * 4);
        std::memset(occupied.data() + qsizetype(y) * size.width(), 1, size_t(copyWidth));
    }
    QSet<QString> processed; SupportCache support(snapshot, plan, 1, operations);
    for (const auto& value : plan.state.materials) if (value.textureIndex == slot && hasSupport(plan, value)) {
        if (!mesh(snapshot, value.id)->editable) return fail(error, QStringLiteral("调整尺寸会修改已锁定素材 %1 的 UV").arg(value.id));
        const auto points = plan.pixelCoordinates.value(value.id);
        if (size.width() < original->width() || size.height() < original->height())
            for (const auto& p : points) if (p.x() < 0 || p.y() < 0 || p.x() > size.width() || p.y() > size.height())
                return fail(error, QStringLiteral("缩小后的图集无法容纳素材 %1，请先重排").arg(value.id));
        QRect bounds;
        if (!support.Bounds(value.id, bounds, error)) return false;
        const auto key = QStringLiteral("%1,%2,%3,%4").arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height());
        if (!processed.contains(key)) {
            processed.insert(key);
            if (!budget(snapshot, qint64(bounds.width()) * bounds.height(), operations, error)) return false;
            for (qint64 y = bounds.y(); y < qint64(bounds.y()) + bounds.height(); ++y) {
                if (!checkpoint(control, error)) return false;
                const int ty = repeat(y, size.height());
                auto* row = image.scanLine(ty); auto* flags = occupied.data() + qsizetype(ty) * size.width();
                for (qint64 x = bounds.x(); x < qint64(bounds.x()) + bounds.width(); ++x) {
                    const int tx = repeat(x, size.width()); const auto* pixel = rgba(*original, x, y);
                    if (flags[tx] && std::memcmp(row + tx * 4, pixel, 4) != 0)
                        return fail(error, QStringLiteral("新尺寸会让不同的重复采样区域重叠，请先重排素材"));
                    std::memcpy(row + tx * 4, pixel, 4); flags[tx] = 1;
                }
            }
        }
    }
    candidate.state.workspace.atlases[slot].size = size;
    candidate.images[slot] = std::move(image);
    QMap<QString, CArtworkReconciler::Rebase> rebases;
    for (const auto& value : plan.state.materials) if (value.textureIndex == slot && hasSupport(plan, value)) {
        if (!setPixels(snapshot, candidate, value.id, slot, plan.pixelCoordinates.value(value.id), error)) return false;
        QRect patch; if (!support.Bounds(value.id, patch, error)) return false;
        const auto binding = plan.state.artwork.bindings.constFind(value.id);
        const int gutter = binding == plan.state.artwork.bindings.cend() || binding->status == CModelArtwork::BindingStatus::Detached
            ? 1 : plan.state.artwork.placements.value(binding->placementId).gutter;
        rebases.insert(value.id, {QTransform(), slot, gutter, patch});
    }
    if (!CArtworkReconciler::RebasePlacements(snapshot.artwork, plan.state, candidate.state.workspace, candidate.state.materials,
        triangles(snapshot), rebases, candidate.state.artwork, error)) return false;
    return finish(snapshot, plan, std::move(candidate), result, error, control);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法调整图集尺寸")); }

bool CAtlasRepackPlanner::Move(const Snapshot& requestedSnapshot, const Plan& requestedPlan, const QStringList& requestedIds, int targetSlot,
                              QPoint topLeft, int quarterTurns, int gutter, Plan& result, QString* error, CExportControl* control) try
{
    Snapshot snapshot; Plan plan; QSet<QString> unique;
    if (!CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error) ||
        !selected(snapshot, plan, requestedIds, unique, error)) return false;
    const auto ids = ownedIds(requestedIds);
    if (!checkpoint(control, error)) return false;
    if (quarterTurns < 0 || quarterTurns > 3 || !plan.state.workspace.atlases.contains(targetSlot))
        return fail(error, QStringLiteral("目标图集或旋转角度无效"));
    QRect bounds; int slot; qint64 operations = 0;
    SupportCache support(snapshot, plan, gutter, operations);
    if (!selectionArea(plan, ids, support, bounds, slot, error)) return false;
    if (targetSlot == slot && topLeft == bounds.topLeft() && quarterTurns == 0) { result = plan; return true; }
    Plan candidate = plan;
    if (!render(snapshot, plan, {{{slot, bounds, ids}, targetSlot, topLeft, quarterTurns}}, {}, gutter, candidate, operations, error, control)) return false;
    return finish(snapshot, plan, std::move(candidate), result, error, control);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法移动图集素材")); }

bool CAtlasRepackPlanner::Pack(const Snapshot& requestedSnapshot, const Plan& requestedPlan, const QStringList& requestedIds,
                              const QVector<int>& requestedTargetSlots, int gutter, bool rotate90, Plan& result,
                              QString* error, CExportControl* control) try
{
    Snapshot snapshot; Plan plan; QSet<QString> unique;
    if (!CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error) ||
        !selected(snapshot, plan, requestedIds, unique, error)) return false;
    if (gutter < 1 || gutter > 64 || requestedTargetSlots.isEmpty() || requestedTargetSlots.size() > plan.images.size())
        return fail(error, QStringLiteral("请选择有效数量的目标图集和 1–64 像素边距"));
    QSet<int> targets;
    for (int slot : requestedTargetSlots) {
        if (targets.contains(slot) || !plan.images.contains(slot)) return fail(error, QStringLiteral("重排目标重复或不存在"));
        targets.insert(slot);
    }
    const auto ids = ownedIds(requestedIds);
    if (!checkpoint(control, error)) return false;
    qint64 operations = 0; SupportCache support(snapshot, plan, gutter, operations);
    QVector<Patch> patches;
    for (const auto& id : ids) {
        QRect bounds;
        if (!support.Bounds(id, bounds, error)) return false;
        patches.append({material(plan, id)->textureIndex, bounds, {id}});
    }
    QVector<int> parent(patches.size()); std::iota(parent.begin(), parent.end(), 0);
    QVector<int> rank(patches.size());
    const auto root = [&](int index) {
        while (parent[index] != index) {
            if (!budget(snapshot, 1, operations, error)) return -1;
            parent[index] = parent[parent[index]]; index = parent[index];
        }
        return index;
    };
    for (int first = 0; first < patches.size(); ++first) {
        if (!checkpoint(control, error)) return false;
        for (int second = first + 1; second < patches.size(); ++second) {
            if (!budget(snapshot, 1, operations, error)) return false;
            if (patches[first].slot == patches[second].slot && patches[first].source.intersects(patches[second].source)) {
                int firstRoot = root(first), secondRoot = root(second);
                if (firstRoot < 0 || secondRoot < 0) return false;
                if (firstRoot != secondRoot) {
                    if (rank[firstRoot] < rank[secondRoot]) std::swap(firstRoot, secondRoot);
                    parent[secondRoot] = firstRoot;
                    if (rank[firstRoot] == rank[secondRoot]) ++rank[firstRoot];
                }
            }
        }
    }
    QMap<int, Patch> merged;
    for (int index = 0; index < patches.size(); ++index) {
        const int key = root(index);
        if (key < 0 || !budget(snapshot, 1, operations, error)) return false;
        if (!merged.contains(key)) merged[key] = patches[index];
        else {
            QRect area;
            if (!unite(merged[key].source, patches[index].source, area, error)) return false;
            merged[key].source = area; merged[key].ids += patches[index].ids;
        }
    }
    patches.clear(); for (auto patch : merged) { std::sort(patch.ids.begin(), patch.ids.end()); patches.append(std::move(patch)); }
    std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) {
        const auto key = [](const Patch& p) { return std::make_tuple(-qint64(std::max(p.source.width(), p.source.height())),
            -qint64(p.source.width()) * p.source.height(), p.ids.first()); };
        return key(a) < key(b);
    });
    QMap<int, QVector<QRect>> free;
    for (int slot : targets) free[slot] = {QRect(QPoint(), plan.images[slot].size())};
    QVector<Placement> placements;
    for (const auto& patch : patches) {
        if (!checkpoint(control, error)) return false;
        std::tuple<int, int, int, int, int, int> best{INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX};
        int chosen = -1, turn = 0; QRect destination;
        for (auto atlas = free.cbegin(); atlas != free.cend(); ++atlas) for (const auto& area : *atlas)
            for (int rotation = 0; rotation <= int(rotate90); ++rotation) {
                if (!budget(snapshot, 1, operations, error)) return false;
                const auto size = rotatedSize(patch.source.size(), rotation);
                if (size.width() > area.width() || size.height() > area.height()) continue;
                const int dx = area.width() - size.width(), dy = area.height() - size.height();
                const auto score = std::make_tuple(std::min(dx, dy), std::max(dx, dy), atlas.key(), area.y(), area.x(), rotation);
                if (score < best) { best = score; chosen = atlas.key(); turn = rotation; destination = QRect(area.topLeft(), size); }
            }
        if (chosen < 0) return fail(error, QStringLiteral("素材 %1 与边距无法放入所选图集，请增加图集或尺寸").arg(patch.ids.join(QStringLiteral("、"))));
        placements.append({patch, chosen, destination.topLeft(), turn});
        QVector<QRect> next;
        for (const auto& area : free[chosen]) {
            if (!budget(snapshot, 1, operations, error)) return false;
            if (!area.intersects(destination)) { next.append(area); continue; }
            if (destination.x() > area.x()) next.append({area.x(), area.y(), destination.x() - area.x(), area.height()});
            if (destination.right() < area.right()) next.append({destination.right() + 1, area.y(), area.right() - destination.right(), area.height()});
            if (destination.y() > area.y()) next.append({area.x(), area.y(), area.width(), destination.y() - area.y()});
            if (destination.bottom() < area.bottom()) next.append({area.x(), destination.bottom() + 1, area.width(), area.bottom() - destination.bottom()});
        }
        QVector<QRect> pruned;
        for (int index = 0; index < next.size(); ++index) {
            bool contained = false;
            for (int other = 0; other < next.size(); ++other) {
                if (!budget(snapshot, 1, operations, error)) return false;
                if (index != other && next[other].contains(next[index]) && (next[index] != next[other] || other < index)) { contained = true; break; }
            }
            if (!contained && !next[index].isEmpty()) pruned.append(next[index]);
        }
        free[chosen] = std::move(pruned);
    }
    Plan candidate = plan;
    if (!render(snapshot, plan, placements, targets, gutter, candidate, operations, error, control)) return false;
    return finish(snapshot, plan, std::move(candidate), result, error, control);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法完成图集自动排布")); }

bool CAtlasRepackPlanner::RemoveAtlas(const Snapshot& requestedSnapshot, const Plan& requestedPlan, int slot, Plan& result,
                                     QString* error, CExportControl* control) try
{
    Snapshot snapshot; Plan plan;
    if (!CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error) || !checkpoint(control, error)) return false;
    const auto atlas = plan.state.workspace.atlases.constFind(slot);
    if (atlas == plan.state.workspace.atlases.cend() || atlas->sourceSlot >= 0 || slot != plan.state.workspace.atlases.lastKey())
        return fail(error, QStringLiteral("只能删除末尾的生成图集；原始图集需保留来源身份"));
    for (const auto& value : plan.state.materials) if (value.textureIndex == slot && hasSupport(plan, value))
        return fail(error, QStringLiteral("图集仍被素材 %1 引用，请先移动素材").arg(value.id));
    Plan candidate = plan; candidate.state.workspace.atlases.remove(slot); candidate.images.remove(slot);
    return finish(snapshot, plan, std::move(candidate), result, error, control);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法删除图集")); }

bool CAtlasRepackPlanner::RenameAtlas(const Snapshot& requestedSnapshot, const Plan& requestedPlan, int slot, const QString& requestedName,
                                     Plan& result, QString* error) try
{
    Snapshot snapshot; Plan plan; QString name;
    if (!atlasName(requestedName, name, error) || !CAtlasPlanVerification::Capture(requestedSnapshot, requestedPlan, snapshot, plan, error)) return false;
    if (!plan.state.workspace.atlases.contains(slot)) return fail(error, QStringLiteral("图集不存在"));
    Plan candidate = plan; candidate.state.workspace.atlases[slot].name = name;
    return finish(snapshot, plan, std::move(candidate), result, error, nullptr);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法重命名图集")); }
}
