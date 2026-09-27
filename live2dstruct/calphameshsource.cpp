#include "calphameshsource.h"
#include "cmodeldocument.h"
#include "csectiontable.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QString own(const QString& value) { return QString(value.constData(), value.size()); }
QByteArray hash(const QByteArray& value) { return QCryptographicHash::hash(value, QCryptographicHash::Sha256); }
QTransform inverse(const QTransform& value)
{
    const double scale = std::max({std::abs(value.m11()), std::abs(value.m12()), std::abs(value.m21()), std::abs(value.m22())});
    const double a = value.m11() / scale, b = value.m12() / scale, c = value.m21() / scale, d = value.m22() / scale;
    const double determinant = a * d - b * c;
    const double x = (d / determinant) / scale, y = (-b / determinant) / scale;
    const double z = (-c / determinant) / scale, w = (a / determinant) / scale;
    return QTransform(x, y, z, w, -x * value.dx() - z * value.dy(), -y * value.dx() - w * value.dy());
}
bool checkpoint(CExportControl* control, QString* error)
{ return !control || control->Checkpoint() || fail(error, control->Error()); }
bool pins(const QByteArray& bytes, const QString& id, int count, QVector<int>& result, QString* error)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    if (!table.IsValid()) return fail(error, QStringLiteral("无法读取网格接缝定义：%1").arg(reader.Error()));
    int target = -1;
    for (int index = 0; index < table.art_meshes.size(); ++index) if (table.art_meshes[index]->ids == id) {
        if (target >= 0) return fail(error, QStringLiteral("网格 ID 重复"));
        target = index;
    }
    if (target < 0 || table.art_meshes[target]->vertex_counts != count) return fail(error, QStringLiteral("网格顶点目录与素材不一致"));
    QSet<int> endpoints;
    for (const auto* glue : table.glue) {
        if (glue->art_mesh_indices_a != target && glue->art_mesh_indices_b != target) continue;
        const int first = glue->glue_info_sources_begin_indices, size = glue->glue_info_sources_counts;
        if (first < 0 || size < 0 || size % 2 || first > table.glue_info.size() || size > table.glue_info.size() - first)
            return fail(error, QStringLiteral("网格 Glue 配对范围无效"));
        for (int pair = 0; pair < size; pair += 2) for (int side = 0; side < 2; ++side) {
            if ((side ? glue->art_mesh_indices_b : glue->art_mesh_indices_a) != target) continue;
            const auto* value = table.glue_info[first + pair + side];
            if (!value || value->position_indices < 0 || value->position_indices >= count || !std::isfinite(value->weights))
                return fail(error, QStringLiteral("网格 Glue 端点无效"));
            endpoints.insert(value->position_indices);
        }
    }
    result = endpoints.values(); std::sort(result.begin(), result.end()); return true;
}
}

