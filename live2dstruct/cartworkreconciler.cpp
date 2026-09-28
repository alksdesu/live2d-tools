#include "cartworkreconciler.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QImage>
#include <QIODevice>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
using Materials = QMap<QString, CModelDrawableMaterial>;
using Triangles = CArtworkReconciler::Triangles;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool close(double expected, double actual)
{
    return std::isfinite(expected) && std::isfinite(actual) && std::abs(expected - actual) <=
        2 * std::numeric_limits<float>::epsilon() * std::max({1.0, std::abs(expected), std::abs(actual)});
}
bool directory(const CModelMaterialState& values, Materials& result, QString* error)
{
    if (!CModelMaterialEdit::Validate(values, error)) return false;
    Materials candidate; for (const auto& value : values) candidate.insert(value.id, value);
    result.swap(candidate); return true;
}
bool topology(const Materials& materials, const Triangles& triangles, QMap<QString, QByteArray>& result, QString* error)
{
    if (materials.keys() != triangles.keys()) return fail(error, QStringLiteral("素材校验需要完整的网格三角目录"));
    QMap<QString, QByteArray> hashes; qint64 count = 0;
    for (auto it = materials.cbegin(); it != materials.cend(); ++it) {
        const auto indices = triangles.constFind(it.key());
        if (indices->size() > Artwork::MaximumTopologyIndices - count) return fail(error, QStringLiteral("素材三角目录超过检查预算"));
        QByteArray value; if (!Artwork::TopologyDigest(it->uvs.size() / 2, *indices, value, error)) return false;
        count += indices->size(); hashes.insert(it.key(), std::move(value));
    }
    result.swap(hashes); return true;
}
bool atlasDirectory(const CTextureWorkspace::State& workspace, QMap<QString, int>& result, QString* error)
{
    QMap<QString, int> indices;
    for (auto it = workspace.atlases.cbegin(); it != workspace.atlases.cend(); ++it) {
        if (it.key() < 0 || it.key() >= CTextureAssets::MaximumSlots || it->id.isEmpty() || indices.contains(it->id) ||
            it->size.width() <= 0 || it->size.height() <= 0 || qint64(it->size.width()) * it->size.height() > CTextureWorkspace::MaximumDecodedBytes / 4)
            return fail(error, QStringLiteral("素材关联的图集目录无效"));
        indices.insert(it->id, it.key());
    }
    result.swap(indices); return true;
}
bool linked(const Artwork::State& artwork, const CTextureWorkspace::State& workspace, const Materials& materials,
            const QMap<QString, QByteArray>* hashes, QString* error)
{
    QMap<QString, int> atlasSlots; if (!atlasDirectory(workspace, atlasSlots, error)) return false;
    for (const auto& binding : artwork.bindings) {
        const auto material = materials.constFind(binding.meshId);
        if (material == materials.cend()) return fail(error, QStringLiteral("素材关联引用了不存在的网格：%1").arg(binding.meshId));
        if (binding.status == Artwork::BindingStatus::Detached) continue;
        const auto placement = artwork.placements.constFind(binding.placementId);
        if (placement == artwork.placements.cend() || !atlasSlots.contains(placement->atlasId) ||
            atlasSlots.value(placement->atlasId) != material->textureIndex || binding.sampledMaterial != *material ||
            binding.familyCoordinates.size() * 2 != material->uvs.size() || (hashes && hashes->value(binding.meshId) != binding.topologyDigest))
            return fail(error, QStringLiteral("素材关联与当前网格采样或拓扑不一致：%1").arg(binding.meshId));
        const auto size = workspace.atlases.value(material->textureIndex).size;
        for (qsizetype vertex = 0; vertex < binding.familyCoordinates.size(); ++vertex) {
            const auto point = placement->familyToAtlas.map(binding.familyCoordinates[vertex]);
            if (!close(point.x() / size.width(), material->uvs[vertex * 2]) || !close(point.y() / size.height(), material->uvs[vertex * 2 + 1]))
                return fail(error, QStringLiteral("素材来源坐标无法对应当前 UV：%1").arg(binding.meshId));
        }
    }
    return true;
}
QTransform inverse(const QTransform& value)
{
    const double scale = std::max({std::abs(value.m11()), std::abs(value.m12()), std::abs(value.m21()), std::abs(value.m22())});
    const double a = value.m11() / scale, b = value.m12() / scale, c = value.m21() / scale, d = value.m22() / scale;
    const double determinant = a * d - b * c;
    const double x = (d / determinant) / scale, y = (-b / determinant) / scale;
    const double z = (-c / determinant) / scale, w = (a / determinant) / scale;
    return QTransform(x, y, z, w, -x * value.dx() - z * value.dy(), -y * value.dx() - w * value.dy());
}
bool contains(const QRectF& domain, QPointF point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y()) && point.x() >= domain.left() && point.x() <= domain.right() &&
        point.y() >= domain.top() && point.y() <= domain.bottom();
}
void detach(Artwork::Binding& binding, const QString& reason)
{ binding.status = Artwork::BindingStatus::Detached; binding.detachedReason = reason; }
bool pixelsEqual(const QByteArray& first, const QByteArray& second, bool& same, QString* error)
{
    if (first == second) { same = true; return true; }
    QImage a, b; if (!CPngImage::Decode(first, a, error) || !CPngImage::Decode(second, b, error)) return false;
    same = a.size() == b.size();
    for (int y = 0; same && y < a.height(); ++y) same = std::memcmp(a.constScanLine(y), b.constScanLine(y), size_t(a.width()) * 4) == 0;
    return true;
}
bool exactCopy(const QTransform& value)
{
    if (!value.isAffine()) return false;
    const double a = value.m11(), b = value.m12(), c = value.m21(), d = value.m22();
    const auto unit = [](double x) { return x == -1.0 || x == 0.0 || x == 1.0; };
    return unit(a) && unit(b) && unit(c) && unit(d) && std::abs(a) + std::abs(c) == 1.0 &&
        std::abs(b) + std::abs(d) == 1.0 && a * b + c * d == 0.0 &&
        std::isfinite(value.dx()) && std::isfinite(value.dy()) &&
        value.dx() == std::trunc(value.dx()) && value.dy() == std::trunc(value.dy());
}
bool exact(float first, float second) { return std::memcmp(&first, &second, sizeof(float)) == 0; }
bool samePlacement(Artwork::Placement first, const Artwork::Placement& second)
{ first.id = second.id; return first == second; }
bool nearTransform(const QTransform& first, const QTransform& second)
{
    const double a[]{first.m11(), first.m12(), first.m21(), first.m22(), first.dx(), first.dy()};
    const double b[]{second.m11(), second.m12(), second.m21(), second.m22(), second.dx(), second.dy()};
    for (int index = 0; index < 6; ++index)
        if (std::abs(a[index] - b[index]) > 16 * std::numeric_limits<double>::epsilon() * std::max({1.0, std::abs(a[index]), std::abs(b[index])})) return false;
    return true;
}
QString placementKey(const Artwork::Placement& value)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << value.materialId << value.atlasId << value.familyToAtlas << value.sourceDomain << value.gutter << int(value.filter);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString placementId(const Artwork::Placement& value, const QMap<QString, Artwork::Placement>& existing)
{
    const QString base = QStringLiteral("placement:rebase:") + placementKey(value);
    QString id = base; int suffix = 0;
    while (existing.contains(id) && !samePlacement(value, existing.value(id))) id = base + QLatin1Char(':') + QString::number(++suffix);
    return id;
}
}

