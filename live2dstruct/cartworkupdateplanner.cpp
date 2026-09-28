#include "cartworkupdateplanner.h"
#include "cartworkreconciler.h"
#include "cartworksampler.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace live2dstruct {
struct CArtworkUpdatePlanner::Plan::Verification
{
    Snapshot source;
    Request request;
    Plan plan;
};
struct CArtworkUpdateVerification
{
    static void Seal(CArtworkUpdatePlanner::Snapshot source, CArtworkUpdatePlanner::Request request, CArtworkUpdatePlanner::Plan& plan);
    static bool Check(const CArtworkUpdatePlanner::Snapshot& source, const CArtworkUpdatePlanner::Plan& plan);
    static CArtworkSourceImport::SourceCandidate Source(const CArtworkUpdatePlanner::Plan& plan);
};
namespace {
using Planner = CArtworkUpdatePlanner;
using Artwork = CModelArtwork;
using Mesh = CAtlasRepackPlanner::Mesh;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
QByteArray owned(const QByteArray& value) { return QByteArray(value.constData(), value.size()); }
bool checkpoint(CExportControl* control, QString* error)
{ return !control || control->Checkpoint() || fail(error, control->Error().isEmpty() ? QStringLiteral("已取消素材更新") : control->Error()); }
QTransform inverse(const QTransform& value)
{
    const double scale = std::max({std::abs(value.m11()), std::abs(value.m12()), std::abs(value.m21()), std::abs(value.m22())});
    const double a = value.m11() / scale, b = value.m12() / scale, c = value.m21() / scale, d = value.m22() / scale;
    const double determinant = a * d - b * c;
    const double x = (d / determinant) / scale, y = (-b / determinant) / scale, z = (-c / determinant) / scale, w = (a / determinant) / scale;
    return QTransform(x, y, z, w, -x * value.dx() - z * value.dy(), -y * value.dx() - w * value.dy());
}
bool same(const Planner::Snapshot& first, const Planner::Snapshot& second)
{
    if (first.sourceDigest != second.sourceDigest || first.documentRevision != second.documentRevision || first.sources != second.sources ||
        first.baseline != second.baseline || first.meshes.size() != second.meshes.size() || first.limits.maximumDimension != second.limits.maximumDimension ||
        first.limits.maximumPixels != second.limits.maximumPixels || first.limits.maximumOperations != second.limits.maximumOperations) return false;
    for (int index = 0; index < first.meshes.size(); ++index) {
        const auto& a = first.meshes[index]; const auto& b = second.meshes[index];
        if (a.material != b.material || a.triangles != b.triangles || a.editable != b.editable || a.reason != b.reason) return false;
    }
    return true;
}
bool prepare(const Planner::Snapshot& input, const Planner::Request& request, Planner::Snapshot& snapshot, Planner::Request& frozen,
             QMap<QString, Mesh>& meshes, CArtworkReconciler::Triangles& triangles, QByteArray& token, QString* error)
{
    if (input.sourceDigest.size() != 32 || input.limits.maximumDimension <= 0 || input.limits.maximumPixels <= 0 || input.limits.maximumOperations <= 0 ||
        request.decisions.size() > Artwork::MaximumRecords || request.familyName.size() > 32768 || request.familyName.contains(QChar::Null) ||
        request.incoming.diagnostics.size() > Artwork::MaximumRecords) return fail(error, QStringLiteral("素材更新来源或预算无效"));
    snapshot = input; snapshot.sourceDigest = owned(input.sourceDigest);
    if (!CTextureAssets::ValidateCatalog(input.sources, error)) return false;
    QMap<QByteArray, QByteArray> blobs;
    for (auto& source : snapshot.sources) {
        source.path = owned(source.path);
        const auto previous = blobs.constFind(source.digest);
        if (previous == blobs.cend()) blobs.insert(owned(source.digest), owned(source.png));
        else if (*previous != source.png) return fail(error, QStringLiteral("素材来源摘要对应不同内容"));
        source.png = blobs.value(source.digest); source.digest = owned(source.digest);
    }
    if (!CTextureWorkspace::Freeze(snapshot.sources, input.baseline.workspace, snapshot.baseline.workspace, error) ||
        !Artwork::Freeze(input.baseline.artwork, snapshot.baseline.artwork, error) || !CModelMaterialEdit::Validate(input.baseline.materials, error)) return false;
    snapshot.baseline.materials.clear();
    for (const auto& material : input.baseline.materials) snapshot.baseline.materials.append({owned(material.id), material.textureIndex, QVector<float>(material.uvs.cbegin(), material.uvs.cend())});
    for (auto& mesh : snapshot.meshes) {
        mesh.material.id = owned(mesh.material.id); mesh.material.uvs = QVector<float>(mesh.material.uvs.cbegin(), mesh.material.uvs.cend());
        mesh.triangles = QVector<quint16>(mesh.triangles.cbegin(), mesh.triangles.cend()); mesh.reason = owned(mesh.reason);
        if (meshes.contains(mesh.material.id)) return fail(error, QStringLiteral("素材更新的网格目录重复"));
        meshes.insert(mesh.material.id, mesh); triangles.insert(mesh.material.id, mesh.triangles);
    }
    if (meshes.size() != snapshot.baseline.materials.size()) return fail(error, QStringLiteral("素材更新需要完整网格目录"));
    for (const auto& material : snapshot.baseline.materials)
        if (!meshes.contains(material.id) || meshes.value(material.id).material != material ||
            (!material.uvs.isEmpty() && !snapshot.baseline.workspace.atlases.contains(material.textureIndex)))
            return fail(error, QStringLiteral("素材更新网格与材质或图集快照不一致"));
    if (!CArtworkReconciler::ValidateLinks(snapshot.baseline.artwork, snapshot.baseline.workspace, snapshot.baseline.materials, triangles, error)) return false;
    Artwork::State incoming, checked;
    incoming.images = request.incoming.images; incoming.revisions.insert(request.incoming.revision.id, request.incoming.revision);
    incoming.families.insert(request.incoming.revision.familyId, {request.incoming.revision.familyId, request.familyName,
        request.incoming.kind, request.incoming.revision.id});
    if (!Artwork::Freeze(incoming, checked, error)) return false;
    frozen.incoming.revision = checked.revisions.first(); frozen.incoming.images = checked.images; frozen.incoming.kind = request.incoming.kind;
    frozen.familyName = owned(request.familyName);
    qint64 textBytes = 0;
    for (const auto& diagnostic : request.incoming.diagnostics) {
        if (diagnostic.size() > 32768 || diagnostic.contains(QChar::Null) || diagnostic.size() * 2 > Artwork::MaximumMetadataBytes - textBytes)
            return fail(error, QStringLiteral("素材来源诊断超过范围"));
        textBytes += diagnostic.size() * 2; frozen.incoming.diagnostics.append(owned(diagnostic));
    }
    QMap<QString, Planner::Decision> decisions;
    for (const auto& decision : request.decisions) {
        if (!meshes.contains(decision.meshId) || decisions.contains(decision.meshId) || decision.layerId.size() > 1024 || decision.layerId.contains(QChar::Null) ||
            (decision.action != Planner::Action::Keep && decision.action != Planner::Action::Associate && decision.action != Planner::Action::Update && decision.action != Planner::Action::Detach))
            return fail(error, QStringLiteral("素材更新决策引用重复或未知对象"));
        if (!Artwork::ValidateTransform(decision.atlasToDocument, error)) return false;
        auto copy = decision; copy.meshId = owned(decision.meshId); copy.layerId = owned(decision.layerId); decisions.insert(copy.meshId, copy);
    }
    for (const auto& decision : decisions) frozen.decisions.append(decision);
    QByteArray workspace, artwork, source;
    if (!CTextureWorkspace::Fingerprint(snapshot.sources, snapshot.baseline.workspace, workspace, error) ||
        !Artwork::Fingerprint(snapshot.baseline.artwork, artwork, error) || !Artwork::Fingerprint(checked, source, error)) return false;
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(1) << snapshot.sourceDigest << snapshot.documentRevision << workspace << artwork << source << frozen.familyName << frozen.incoming.diagnostics;
    stream << snapshot.limits.maximumDimension << snapshot.limits.maximumPixels << snapshot.limits.maximumOperations;
    for (const auto& mesh : meshes) stream << CModelMaterialEdit::Fingerprint({{mesh.material.id, mesh.material}}) << mesh.triangles << mesh.editable << mesh.reason;
    for (const auto& decision : frozen.decisions) stream << decision.meshId << int(decision.action) << decision.layerId << decision.atlasToDocument << decision.confirmed << decision.applyLayerOpacity;
    if (stream.status() != QDataStream::Ok) return fail(error, QStringLiteral("无法计算素材更新快照标识"));
    token = hash(bytes); return true;
}
struct Budget {
    qint64 pixels = 0, encoded = 0, operations = 0;
    qint64 maximumPixels, maximumOperations;
    QMap<QByteArray, QByteArray> blobs;
    QMap<QByteArray, QSize> images;
    bool work(qint64 amount, QString* error)
    {
        if (amount < 0 || amount > maximumOperations - operations) return fail(error, QStringLiteral("素材更新超过计算预算"));
        operations += amount; return true;
    }
    bool reserve(qint64 amount, QString* error)
    {
        if (amount < 0 || amount > maximumPixels - pixels) return fail(error, QStringLiteral("来源素材与候选图集超过总像素预算"));
        pixels += amount; return true;
    }
    bool bytes(const QByteArray& value, const QSize& size, QString* error)
    {
        if (value.isEmpty()) return true;
        const auto key = hash(value);
        if (!blobs.contains(key)) {
            if (value.size() > Artwork::MaximumBytes - encoded) return fail(error, QStringLiteral("来源素材与候选图集超过总字节预算"));
            encoded += value.size(); blobs.insert(key, value);
        } else if (blobs.value(key) != value) return fail(error, QStringLiteral("素材摘要对应不同内容"));
        if (size.isValid() && !size.isEmpty()) {
            if (images.contains(key)) return images.value(key) == size || fail(error, QStringLiteral("素材摘要对应不同图像尺寸"));
            if (!reserve(qint64(size.width()) * size.height(), error)) return false;
            images.insert(key, size);
        }
        return true;
    }
};
struct Group {
    QString layerId;
    QTransform linear;
    Artwork::Boundary boundary = Artwork::Boundary::Transparent;
    int gutter = 1;
    bool opacity = true;
    QStringList meshes;
    QMap<QString, QVector<QPointF>> coordinates;
};
QByteArray groupKey(const Group& group)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << group.layerId << group.linear << int(group.boundary) << group.gutter << group.opacity; return hash(bytes);
}
bool exactTransform(const QTransform& transform)
{
    const double a = transform.m11(), b = transform.m12(), c = transform.m21(), d = transform.m22();
    const bool axes = ((std::abs(a) == 1 && b == 0 && c == 0 && std::abs(d) == 1) ||
                       (a == 0 && std::abs(b) == 1 && std::abs(c) == 1 && d == 0));
    return axes && std::floor(transform.dx()) == transform.dx() && std::floor(transform.dy()) == transform.dy();
}
bool extent(const Group& group, const Artwork::Revision& revision, const Artwork::Layer& layer, QRectF& domain,
            QTransform& placement, QSize& size, const Planner::Snapshot& snapshot, QString* error)
{
    QVector<QPointF> points;
    const double left = layer.bounds.x(), top = layer.bounds.y(), right = left + layer.bounds.width(), bottom = top + layer.bounds.height();
    for (const auto& point : {QPointF(left, top), QPointF(right, top), QPointF(left, bottom), QPointF(right, bottom)}) points.append(revision.documentToFamily.map(point));
    for (const auto& coordinates : group.coordinates) points += coordinates;
    double x = points.first().x(), y = points.first().y(), r = x, b = y;
    for (const auto& point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail(error, QStringLiteral("素材来源范围包含非有限坐标"));
        x = std::min(x, point.x()); y = std::min(y, point.y()); r = std::max(r, point.x()); b = std::max(b, point.y());
    }
    domain = QRectF(x, y, r > x ? r - x : 1, b > y ? b - y : 1);
    const auto bounds = group.linear.mapRect(domain);
    const double minX = std::floor(bounds.left()), minY = std::floor(bounds.top());
    const double width = std::ceil(bounds.right()) - minX + group.gutter * 2, height = std::ceil(bounds.bottom()) - minY + group.gutter * 2;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > snapshot.limits.maximumDimension || height > snapshot.limits.maximumDimension ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() || width * height > snapshot.limits.maximumPixels)
        return fail(error, QStringLiteral("完整源图层与网格坐标无法放入图集预算"));
    placement = QTransform(group.linear.m11(), group.linear.m12(), group.linear.m21(), group.linear.m22(), group.gutter - minX, group.gutter - minY);
    size = QSize(int(width), int(height)); return Artwork::ValidateTransform(placement, error);
}
long double cross(QPointF a, QPointF b, QPointF p)
{ return (static_cast<long double>(b.x()) - a.x()) * (static_cast<long double>(p.y()) - a.y()) - (static_cast<long double>(b.y()) - a.y()) * (static_cast<long double>(p.x()) - a.x()); }
bool uncovered(const QImage& image, const Group& group, const Artwork::Revision& revision, const Artwork::Layer& layer,
               const QMap<QString, Mesh>& meshes, Budget& budget, bool& result, QString* error, CExportControl* control)
{
    result = false;
    if (layer.bounds.isEmpty() || (group.opacity && layer.opacity == 0)) return true;
    const qint64 pixels = qint64(image.width()) * image.height();
    const qint64 scratchPixels = (pixels + 3) / 4;
    if (pixels > std::numeric_limits<int>::max() || !budget.work(pixels, error) || !budget.reserve(scratchPixels, error)) return false;
    const auto releaseScratch = qScopeGuard([&] { budget.pixels -= scratchPixels; });
    QByteArray covered(pixels, '\0'); const auto toLocal = inverse(revision.documentToFamily) * QTransform::fromTranslate(-layer.bounds.x(), -layer.bounds.y());
    const auto raster = [&](QPointF a, QPointF b, QPointF c) {
        const auto area = cross(a, b, c);
        if (!std::isfinite(area)) return fail(error, QStringLiteral("素材覆盖计算超出有限范围"));
        if (area == 0) return true;
        const double x0 = std::max(0.0, std::ceil(std::min({a.x(), b.x(), c.x()}) - .5)), y0 = std::max(0.0, std::ceil(std::min({a.y(), b.y(), c.y()}) - .5));
        const double x1 = std::min(double(image.width() - 1), std::floor(std::max({a.x(), b.x(), c.x()}) - .5)), y1 = std::min(double(image.height() - 1), std::floor(std::max({a.y(), b.y(), c.y()}) - .5));
        if (x1 < x0 || y1 < y0) return true;
        if (!budget.work(qint64(x1 - x0 + 1) * qint64(y1 - y0 + 1), error)) return false;
        for (int y = int(y0); y <= int(y1); ++y) {
            if (!checkpoint(control, error)) return false;
            for (int x = int(x0); x <= int(x1); ++x) {
                const QPointF p(x + .5, y + .5); const auto ab = cross(a, b, p), bc = cross(b, c, p), ca = cross(c, a, p);
                if (!std::isfinite(ab) || !std::isfinite(bc) || !std::isfinite(ca)) return fail(error, QStringLiteral("素材覆盖计算超出有限范围"));
                if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0)) covered[qsizetype(y) * image.width() + x] = 1;
            }
        }
        return true;
    };
    for (const auto& id : group.meshes) {
        QVector<QPointF> points;
        for (auto point : group.coordinates.value(id)) {
            const auto local = toLocal.map(point);
            if (!std::isfinite(local.x()) || !std::isfinite(local.y())) return fail(error, QStringLiteral("素材覆盖坐标超出有限范围"));
            points.append(local);
        }
        const auto indices = meshes.value(id).triangles;
        for (int triangle = 0; triangle < indices.size(); triangle += 3) {
            const auto a = points[indices[triangle]], b = points[indices[triangle + 1]], c = points[indices[triangle + 2]];
            if (group.boundary == Artwork::Boundary::Transparent) { if (!raster(a, b, c)) return false; continue; }
            const double x0 = std::ceil((.5 - std::max({a.x(), b.x(), c.x()})) / image.width());
            const double x1 = std::floor((image.width() - .5 - std::min({a.x(), b.x(), c.x()})) / image.width());
            const double y0 = std::ceil((.5 - std::max({a.y(), b.y(), c.y()})) / image.height());
            const double y1 = std::floor((image.height() - .5 - std::min({a.y(), b.y(), c.y()})) / image.height());
            if (x1 < x0 || y1 < y0) continue;
            const double copies = (x1 - x0 + 1) * (y1 - y0 + 1);
            if (std::max({std::abs(x0), std::abs(x1), std::abs(y0), std::abs(y1)}) > 1e12 || !std::isfinite(copies) ||
                copies > budget.maximumOperations - budget.operations || !budget.work(qint64(copies), error))
                return fail(error, QStringLiteral("重复采样的素材覆盖范围超过计算预算"));
            for (qint64 y = qint64(y0); y <= qint64(y1); ++y) for (qint64 x = qint64(x0); x <= qint64(x1); ++x) {
                const QPointF shift(double(x) * image.width(), double(y) * image.height());
                if (!raster(a + shift, b + shift, c + shift)) return false;
            }
        }
    }
    for (int y = 0; y < image.height(); ++y) {
        if (!checkpoint(control, error)) return false;
        for (int x = 0; x < image.width(); ++x) if (image.constScanLine(y)[x * 4 + 3] && !covered[qsizetype(y) * image.width() + x]) { result = true; return true; }
    }
    return true;
}
}