bool CAlphaMeshSource::Capture(const CModelDocument& document, const QString& meshId, Source& result,
                               QString* error, CExportControl* control) try
{
    if (error) error->clear();
    if (control && control->IsCancellationRequested()) {
        control->Checkpoint();
        return fail(error, control->Error().isEmpty() ? QStringLiteral("自动网格准备已取消") : control->Error());
    }
    if (!document.IsLoaded() || meshId.isEmpty()) return fail(error, QStringLiteral("请选择已加载模型中的 ArtMesh"));
    Source candidate; auto& input = candidate.input; input.artMeshId = own(meshId);
    CModelDrawableMaterial material;
    if (!document.CaptureDrawableMaterial(meshId, material, error)) return false;
    if (material.uvs.size() < 6 || material.uvs.size() % 2 || material.uvs.size() / 2 > CMocMeshStructureBuilder::MaximumVertices)
        return fail(error, QStringLiteral("网格顶点数量不支持自动生成"));
    input.uvs.reserve(material.uvs.size());
    for (float uv : material.uvs) input.uvs.append(uv);
    const auto indices = document.DrawableIndices(meshId);
    if (indices.isEmpty() || indices.size() % 3 || indices.size() > Artwork::MaximumTopologyIndices)
        return fail(error, QStringLiteral("网格没有可用的三角形覆盖范围"));
    input.indices.reserve(indices.size());
    for (auto index : indices) {
        if (index >= material.uvs.size() / 2) return fail(error, QStringLiteral("网格三角索引越界"));
        input.indices.append(index);
    }
    QByteArray runtime;
    if (!document.BuildRuntimeMoc(runtime, error) || !pins(runtime, input.artMeshId, material.uvs.size() / 2, input.pinnedVertices, error)) return false;
    QByteArray encoded, identity; QSize expected; quint8 opacity = 255;
    const auto& artwork = document.Artwork();
    const auto binding = artwork.bindings.constFind(meshId);
    if (binding != artwork.bindings.cend() && binding->status == Artwork::BindingStatus::Linked) {
        if (!document.ValidateArtworkReferences(artwork, error)) return false;
        const auto& placement = artwork.placements[binding->placementId];
        const auto& sourceMaterial = artwork.materials[placement.materialId];
        const auto& revision = artwork.revisions[sourceMaterial.revisionId];
        const auto layer = std::find_if(revision.layers.cbegin(), revision.layers.cend(),
            [&](const auto& value) { return value.id == sourceMaterial.layerId; });
        if (layer == revision.layers.cend() || layer->kind != Artwork::LayerKind::Raster || !layer->pixelEquivalent)
            return fail(error, QStringLiteral("关联来源不是可用于自动网格的独立栅格素材"));
        const auto transform = inverse(revision.documentToFamily);
        if (!Artwork::ValidateTransform(transform, error))
            return fail(error, QStringLiteral("关联素材坐标无法反解"));
        input.pixelCoordinates.reserve(binding->familyCoordinates.size());
        for (const auto& point : binding->familyCoordinates) input.pixelCoordinates.append(transform.map(point) - layer->bounds.topLeft());
        input.referenceSize = revision.canvasSize;
        input.repeat = sourceMaterial.boundary == Artwork::Boundary::Repeat;
        opacity = sourceMaterial.applyLayerOpacity ? layer->opacity : 255;
        candidate.linkedSource = true;
        const bool atlas = artwork.families[sourceMaterial.familyId].kind == Artwork::SourceKind::AtlasSnapshot;
        candidate.label = atlas ? QStringLiteral("关联的图集快照") : QStringLiteral("关联素材：%1").arg(layer->name);
        candidate.detail = QStringLiteral("使用已固定的素材版本 %1；仅在现有网格覆盖范围内生成。%2")
            .arg(revision.fileName.isEmpty() ? revision.id : revision.fileName,
                 atlas ? QStringLiteral("图集快照不等于独立原图层。") : QStringLiteral("不包含模型透明度和裁剪蒙版。"));
        QDataStream stream(&identity, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
        stream << sourceMaterial.familyId << revision.id << revision.sourceDigest << layer->id << layer->imageDigest
               << placement.id << placement.atlasId << placement.familyToAtlas << placement.sourceDomain << revision.documentToFamily << layer->bounds;
        if (layer->bounds.isEmpty()) {
            expected = QSize(1, 1); candidate.input.image = QImage(expected, QImage::Format_RGBA8888);
            if (candidate.input.image.isNull()) return fail(error, QStringLiteral("无法准备空素材"));
            candidate.input.image.fill(Qt::transparent);
        } else {
            const auto source = artwork.images.constFind(layer->imageDigest);
            if (source == artwork.images.cend()) return fail(error, QStringLiteral("关联素材缺少原始图像"));
            expected = source->size; encoded = source->png;
        }
    } else {
        CTextureAssets::State effective;
        if (!CTextureWorkspace::Effective(document.TextureSources(), document.TextureWorkspace(), effective, error)) return false;
        const auto atlas = document.TextureWorkspace().atlases.constFind(material.textureIndex);
        if (atlas == document.TextureWorkspace().atlases.cend() || !effective.contains(material.textureIndex))
            return fail(error, QStringLiteral("网格当前图集不存在"));
        expected = atlas->size; encoded = effective.value(material.textureIndex); input.referenceSize = expected; input.repeat = true;
        input.pixelCoordinates.reserve(material.uvs.size() / 2);
        for (int index = 0; index < material.uvs.size(); index += 2)
            input.pixelCoordinates.append(QPointF(double(material.uvs[index]) * expected.width(), double(material.uvs[index + 1]) * expected.height()));
        candidate.label = QStringLiteral("当前图集采样：%1").arg(atlas->name);
        candidate.detail = QStringLiteral("未使用独立来源图层；只处理现有 UV 三角覆盖内的图集透明度，不能分离覆盖范围内混入的其它图案。超界 UV 按重复纹理处理。");
        if (binding != artwork.bindings.cend()) candidate.detail.prepend(QStringLiteral("原素材关联已分离。"));
        QDataStream stream(&identity, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
        stream << atlas->id << material.textureIndex << document.TextureWorkspaceFingerprint();
    }
    if (expected.width() <= 0 || expected.height() <= 0 || expected.width() > 32768 || expected.height() > 32768 ||
        qint64(expected.width()) * expected.height() > input.limits.maximumPixels ||
        input.referenceSize.width() <= 0 || input.referenceSize.height() <= 0)
        return fail(error, QStringLiteral("自动网格素材超过图像尺寸或内存预算"));
    const qint64 workingBytes = qint64(expected.width()) * expected.height() * 4 + runtime.size() +
        qint64(input.pixelCoordinates.size()) * qint64(sizeof(QPointF)) * 2 + qint64(input.indices.size()) * 4 + qint64(input.uvs.size()) * 8;
    if (workingBytes > input.limits.maximumWorkingBytes || encoded.size() > input.limits.maximumWorkingBytes - workingBytes)
        return fail(error, QStringLiteral("自动网格素材超过工作内存预算"));
    for (const auto& point : input.pixelCoordinates)
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail(error, QStringLiteral("素材坐标包含非有限数值"));
    encoded = QByteArray(encoded.constData(), encoded.size());
    QByteArray token; QDataStream stream(&token, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(1) << document.SourceDigest() << document.Revision() << hash(runtime) << input.artMeshId << identity << hash(encoded)
           << opacity << expected << input.referenceSize << input.pixelCoordinates << input.uvs << input.indices << input.pinnedVertices << input.repeat;
    input.sourceToken = hash(token);
    if (control && !control->Report(CExportControl::Phase::Preparing, 0, 1, candidate.label)) return fail(error, control->Error());
    if (!encoded.isEmpty() && (!CPngImage::Decode(encoded, input.image, error) || input.image.size() != expected))
        return error && !error->isEmpty() ? false : fail(error, QStringLiteral("素材 PNG 尺寸与来源记录不一致"));
    input.image = input.image.convertToFormat(QImage::Format_RGBA8888);
    if (input.image.isNull()) return fail(error, QStringLiteral("无法分配自动网格素材图像"));
    if (opacity != 255) for (int y = 0; y < input.image.height(); ++y) {
        if (y % 64 == 0 && !checkpoint(control, error)) return false;
        auto* row = input.image.scanLine(y);
        for (int x = 0; x < input.image.width(); ++x) row[x * 4 + 3] = (int(row[x * 4 + 3]) * opacity + 127) / 255;
    }
    if (control && !control->Report(CExportControl::Phase::Preparing, 1, 1, candidate.label)) return fail(error, control->Error());
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备自动网格素材，原结果保持不变")); }
}