bool CArtworkReconciler::SourcePatch(const Artwork::State& artwork, const QString& meshId, int gutter,
                                    QRect& result, QString* error) try
{
    if (gutter < 1 || gutter > 64) return fail(error, QStringLiteral("素材复制边距必须为 1–64 像素"));
    const auto binding = artwork.bindings.constFind(meshId);
    if (binding == artwork.bindings.cend() || binding->status == Artwork::BindingStatus::Detached) {
        result = {}; if (error) error->clear(); return true;
    }
    const auto placement = artwork.placements.constFind(binding->placementId);
    if (binding->status != Artwork::BindingStatus::Linked || placement == artwork.placements.cend() ||
        placement->gutter < 1 || placement->gutter > 64 || !Artwork::ValidateTransform(placement->familyToAtlas, error))
        return fail(error, QStringLiteral("关联素材的排布或边距无效"));
    const auto& domain = placement->sourceDomain;
    if (!std::isfinite(domain.x()) || !std::isfinite(domain.y()) || !std::isfinite(domain.width()) || !std::isfinite(domain.height()) ||
        domain.width() <= 0 || domain.height() <= 0)
        return fail(error, QStringLiteral("关联素材的完整范围无效"));
    double left = std::numeric_limits<double>::infinity(), right = -left, top = left, bottom = -left;
    for (const auto& point : {domain.topLeft(), domain.topRight(), domain.bottomLeft(), domain.bottomRight()}) {
        const auto pixel = placement->familyToAtlas.map(point);
        if (!std::isfinite(pixel.x()) || !std::isfinite(pixel.y())) return fail(error, QStringLiteral("关联素材的完整范围映射超出有限坐标"));
        left = std::min(left, pixel.x()); right = std::max(right, pixel.x());
        top = std::min(top, pixel.y()); bottom = std::max(bottom, pixel.y());
    }
    const int padding = std::max(gutter, placement->gutter);
    left = std::floor(left) - padding; top = std::floor(top) - padding;
    right = std::ceil(right) + padding; bottom = std::ceil(bottom) + padding;
    const double low = std::numeric_limits<int>::min(), high = std::numeric_limits<int>::max();
    if (left < low || top < low || right > high || bottom > high || right <= left || bottom <= top ||
        right - left > high || bottom - top > high)
        return fail(error, QStringLiteral("关联素材的完整复制范围超出整数坐标，请先调整来源映射"));
    const QRect patch(int(left), int(top), int(right - left), int(bottom - top));
    result = patch; if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法计算完整关联素材范围")); }

bool CArtworkReconciler::ValidateLinks(const Artwork::State& artwork, const CTextureWorkspace::State& workspace,
                                      const CModelMaterialState& materials, const Triangles& triangles, QString* error) try
{
    Materials directoryValues; QMap<QString, QByteArray> hashes;
    if (!Artwork::Validate(artwork, nullptr, error) || !directory(materials, directoryValues, error) ||
        !topology(directoryValues, triangles, hashes, error) || !linked(artwork, workspace, directoryValues, &hashes, error)) return false;
    if (error) error->clear(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法校验素材关联")); }

bool CArtworkReconciler::ManualAppearance(const CTextureAssets::Catalog& sources, const CModelAtlasState& before,
                                         const CTextureWorkspace::State& workspace, const CModelMaterialState& materials,
                                         const Triangles& triangles, Artwork::State& result, QString* error) try
{
    Artwork::State candidate; Materials previous, next; QMap<QString, QByteArray> hashes;
    if (!Artwork::Freeze(before.artwork, candidate, error) || !directory(before.materials, previous, error) || !directory(materials, next, error) ||
        previous.keys() != next.keys() || !topology(next, triangles, hashes, error) ||
        !linked(candidate, before.workspace, previous, nullptr, error) ||
        !CTextureWorkspace::ValidateTransition(sources, before.workspace, workspace, CTextureWorkspace::Transition::ReplaceGenerated, error))
        return error && error->isEmpty() ? fail(error, QStringLiteral("手动外观修改改变了网格目录")) : false;
    QMap<int, bool> unchanged;
    for (auto binding = candidate.bindings.begin(); binding != candidate.bindings.end(); ++binding) {
        if (binding->status == Artwork::BindingStatus::Detached) continue;
        const auto& oldMaterial = previous[binding.key()]; const auto& material = next[binding.key()];
        const auto oldAtlas = before.workspace.atlases.constFind(oldMaterial.textureIndex), atlas = workspace.atlases.constFind(oldMaterial.textureIndex);
        if (oldAtlas == before.workspace.atlases.cend() || atlas == workspace.atlases.cend() || oldAtlas->id != atlas->id || oldAtlas->size != atlas->size) {
            detach(*binding, QStringLiteral("图集身份或尺寸已手动改变")); continue;
        }
        if (!unchanged.contains(oldMaterial.textureIndex)) {
            const auto oldPng = oldAtlas->png.isEmpty() ? sources.value(oldAtlas->sourceSlot).png : oldAtlas->png;
            const auto png = atlas->png.isEmpty() ? sources.value(atlas->sourceSlot).png : atlas->png;
            bool same; if (!pixelsEqual(oldPng, png, same, error)) return false;
            unchanged.insert(oldMaterial.textureIndex, same);
        }
        if (!unchanged.value(oldMaterial.textureIndex)) { detach(*binding, QStringLiteral("图集像素已手动替换，原素材关联已保留")); continue; }
        if (material.textureIndex != oldMaterial.textureIndex || material.uvs.size() != oldMaterial.uvs.size()) {
            detach(*binding, QStringLiteral("纹理槽或顶点数量已改变，需要重新关联素材")); continue;
        }
        if (material != oldMaterial) {
            const auto& placement = candidate.placements[binding->placementId]; const auto transform = inverse(placement.familyToAtlas);
            QVector<QPointF> points = binding->familyCoordinates; bool inside = true;
            for (int vertex = 0; vertex < points.size(); ++vertex) {
                if (exact(material.uvs[vertex * 2], oldMaterial.uvs[vertex * 2]) && exact(material.uvs[vertex * 2 + 1], oldMaterial.uvs[vertex * 2 + 1])) continue;
                const QPointF pixel(double(material.uvs[vertex * 2]) * atlas->size.width(), double(material.uvs[vertex * 2 + 1]) * atlas->size.height());
                points[vertex] = transform.map(pixel);
                if (!contains(placement.sourceDomain, points[vertex]) && std::isfinite(points[vertex].x()) && std::isfinite(points[vertex].y())) {
                    const QPointF boundary(std::clamp(points[vertex].x(), placement.sourceDomain.left(), placement.sourceDomain.right()),
                                           std::clamp(points[vertex].y(), placement.sourceDomain.top(), placement.sourceDomain.bottom()));
                    const auto projected = placement.familyToAtlas.map(boundary);
                    if (exact(float(projected.x() / atlas->size.width()), material.uvs[vertex * 2]) &&
                        exact(float(projected.y() / atlas->size.height()), material.uvs[vertex * 2 + 1])) points[vertex] = boundary;
                }
                inside &= contains(placement.sourceDomain, points[vertex]);
            }
            if (!inside) { detach(*binding, QStringLiteral("手动 UV 已移出关联素材范围")); continue; }
            binding->familyCoordinates.swap(points); binding->sampledMaterial = material;
        }
        binding->topologyDigest = hashes.value(binding.key());
    }
    if (!ValidateLinks(candidate, workspace, materials, triangles, error)) return false;
    return Artwork::Freeze(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法协调手动外观修改，原关联保持不变")); }

bool CArtworkReconciler::RemapMesh(const Artwork::State& before, const CMocMeshStructureBuilder::MeshEdit& edit,
                                  const CModelDrawableMaterial& material, const QVector<quint16>& triangles,
                                  Artwork::State& result, QString* error) try
{
    Artwork::State candidate; QByteArray hash;
    if (!Artwork::Freeze(before, candidate, error) || material.id != edit.artMeshId || !CModelMaterialEdit::Validate({material}, error) ||
        !Artwork::TopologyDigest(material.uvs.size() / 2, triangles, hash, error)) return false;
    auto binding = candidate.bindings.find(edit.artMeshId);
    if (binding == candidate.bindings.end() || binding->status == Artwork::BindingStatus::Detached) { result = std::move(candidate); if (error) error->clear(); return true; }
    const int count = binding->familyCoordinates.size();
    if (edit.vertices.size() * 2 != material.uvs.size() || edit.oldToNew.size() != count || edit.indices != triangles ||
        material.textureIndex != binding->sampledMaterial.textureIndex)
        return fail(error, QStringLiteral("素材顶点迁移与完整网格请求不一致"));
    QVector<QPointF> points; points.reserve(edit.vertices.size()); QSet<int> survivors; qint64 references = 0;
    for (int vertex = 0; vertex < edit.vertices.size(); ++vertex) {
        const auto& source = edit.vertices[vertex];
        if (source.indices.isEmpty() || source.indices.size() != source.weights.size() || source.indices.size() > Artwork::MaximumCoordinates - references)
            return fail(error, QStringLiteral("素材顶点来源数量无效"));
        references += source.indices.size(); double sum = 0;
        for (int index = 0; index < source.indices.size(); ++index) {
            if (source.indices[index] < 0 || source.indices[index] >= count || !std::isfinite(source.weights[index]) || source.weights[index] < 0)
                return fail(error, QStringLiteral("素材顶点来源索引或权重无效"));
            sum += source.weights[index];
        }
        if (!std::isfinite(sum) || std::abs(sum - 1) > CMocMeshStructureBuilder::WeightTolerance)
            return fail(error, QStringLiteral("素材顶点来源权重之和必须为 1"));
        QPointF point;
        if (source.indices.size() == 1) point = binding->familyCoordinates[source.indices.first()];
        else for (int index = 0; index < source.indices.size(); ++index) point += binding->familyCoordinates[source.indices[index]] * (source.weights[index] / sum);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail(error, QStringLiteral("插值后的素材坐标超出有限范围"));
        for (int axis = 0; axis < 2; ++axis) {
            double expected = 0, minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
            for (int index = 0; index < source.indices.size(); ++index) {
                const double value = binding->sampledMaterial.uvs[source.indices[index] * 2 + axis];
                expected += value * (source.weights[index] / sum); minimum = std::min(minimum, value); maximum = std::max(maximum, value);
            }
            const float quantized = source.indices.size() == 1 ? binding->sampledMaterial.uvs[source.indices.first() * 2 + axis] : float(std::clamp(expected, minimum, maximum));
            if (source.indices.size() == 1 ? !exact(quantized, material.uvs[vertex * 2 + axis]) : !close(quantized, material.uvs[vertex * 2 + axis]))
                return fail(error, QStringLiteral("网格 UV 包含顶点插值之外的修改"));
        }
        points.append(point);
    }
    for (int old = 0; old < count; ++old) {
        const int next = edit.oldToNew[old];
        if (next < -1 || next >= edit.vertices.size() || (next >= 0 && survivors.contains(next))) return fail(error, QStringLiteral("保留顶点映射无效"));
        if (next < 0) continue;
        const auto& source = edit.vertices[next];
        if (source.indices.size() != 1 || source.indices.first() != old) return fail(error, QStringLiteral("保留顶点不是原顶点的直接副本"));
        survivors.insert(next);
    }
    binding->familyCoordinates.swap(points); binding->sampledMaterial = material; binding->topologyDigest = hash;
    return Artwork::Freeze(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法迁移素材顶点，原关联保持不变")); }

bool CArtworkReconciler::RebasePlacements(const Artwork::State& baseline, const CModelAtlasState& before,
                                         const CTextureWorkspace::State& workspace, const CModelMaterialState& materials,
                                         const Triangles& triangles, const QMap<QString, Rebase>& rebases,
                                         Artwork::State& result, QString* error) try
{
    Artwork::State candidate; Materials previous, next; QMap<QString, QByteArray> hashes;
    if (!Artwork::Validate(baseline, nullptr, error) || !Artwork::Freeze(before.artwork, candidate, error) ||
        !directory(before.materials, previous, error) || !directory(materials, next, error) || previous.keys() != next.keys() ||
        !topology(next, triangles, hashes, error) || !linked(candidate, before.workspace, previous, &hashes, error)) return false;
    if (baseline.images != candidate.images || baseline.families != candidate.families || baseline.revisions != candidate.revisions ||
        baseline.materials != candidate.materials || baseline.bindings.keys() != candidate.bindings.keys())
        return fail(error, QStringLiteral("图集排布不能改变原稿来源或素材目录"));
    for (auto binding = candidate.bindings.cbegin(); binding != candidate.bindings.cend(); ++binding) {
        auto original = baseline.bindings.value(binding.key());
        original.placementId = binding->placementId; original.sampledMaterial = binding->sampledMaterial;
        if (!(original == *binding)) return fail(error, QStringLiteral("图集排布不能改变素材来源坐标或关联状态"));
    }
    QMap<QString, Artwork::Placement> desired;
    for (auto value = previous.cbegin(); value != previous.cend(); ++value)
        if (*value != next.value(value.key()) && !rebases.contains(value.key())) return fail(error, QStringLiteral("图集操作缺少改变网格的明确坐标变换"));
    for (auto rebase = rebases.cbegin(); rebase != rebases.cend(); ++rebase) {
        if (!previous.contains(rebase.key()) || !workspace.atlases.contains(rebase->targetSlot) ||
            rebase->gutter < 1 || rebase->gutter > 64 || !Artwork::ValidateTransform(rebase->pixelDelta, error))
            return fail(error, QStringLiteral("图集素材变换对象或目标无效"));
        if (!exactCopy(rebase->pixelDelta)) return fail(error, QStringLiteral("图集复制只支持整数平移、直角旋转或镜像，不能隐式重采样"));
        const auto& old = previous[rebase.key()]; const auto& material = next[rebase.key()];
        const auto source = before.workspace.atlases.constFind(old.textureIndex); const auto& atlas = workspace.atlases[rebase->targetSlot];
        if (source == before.workspace.atlases.cend() || material.textureIndex != rebase->targetSlot || old.uvs.size() != material.uvs.size())
            return fail(error, QStringLiteral("图集素材变换不能改变顶点数量"));
        const qint64 width = qint64(rebase->sourcePatch.right()) - rebase->sourcePatch.x() + 1;
        const qint64 height = qint64(rebase->sourcePatch.bottom()) - rebase->sourcePatch.y() + 1;
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
            return fail(error, QStringLiteral("图集素材复制范围无效"));
        const QRectF patch(rebase->sourcePatch.x(), rebase->sourcePatch.y(), width, height);
        QRect required;
        if (!SourcePatch(before.artwork, rebase.key(), rebase->gutter, required, error)) return false;
        if (!required.isEmpty() && !rebase->sourcePatch.contains(required))
            return fail(error, QStringLiteral("图集复制没有保留完整关联素材范围及边距"));
        const auto binding = candidate.bindings.constFind(rebase.key());
        const int padding = binding != candidate.bindings.cend() && binding->status == Artwork::BindingStatus::Linked
            ? std::max(rebase->gutter, candidate.placements.value(binding->placementId).gutter) : rebase->gutter;
        const QPointF halo(padding, padding);
        for (int vertex = 0; vertex < old.uvs.size() / 2; ++vertex) {
            const QPointF pixel(double(old.uvs[vertex * 2]) * source->size.width(), double(old.uvs[vertex * 2 + 1]) * source->size.height());
            const auto moved = rebase->pixelDelta.map(pixel);
            if (!contains(patch, pixel - halo) || !contains(patch, pixel + halo) || !close(moved.x() / atlas.size.width(), material.uvs[vertex * 2]) || !close(moved.y() / atlas.size.height(), material.uvs[vertex * 2 + 1]))
                return fail(error, QStringLiteral("图集素材变换与复制范围或候选 UV 不一致"));
        }
        if (binding == candidate.bindings.cend() || binding->status == Artwork::BindingStatus::Detached) continue;
        auto placement = candidate.placements.value(binding->placementId);
        placement.familyToAtlas = placement.familyToAtlas * rebase->pixelDelta;
        placement.atlasId = atlas.id; placement.gutter = rebase->gutter;
        const auto initialBinding = baseline.bindings.constFind(rebase.key());
        if (initialBinding != baseline.bindings.cend() && initialBinding->status == Artwork::BindingStatus::Linked) {
            const auto initial = baseline.placements.constFind(initialBinding->placementId);
            if (initial != baseline.placements.cend() && initial->materialId == placement.materialId && initial->atlasId == placement.atlasId &&
                initial->gutter == placement.gutter && initial->filter == placement.filter && initial->sourceDomain == placement.sourceDomain &&
                nearTransform(initial->familyToAtlas, placement.familyToAtlas)) placement = *initial;
        }
        if (!Artwork::ValidateTransform(placement.familyToAtlas, error)) return false;
        desired.insert(rebase.key(), placement);
    }
    QMap<QString, QSet<QString>> consumers;
    QMap<QString, QMap<QString, bool>> equivalent;
    QMap<QString, QString> keys;
    for (const auto& binding : candidate.bindings) consumers[binding.placementId].insert(binding.meshId);
    for (auto placement = candidate.placements.cbegin(); placement != candidate.placements.cend(); ++placement) {
        const auto key = placementKey(*placement); equivalent[key].insert(placement.key(), true); keys.insert(placement.key(), key);
    }
    const auto assign = [&](const Artwork::Placement& placement, Artwork::Binding& binding) {
        if (keys.contains(placement.id)) equivalent[keys.value(placement.id)].remove(placement.id);
        const auto key = placementKey(placement); keys[placement.id] = key; equivalent[key].insert(placement.id, true);
        candidate.placements.insert(placement.id, placement);
        consumers[binding.placementId].remove(binding.meshId); consumers[placement.id].insert(binding.meshId); binding.placementId = placement.id;
    };
    const auto available = [&](const QString& id, const Artwork::Placement& wanted) {
        const auto current = candidate.placements.constFind(id);
        if (current == candidate.placements.cend() || samePlacement(*current, wanted)) return true;
        for (const auto& mesh : consumers.value(id)) {
            const auto change = desired.constFind(mesh);
            if (change == desired.cend() || !samePlacement(*change, wanted)) return false;
        }
        return true;
    };
    for (auto entry = desired.cbegin(); entry != desired.cend(); ++entry) {
        auto placement = *entry; auto& binding = candidate.bindings[entry.key()]; QString chosen;
        const auto originalBinding = baseline.bindings.constFind(entry.key());
        if (originalBinding != baseline.bindings.cend()) {
            const auto original = baseline.placements.constFind(originalBinding->placementId);
            if (original != baseline.placements.cend() && samePlacement(placement, *original) && available(original.key(), placement)) chosen = original.key();
        }
        if (chosen.isEmpty()) {
            const auto matching = equivalent.value(placementKey(placement));
            for (auto existing = matching.cbegin(); existing != matching.cend(); ++existing)
                if (samePlacement(placement, candidate.placements.value(existing.key()))) { chosen = existing.key(); break; }
        }
        if (chosen.isEmpty()) {
            chosen = available(binding.placementId, placement) ? binding.placementId : placementId(placement, candidate.placements);
        }
        placement.id = chosen; assign(placement, binding);
        binding.sampledMaterial = next.value(entry.key()); binding.topologyDigest = hashes.value(entry.key());
    }
    for (auto& binding : candidate.bindings) {
        const auto initial = baseline.bindings.constFind(binding.meshId);
        if (binding.status != Artwork::BindingStatus::Linked || initial == baseline.bindings.cend() || initial->status != Artwork::BindingStatus::Linked) continue;
        const auto preferred = baseline.placements.constFind(initial->placementId);
        const auto current = candidate.placements.constFind(binding.placementId);
        if (preferred != baseline.placements.cend() && current != candidate.placements.cend() && samePlacement(*current, *preferred) && available(preferred.key(), *preferred)) {
            assign(*preferred, binding);
        }
    }
    QSet<QString> referenced; for (const auto& binding : candidate.bindings) referenced.insert(binding.placementId);
    for (auto initial = baseline.placements.cbegin(); initial != baseline.placements.cend(); ++initial)
        if (!referenced.contains(initial.key())) candidate.placements.insert(initial.key(), *initial);
    for (auto placement = candidate.placements.begin(); placement != candidate.placements.end();)
        if (!baseline.placements.contains(placement.key()) && !referenced.contains(placement.key())) placement = candidate.placements.erase(placement); else ++placement;
    if (!ValidateLinks(candidate, workspace, materials, triangles, error)) return false;
    return Artwork::Freeze(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法迁移图集素材关联，原关联保持不变")); }
}