bool CArtworkUpdatePlanner::Build(const Snapshot& input, const Request& requested, Plan& result, QString* error, CExportControl* control) try
{
    if (error) error->clear();
    if (control && control->IsCancellationRequested() && !checkpoint(control, error)) return false;
    Snapshot snapshot; Request request; Plan candidate;
    QMap<QString, Mesh> meshes; CArtworkReconciler::Triangles triangles;
    if (!prepare(input, requested, snapshot, request, meshes, triangles, candidate.token, error) || !checkpoint(control, error)) return false;
    candidate.state = snapshot.baseline; candidate.warnings = request.incoming.diagnostics;
    auto& artwork = candidate.state.artwork; const auto& revision = request.incoming.revision;
    const auto originalFamily = artwork.families.constFind(revision.familyId);
    if (originalFamily != artwork.families.cend() && originalFamily->kind != request.incoming.kind)
        return fail(error, QStringLiteral("已有素材来源类型不能被新的修订改写"));
    const auto previous = artwork.revisions.constFind(revision.id);
    if (previous != artwork.revisions.cend() && !(*previous == revision)) return fail(error, QStringLiteral("已有素材修订标识对应不同内容，不能改写历史来源"));
    for (auto image = request.incoming.images.cbegin(); image != request.incoming.images.cend(); ++image) {
        const auto existing = artwork.images.constFind(image.key());
        if (existing != artwork.images.cend() && !(*existing == *image)) return fail(error, QStringLiteral("已有素材图像摘要对应不同内容"));
        artwork.images.insert(image.key(), *image);
    }
    artwork.revisions.insert(revision.id, revision);
    auto family = artwork.families.find(revision.familyId);
    if (family == artwork.families.end()) artwork.families.insert(revision.familyId, {revision.familyId, request.familyName, request.incoming.kind, revision.id});
    else { family->name = request.familyName; family->currentRevision = revision.id; }
    QMap<QString, Artwork::Layer> layers; for (const auto& layer : revision.layers) layers.insert(layer.id, layer);
    QMap<QString, QMap<QString, int>> layerIndices;
    QMap<QString, QMap<quint32, int>> externalCounts;
    for (const auto& source : artwork.revisions) for (int index = 0; index < source.layers.size(); ++index) {
        const auto& layer = source.layers[index]; layerIndices[source.id].insert(layer.id, index);
        if (layer.externalId) ++externalCounts[source.id][*layer.externalId];
    }
    QMap<QString, int> materialRows, impactRows;
    for (int index = 0; index < candidate.state.materials.size(); ++index) materialRows.insert(candidate.state.materials[index].id, index);
    QMap<QByteArray, Group> groups;
    for (const auto& decision : request.decisions) {
        if (!checkpoint(control, error)) return false;
        const auto& mesh = meshes[decision.meshId]; const auto binding = artwork.bindings.constFind(decision.meshId);
        Impact impact{decision.meshId, decision.action, decision.layerId, mesh.material.textureIndex, mesh.material.textureIndex, {}};
        impactRows.insert(decision.meshId, candidate.impacts.size());
        if (decision.action == Action::Keep) { impact.notes.append(QStringLiteral("保留当前素材修订与采样")); candidate.impacts.append(impact); continue; }
        if (!mesh.editable) return fail(error, mesh.reason.isEmpty() ? QStringLiteral("网格已锁定：%1").arg(decision.meshId) : mesh.reason);
        if (decision.action == Action::Detach) {
            if (binding != artwork.bindings.cend()) {
                auto& detached = artwork.bindings[decision.meshId]; detached.status = Artwork::BindingStatus::Detached;
                detached.detachedReason = QStringLiteral("用户解除素材关联，保留当前图集与原来源记录");
            }
            impact.notes.append(QStringLiteral("保持当前外观，停止自动更新来源")); candidate.impacts.append(impact); continue;
        }
        if (mesh.material.uvs.isEmpty()) return fail(error, QStringLiteral("没有 UV 顶点的网格不能关联源图层"));
        if (decision.action == Action::Update && (binding == artwork.bindings.cend() || binding->status != Artwork::BindingStatus::Linked))
            return fail(error, QStringLiteral("更新原稿需要有效的既有素材关联：%1").arg(decision.meshId));
        if (decision.action == Action::Update && artwork.materials.value(artwork.placements.value(binding->placementId).materialId).familyId != revision.familyId)
            return fail(error, QStringLiteral("更新不能将既有素材静默切换到其他来源"));
        if (decision.action == Action::Update && decision.layerId.isEmpty()) {
            impact.notes.append(QStringLiteral("没有指定新修订中的对应图层，保留旧素材修订")); candidate.impacts.append(impact); continue;
        }
        const auto layer = layers.constFind(decision.layerId);
        if (layer == layers.cend() || layer->kind != Artwork::LayerKind::Raster || !layer->pixelEquivalent)
            return fail(error, QStringLiteral("请选择已支持的栅格源图层：%1").arg(decision.layerId));
        Group group; group.layerId = layer->id; group.opacity = decision.applyLayerOpacity;
        QVector<QPointF> coordinates; QTransform mapping; bool confirmed = decision.confirmed;
        if (decision.action == Action::Associate) {
            const auto atlas = snapshot.baseline.workspace.atlases.constFind(mesh.material.textureIndex);
            if (atlas == snapshot.baseline.workspace.atlases.cend()) return fail(error, QStringLiteral("原网格图集不存在"));
            const auto toFamily = decision.atlasToDocument * revision.documentToFamily;
            if (!Artwork::ValidateTransform(toFamily, error)) return false; mapping = inverse(toFamily);
            for (int vertex = 0; vertex < mesh.material.uvs.size(); vertex += 2)
                coordinates.append(toFamily.map(QPointF(double(mesh.material.uvs[vertex]) * atlas->size.width(), double(mesh.material.uvs[vertex + 1]) * atlas->size.height())));
            group.boundary = artwork.families.value(revision.familyId).kind == Artwork::SourceKind::AtlasSnapshot ? Artwork::Boundary::Repeat : Artwork::Boundary::Transparent;
        } else {
            const auto placement = artwork.placements.value(binding->placementId); const auto material = artwork.materials.value(placement.materialId);
            if (material.familyId != revision.familyId) return fail(error, QStringLiteral("更新不能将既有素材静默切换到其他来源"));
            const auto oldRevision = artwork.revisions.constFind(material.revisionId);
            const int oldIndex = layerIndices.value(material.revisionId).value(material.layerId, -1);
            if (oldRevision == artwork.revisions.cend() || oldIndex < 0) return fail(error, QStringLiteral("既有素材原图层不存在"));
            const auto& oldLayer = oldRevision->layers[oldIndex];
            confirmed |= oldRevision->id == revision.id ? oldLayer.id == layer->id : oldLayer.externalId && layer->externalId && oldLayer.externalId == layer->externalId &&
                externalCounts.value(oldRevision->id).value(*oldLayer.externalId) == 1 && externalCounts.value(revision.id).value(*layer->externalId) == 1;
            coordinates = binding->familyCoordinates; mapping = placement.familyToAtlas; group.gutter = placement.gutter; group.boundary = material.boundary;
        }
        if (!confirmed) candidate.unconfirmed.insert(decision.meshId);
        group.linear = QTransform(mapping.m11(), mapping.m12(), mapping.m21(), mapping.m22(), 0, 0);
        if (!Artwork::ValidateTransform(group.linear, error)) return false;
        const auto key = groupKey(group);
        if (!groups.contains(key)) groups.insert(key, group);
        groups[key].meshes.append(decision.meshId); groups[key].coordinates.insert(decision.meshId, coordinates);
        impact.notes.append(QStringLiteral("从原始源图生成独立图集，保留模型网格形状")); candidate.impacts.append(impact);
    }
    if (groups.size() > CTextureAssets::MaximumSlots - candidate.state.workspace.atlases.size()) return fail(error, QStringLiteral("新增图集数量超过限制"));
    Artwork::Usage usage; if (!Artwork::Validate(artwork, &usage, error)) return false;
    Budget budget{0, 0, 0, std::min(snapshot.limits.maximumPixels, Artwork::MaximumBytes / 4),
        std::min(snapshot.limits.maximumOperations, 512LL * 1024 * 1024), {}, {}};
    for (const auto& source : snapshot.sources) if (!budget.bytes(source.png, source.size, error)) return false;
    for (const auto& image : artwork.images) if (!budget.bytes(image.png, image.size, error)) return false;
    for (const auto& value : artwork.revisions) if (!budget.bytes(value.sourceBytes, {}, error) || !budget.bytes(value.iccProfile, {}, error)) return false;
    CTextureAssets::State pngs; if (!CTextureWorkspace::Effective(snapshot.sources, candidate.state.workspace, pngs, error)) return false;
    QMap<QByteArray, QImage> decoded;
    for (auto png = pngs.cbegin(); png != pngs.cend(); ++png) {
        const auto size = candidate.state.workspace.atlases.value(png.key()).size;
        if (!budget.bytes(*png, size, error)) return false;
        const auto digest = hash(*png);
        if (!decoded.contains(digest)) { QImage image; if (!CPngImage::Decode(*png, image, error)) return false; decoded.insert(digest, image); }
        candidate.images.insert(png.key(), decoded.value(digest));
    }
    int progress = 0;
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        if (control && !control->Report(CExportControl::Phase::Rendering, progress++, groups.size(), layers.value(group->layerId).name)) return fail(error, control->Error());
        const auto& layer = layers[group->layerId]; QImage source;
        if (layer.bounds.isEmpty()) { source = QImage(1, 1, QImage::Format_RGBA8888); source.fill(Qt::transparent); }
        else {
            if (!decoded.contains(layer.imageDigest)) {
                QImage image; if (!CPngImage::Decode(artwork.images.value(layer.imageDigest).png, image, error)) return false; decoded.insert(layer.imageDigest, image);
            }
            source = decoded.value(layer.imageDigest);
        }
        QRectF domain; QTransform placement; QSize size;
        if (!extent(*group, revision, layer, domain, placement, size, snapshot, error) || !budget.reserve(qint64(size.width()) * size.height(), error)) return false;
        const auto toLocal = inverse(placement) * inverse(revision.documentToFamily) * QTransform::fromTranslate(-layer.bounds.x(), -layer.bounds.y());
        if (!Artwork::ValidateTransform(toLocal, error)) return false;
        const auto filter = exactTransform(toLocal) ? Artwork::Filter::ExactTexels : Artwork::Filter::LinearLightPremultiplied;
        const qint64 work = qint64(size.width()) * size.height() * (filter == Artwork::Filter::ExactTexels ? 1 : 4);
        if (!budget.work(work, error)) return false;
        CArtworkSampleLimits limits;
        limits.maximumSourcePixels = std::min(limits.maximumSourcePixels, budget.maximumPixels);
        limits.maximumDestinationPixels = std::min(limits.maximumDestinationPixels, budget.maximumPixels);
        limits.maximumTotalPixels = std::min(limits.maximumTotalPixels, budget.maximumPixels); limits.maximumSamples = std::min(limits.maximumSamples, work);
        QImage rendered;
        if (!CArtworkSampler::Render(source, toLocal, size, group->boundary, filter, group->opacity ? layer.opacity : 255, rendered, error, control, limits)) return false;
        QByteArray png; QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !rendered.save(&buffer, "PNG") || png.isEmpty()) return fail(error, QStringLiteral("无法编码新素材图集"));
        if (!budget.bytes(png, {}, error) || !checkpoint(control, error)) return false;
        QByteArray identity; QDataStream identityStream(&identity, QIODevice::WriteOnly); identityStream.setVersion(QDataStream::Qt_6_0);
        identityStream << snapshot.sourceDigest << snapshot.documentRevision << revision.id << revision.sourceDigest << group.key() << group->meshes;
        const auto suffix = QString::fromLatin1(hash(identity).toHex()); const auto materialId = QStringLiteral("material:artwork:") + suffix;
        const auto placementId = QStringLiteral("placement:artwork:") + suffix, atlasId = QStringLiteral("atlas:artwork:") + suffix;
        if (artwork.materials.contains(materialId) || artwork.placements.contains(placementId)) return fail(error, QStringLiteral("候选素材标识与已有记录冲突"));
        const int slot = candidate.state.workspace.atlases.isEmpty() ? 0 : candidate.state.workspace.atlases.lastKey() + 1;
        if (slot >= CTextureAssets::MaximumSlots) return fail(error, QStringLiteral("没有可用的新图集槽"));
        const auto name = layer.name.trimmed().isEmpty() ? QStringLiteral("原稿图集 %1").arg(slot + 1) : layer.name.trimmed().left(240);
        candidate.state.workspace.atlases.insert(slot, {atlasId, name, -1, size, png}); candidate.images.insert(slot, rendered);
        artwork.materials.insert(materialId, {materialId, layer.name, revision.familyId, revision.id, layer.id, group->boundary, group->opacity, {}});
        artwork.placements.insert(placementId, {placementId, materialId, atlasId, placement, domain, group->gutter, filter});
        for (const auto& id : group->meshes) {
            Artwork::Binding binding; binding.meshId = id; binding.placementId = placementId; binding.familyCoordinates = group->coordinates.value(id);
            binding.sampledMaterial = {id, slot, {}};
            for (const auto& point : binding.familyCoordinates) { const auto pixel = placement.map(point); binding.sampledMaterial.uvs << float(pixel.x() / size.width()) << float(pixel.y() / size.height()); }
            if (!Artwork::TopologyDigest(binding.familyCoordinates.size(), meshes.value(id).triangles, binding.topologyDigest, error)) return false;
            candidate.state.materials[materialRows.value(id)] = binding.sampledMaterial;
            artwork.bindings.insert(id, binding);
            candidate.impacts[impactRows.value(id)].newSlot = slot;
        }
        bool extra;
        if (!uncovered(source, *group, revision, layer, meshes, budget, extra, error, control)) return false;
        if (extra) {
            const auto note = QStringLiteral("图层“%1”有非透明源像素中心位于关联网格三角形之外；已保留源内容，网格未自动扩张。").arg(layer.name);
            candidate.warnings.append(note); for (const auto& id : group->meshes) candidate.impacts[impactRows.value(id)].notes.append(note);
        }
    }
    Artwork::State frozenArtwork;
    if (!Artwork::Freeze(artwork, frozenArtwork, error) || !CTextureWorkspace::ValidateTransition(snapshot.sources, snapshot.baseline.workspace,
        candidate.state.workspace, CTextureWorkspace::Transition::ReplaceGenerated, error) ||
        !CArtworkReconciler::ValidateLinks(frozenArtwork, candidate.state.workspace, candidate.state.materials, triangles, error) || !checkpoint(control, error)) return false;
    artwork = std::move(frozenArtwork); candidate.warnings.removeDuplicates();
    CArtworkUpdateVerification::Seal(std::move(snapshot), std::move(request), candidate); result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备原稿更新，当前模型保持不变")); }

void CArtworkUpdateVerification::Seal(Planner::Snapshot source, Planner::Request request, Planner::Plan& plan)
{
    auto proof = std::make_shared<Planner::Plan::Verification>(); proof->source = std::move(source); proof->request = std::move(request);
    proof->plan = plan; proof->plan.m_verification.reset(); plan.m_verification = std::move(proof);
}
bool CArtworkUpdateVerification::Check(const Planner::Snapshot& source, const Planner::Plan& plan)
{
    if (!plan.m_verification || !same(source, plan.m_verification->source)) return false;
    const auto& stored = plan.m_verification->plan;
    if (plan.state != stored.state || plan.token != stored.token || plan.warnings != stored.warnings || plan.unconfirmed != stored.unconfirmed ||
        plan.images.keys() != stored.images.keys() || plan.impacts.size() != stored.impacts.size()) return false;
    for (auto image = plan.images.cbegin(); image != plan.images.cend(); ++image)
        if (image->cacheKey() != stored.images.value(image.key()).cacheKey()) return false;
    for (int index = 0; index < plan.impacts.size(); ++index) {
        const auto& a = plan.impacts[index]; const auto& b = stored.impacts[index];
        if (a.meshId != b.meshId || a.action != b.action || a.layerId != b.layerId || a.oldSlot != b.oldSlot || a.newSlot != b.newSlot || a.notes != b.notes) return false;
    }
    return true;
}
CArtworkSourceImport::SourceCandidate CArtworkUpdateVerification::Source(const Planner::Plan& plan)
{ return plan.m_verification->request.incoming; }
bool CArtworkUpdatePlanner::Validate(const Snapshot& snapshot, const Plan& plan, QString* error, bool requireConfirmed) try
{
    if (!CArtworkUpdateVerification::Check(snapshot, plan)) return fail(error, QStringLiteral("素材更新预览与当前模型或完整计划不一致"));
    if (requireConfirmed && !plan.unconfirmed.isEmpty()) return fail(error, QStringLiteral("请先确认所有首次关联或含糊的图层匹配"));
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法验证素材更新计划")); }
bool CArtworkUpdatePlanner::PreparedSource(const Snapshot& snapshot, const Plan& plan, CArtworkSourceImport::SourceCandidate& result, QString* error) try
{
    if (!Validate(snapshot, plan, error)) return false;
    auto candidate = CArtworkUpdateVerification::Source(plan); result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取已验证的来源素材")); }
}
