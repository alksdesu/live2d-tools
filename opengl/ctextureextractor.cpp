#include "ctextureextractor.h"
#include "cuvgeometry.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmeshtopology.h"
#include "live2dstruct/cdrawableflags.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include "live2dstruct/ctextureassets.h"
#include "live2dstruct/cmocmaterialbuilder.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>
#include <exception>
#include <utility>
#include <cstring>
#include <map>
#include <tuple>
#include <QDebug>

using namespace live2dstruct;

namespace {
int repeatCoordinate(qint64 value, int size)
{
    const qint64 remainder = value % size;
    return int(remainder < 0 ? remainder + size : remainder);
}
bool validIndices(const QVector<quint16>& indices, int vertices)
{
    if (indices.isEmpty() || indices.size() % 3 || indices.size() > 3000000) return false;
    return std::all_of(indices.cbegin(), indices.cend(), [vertices](quint16 index) { return index < vertices; });
}

double cross(QPointF a, QPointF b)
{
    const double product = a.y() * b.x();
    return std::fma(a.x(), b.y(), -product) - std::fma(a.y(), b.x(), -product);
}

QRgb sampleBilinear(const QImage& atlas, double u, double v)
{
    const auto coordinate = [](double value, int size) {
        double wrapped = std::fmod(value * size, double(size));
        if (wrapped < 0.0) wrapped += size;
        return wrapped - 0.5;
    };
    const double x = coordinate(u, atlas.width()), y = coordinate(v, atlas.height());
    const int left = int(std::floor(x)), top = int(std::floor(y));
    const double fx = x - left, fy = y - top;
    const QRgb* first = reinterpret_cast<const QRgb*>(atlas.constScanLine(repeatCoordinate(top, atlas.height())));
    const QRgb* second = reinterpret_cast<const QRgb*>(atlas.constScanLine(repeatCoordinate(top + 1, atlas.height())));
    const QRgb a = first[repeatCoordinate(left, atlas.width())], b = first[repeatCoordinate(left + 1, atlas.width())];
    const QRgb c = second[repeatCoordinate(left, atlas.width())], d = second[repeatCoordinate(left + 1, atlas.width())];
    const auto channel = [&](int shift) {
        const double above = double((a >> shift) & 255) * (1.0 - fx) + double((b >> shift) & 255) * fx;
        const double below = double((c >> shift) & 255) * (1.0 - fx) + double((d >> shift) & 255) * fx;
        return std::clamp(int(std::round(above * (1.0 - fy) + below * fy)), 0, 255);
    };
    return qRgba(channel(16), channel(8), channel(0), channel(24));
}
}

CTextureExtractor::CTextureExtractor() {}
CTextureExtractor::~CTextureExtractor() {}

bool CTextureExtractor::LoadAtlases(const QVector<QString>& atlasPaths, CExportControl* control)
{
    try {
        const auto paths = atlasPaths;
        if (paths.size() > CTextureAssets::MaximumSlots) {
            if (control) control->Fail(QStringLiteral("纹理图集数量超出限制"));
            return false;
        }
        QVector<QByteArray> pngs;
        pngs.reserve(paths.size());
        qint64 total = 0;
        for (const auto& path : paths) {
            if (control && !control->Checkpoint()) return false;
            if (path.isEmpty()) { pngs.append(QByteArray()); continue; }
            const QFileInfo before(path);
            const auto canonical = before.canonicalFilePath();
            if (!before.isFile() || canonical.isEmpty() || before.size() > CTextureAssets::MaximumBytes - total) {
                if (control) control->Fail(QStringLiteral("纹理图集不存在或大小超出限制：%1").arg(path));
                return false;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                if (control) control->Fail(QStringLiteral("无法读取纹理图集：%1").arg(path));
                return false;
            }
            QByteArray bytes;
            while (!file.atEnd()) {
                if (control && !control->Checkpoint()) return false;
                const auto chunk = file.read(256 * 1024);
                if (file.error() != QFileDevice::NoError || (chunk.isEmpty() && !file.atEnd()) ||
                    chunk.size() > CTextureAssets::MaximumBytes - total - bytes.size()) {
                    if (control) control->Fail(QStringLiteral("纹理图集读取失败或大小超出限制：%1").arg(path));
                    return false;
                }
                bytes.append(chunk);
            }
            const QFileInfo after(path);
            if (after.canonicalFilePath() != canonical || before.size() != bytes.size() || after.size() != bytes.size() ||
                before.lastModified() != after.lastModified() || before.metadataChangeTime() != after.metadataChangeTime()) {
                if (control) control->Fail(QStringLiteral("纹理图集在读取期间发生变化：%1").arg(path));
                return false;
            }
            if (bytes.isEmpty()) {
                if (control) control->Fail(QStringLiteral("纹理图集为空：%1").arg(path));
                return false;
            }
            total += bytes.size(); pngs.append(std::move(bytes));
        }
        return LoadAtlasBytes(pngs, control);
    } catch (const std::exception&) {
        if (control) control->Fail(QStringLiteral("无法读取纹理图集，原图集保持不变"));
        return false;
    }
}

bool CTextureExtractor::LoadAtlasBytes(const QVector<QByteArray>& atlasPngs, CExportControl* control)
{
    try {
        const auto pngs = atlasPngs;
        if (control && !control->Checkpoint()) return false;
        if (pngs.isEmpty() || pngs.size() > CTextureAssets::MaximumSlots) {
            if (control) control->Fail(QStringLiteral("纹理图集数量为空或超出限制"));
            return false;
        }
        qint64 bytes = 0, pixels = 0;
        for (const auto& png : pngs) {
            if (control && !control->Checkpoint()) return false;
            if (png.isEmpty()) continue;
            QSize size; QString error;
            if (png.size() > CTextureAssets::MaximumBytes - bytes || !CPngImage::Size(png, size, &error)) {
                if (control) control->Fail(error.isEmpty() ? QStringLiteral("纹理图集编码总量超出限制") : error);
                return false;
            }
            const qint64 count = qint64(size.width()) * size.height();
            if (count > MaximumMaterialPixels || count > MaximumTotalMaterialPixels - pixels) {
                if (control) control->Fail(QStringLiteral("纹理图集解码像素总量超出限制"));
                return false;
            }
            bytes += png.size(); pixels += count;
        }
        QVector<QImage> candidate;
        candidate.reserve(pngs.size());
        for (const auto& png : pngs) {
            if (control && !control->Report(CExportControl::Phase::Preparing, candidate.size(), pngs.size())) return false;
            if (png.isEmpty()) { candidate.append(QImage()); continue; }
            QImage image; QString error;
            if (!CPngImage::Decode(png, image, &error)) {
                if (control) control->Fail(error);
                return false;
            }
            if (control && !control->Checkpoint()) return false;
            auto converted = image.convertToFormat(QImage::Format_ARGB32);
            if (converted.isNull()) {
                if (control) control->Fail(QStringLiteral("无法转换纹理图集像素格式"));
                return false;
            }
            candidate.append(std::move(converted));
        }
        if (control && !control->Report(CExportControl::Phase::Preparing, candidate.size(), pngs.size())) return false;
        m_atlases.swap(candidate);
        return true;
    } catch (const std::exception&) {
        if (control) control->Fail(QStringLiteral("无法解码纹理图集，原图集保持不变"));
        return false;
    }
}

bool CTextureExtractor::ExtractMaterials(CSectionTable* table, QVector<ExtractedMaterial>& result, QString* error) const
{
    if (error) error->clear();
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    try {
        if (!table || !table->IsValid() || !table->count_info || !table->uvs || !table->position_indices ||
            table->count_info->art_meshes < 0 || table->count_info->art_meshes != table->art_meshes.size())
            return fail(QStringLiteral("无法提取 CMO3 素材：模型区段数据无效"));
        CModelMaterialOverrides materials;
        if (!CMocMaterialBuilder::Capture(*table, materials, error)) return false;
        using Slice = std::tuple<int, int, int>;
        using ImageKey = std::tuple<int, int, int, int, int>;
        std::map<Slice, QRect> uvBounds;
        std::map<Slice, bool> indexSlices;
        std::map<ImageKey, int> imageIndices;
        struct ImagePlan { int atlas; QRect rect; QImage image; };
        QVector<ImagePlan> images;
        QVector<ExtractedMaterial> candidate;
        QVector<int> materialImages;
        qint64 references = 0, pixels = 0;
        const auto consume = [&](qint64 count) {
            if (count > MaximumMaterialReferences - references) return false;
            references += count; return true;
        };
        const auto inRange = [](qint64 first, qint64 count, qint64 size) {
            return first >= 0 && count >= 0 && first <= size && count <= size - first;
        };
        for (int index = 0; index < table->art_meshes.size(); ++index) {
            const auto* mesh = table->art_meshes[index];
            if (!mesh) return fail(QStringLiteral("CMO3 素材引用了空网格：%1").arg(index));
            const QString label = QStringLiteral("%1（网格 %2）").arg(mesh->ids).arg(index);
            const int vertexCount = mesh->vertex_counts, indexCount = mesh->position_index_sources_counts;
            if (vertexCount < 0 || vertexCount > 1000000 || indexCount < 0 || indexCount % 3 || (!vertexCount && indexCount))
                return fail(QStringLiteral("CMO3 素材的顶点或三角索引数量无效：%1").arg(label));
            if (!vertexCount) continue;
            const int atlasIndex = mesh->texture_nos;
            if (atlasIndex < 0 || atlasIndex >= m_atlases.size() || m_atlases[atlasIndex].isNull())
                return fail(QStringLiteral("CMO3 素材缺少纹理图集：%1").arg(label));
            const auto& atlas = m_atlases[atlasIndex];
            if (!inRange(mesh->uv_sources_begin_indices, qint64(vertexCount) * 2, table->uvs->uvs.size()) ||
                (indexCount && !inRange(mesh->position_index_sources_begin_indices, indexCount, table->position_indices->indices.size())))
                return fail(QStringLiteral("CMO3 素材的 UV 或三角索引区间越界：%1").arg(label));
            if (indexCount) {
                const Slice slice{mesh->position_index_sources_begin_indices, indexCount, vertexCount};
                if (indexSlices.find(slice) == indexSlices.end()) {
                    if (!consume(indexCount)) return fail(QStringLiteral("CMO3 素材的顶点和索引检查总量超过限制"));
                    for (int at = 0; at < indexCount; ++at) {
                        const int vertex = table->position_indices->indices[mesh->position_index_sources_begin_indices + at];
                        if (vertex >= vertexCount) return fail(QStringLiteral("CMO3 素材的三角索引越界：%1").arg(label));
                    }
                    indexSlices.emplace(slice, true);
                }
            }
            const auto authorUvs = materials.value(mesh->ids).uvs;
            if (authorUvs.size() != vertexCount * 2) return fail(QStringLiteral("CMO3 素材缺少有效 UV：%1").arg(label));
            const Slice uvSlice{atlasIndex, mesh->uv_sources_begin_indices, vertexCount};
            auto foundBounds = uvBounds.find(uvSlice);
            if (foundBounds == uvBounds.end()) {
                if (!consume(vertexCount)) return fail(QStringLiteral("CMO3 素材的顶点和索引检查总量超过限制"));
                double minX = std::numeric_limits<double>::max(), minY = minX, maxX = -minX, maxY = -minX;
                for (int vertex = 0; vertex < vertexCount; ++vertex) {
                    const double u = authorUvs[vertex * 2];
                    const double v = authorUvs[vertex * 2 + 1];
                    if (!std::isfinite(u) || !std::isfinite(v)) return fail(QStringLiteral("CMO3 素材的 UV 包含非有限值：%1").arg(label));
                    const double x = u * atlas.width(), y = v * atlas.height();
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
                const double left = std::floor(minX) - 1, top = std::floor(minY) - 1;
                const double right = std::ceil(maxX) + 1, bottom = std::ceil(maxY) + 1;
                const double minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
                if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
                    right <= left || bottom <= top || right - left > maximum || bottom - top > maximum)
                    return fail(QStringLiteral("CMO3 素材的像素边界超出整数范围：%1").arg(label));
                const QRect rect(int(left), int(top), int(right - left), int(bottom - top));
                if (qint64(rect.width()) * rect.height() > MaximumMaterialPixels)
                    return fail(QStringLiteral("CMO3 单张素材像素数量超过限制：%1").arg(label));
                foundBounds = uvBounds.emplace(uvSlice, rect).first;
            }
            const QRect rect = foundBounds->second;
            const ImageKey key{atlasIndex, rect.x(), rect.y(), rect.width(), rect.height()};
            auto foundImage = imageIndices.find(key);
            if (foundImage == imageIndices.end()) {
                const qint64 added = qint64(rect.width()) * rect.height();
                if (added > MaximumTotalMaterialPixels - pixels)
                    return fail(QStringLiteral("CMO3 素材总像素数量超过限制"));
                pixels += added;
                foundImage = imageIndices.emplace(key, images.size()).first;
                images.append({atlasIndex, rect, {}});
            }
            candidate.append({mesh->ids, index, atlasIndex, {}, rect, atlas.size(), mesh->is_visible, authorUvs});
            materialImages.append(foundImage->second);
        }
        for (auto& plan : images) {
            const auto& atlas = m_atlases[plan.atlas]; const auto& rect = plan.rect;
            QImage image(rect.size(), QImage::Format_ARGB32);
            if (image.isNull()) return fail(QStringLiteral("无法分配 CMO3 素材图像"));
            for (int y = 0; y < rect.height(); ++y) {
                const int sourceY = repeatCoordinate(qint64(rect.y()) + y, atlas.height());
                const auto* source = reinterpret_cast<const QRgb*>(atlas.constScanLine(sourceY));
                auto* target = reinterpret_cast<QRgb*>(image.scanLine(y));
                int written = 0, sourceX = repeatCoordinate(rect.x(), atlas.width());
                while (written < rect.width()) {
                    const int count = std::min(rect.width() - written, atlas.width() - sourceX);
                    std::memcpy(target + written, source + sourceX, size_t(count) * sizeof(QRgb));
                    written += count; sourceX = 0;
                }
            }
            plan.image = std::move(image);
        }
        for (int index = 0; index < candidate.size(); ++index) candidate[index].image = images[materialImages[index]].image;
        result.swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备 CMO3 素材，原结果保持不变"));
    }
}

QSize CTextureExtractor::GetCanvasPixelSize(const CCanvas_info* canvasInfo)
{
    return canvasInfo ? CCanvasGeometry::PixelSize(canvasInfo->width, canvasInfo->height) : QSize();
}

uint32_t CTextureExtractor::BlendModeToPsd(int drawableFlags)
{
    switch (CDrawableFlags::BlendMode(drawableFlags)) {
    case 1:  return 0x6C646467; // 'lddg' Linear Dodge (Additive)
    case 2:  return 0x6D756C20; // 'mul ' Multiply
    default: return 0x6E6F726D; // 'norm' Normal
    }
}

QVector<ExtractedLayer> CTextureExtractor::ExtractAll(CSectionTable* table, bool includeHidden,
                                                      const QHash<QString, QVector<quint16>>* indexOverrides,
                                                      CExportControl* control,
                                                      const QHash<QString, QVector<float>>* modelPositions,
                                                      const QSet<QString>* excludedIds)
{
    CExportControl localControl;
    if (!control) control = &localControl;
    try {
        QVector<ExtractedLayer> results;
        if (control && !control->Checkpoint()) return {};
        if (!table || !table->IsValid() || !table->count_info || !table->uvs ||
            !table->position_indices || !table->keyform_positions || !table->canvas_info) {
            if (control) control->Fail(QStringLiteral("无法提取纹理：模型区段数据无效"));
            return results;
        }
        int meshCount = table->count_info->art_meshes;
        meshCount = std::min(meshCount, static_cast<int>(table->art_meshes.size()));
        results.reserve(meshCount);
        CModelMaterialOverrides materials;
        QString materialError;
        if (!CMocMaterialBuilder::Capture(*table, materials, &materialError)) { control->Fail(materialError); return {}; }
        qint64 pixels = 0;
        qint64 samples = 0;

        for (int i = 0; i < meshCount; ++i) {
            CArt_meshes* mesh = table->art_meshes[i];
            if (control && !control->Report(CExportControl::Phase::Extracting, i, meshCount, mesh->ids)) return {};

            if (excludedIds && excludedIds->contains(mesh->ids)) continue;

            // 跳过隐藏图层（如果不包括的话）
            if (!includeHidden && !mesh->is_visible) {
                continue;
            }

            const int keyformBegin = mesh->keyform_sources_begin_indices;
            const auto appendLayer = [&](const QImage& image, const QRect& rect, CPsdExportCoverage::Reason reason) -> bool {
                // 读取默认透明度
                float opacity = 1.0f;
                if (keyformBegin >= 0 && keyformBegin < table->art_mesh_keyforms.size()) {
                    opacity = table->art_mesh_keyforms[keyformBegin]->opacities;
                }

                // 读取裁剪蒙版信息
                QVector<int> maskIndices;
                int maskBegin = mesh->drawable_mask_sources_begin_indices;
                int maskCount = mesh->drawable_mask_sources_counts;
                const int maskSize = static_cast<int>(table->drawable_masks.size());
                if (maskBegin >= 0 && maskCount >= 0 && maskBegin <= maskSize && maskCount <= maskSize - maskBegin) {
                    for (int m = 0; m < maskCount; ++m) {
                        if (control && m % 1024 == 0 && !control->Checkpoint()) return false;
                        int maskIdx = maskBegin + m;
                        maskIndices.append(table->drawable_masks[maskIdx]->art_mesh_sources_indices);
                    }
                }

                // 构建结果
                ExtractedLayer layer;
                layer.image = image;
                layer.canvasRect = rect;
                layer.name = mesh->ids;
                layer.meshIndex = i;
                layer.textureNo = mesh->texture_nos;
                layer.drawableFlags = mesh->drawable_flags;
                layer.opacity = opacity;
                layer.isVisible = mesh->is_visible;
                layer.isEnabled = mesh->is_enabled;
                layer.parentPartIndex = mesh->parent_part_indices;
                layer.maskIndices = maskIndices;

                layer.emptyReason = reason;
                results.append(layer);
                return true;
            };
            if (modelPositions) {
                const auto found = modelPositions->constFind(mesh->ids);
                if (mesh->vertex_counts < 0 || mesh->vertex_counts > 1000000 || found == modelPositions->cend() ||
                    found->size() != qint64(mesh->vertex_counts) * 2 || !CCanvasGeometry::Validate(table->canvas_info)) {
                    control->Fail(QStringLiteral("纹理导出缺少有效的完整模型坐标：%1").arg(mesh->ids)); return {};
                }
                for (float value : *found) if (!std::isfinite(value)) {
                    control->Fail(QStringLiteral("纹理导出模型坐标不是有限数值：%1").arg(mesh->ids)); return {};
                }
            }
            if (mesh->vertex_counts >= 0 && mesh->position_index_sources_counts == 0 &&
                (!indexOverrides || !indexOverrides->contains(mesh->ids))) {
                if (modelPositions && !appendLayer({}, {}, mesh->vertex_counts == 0 ? CPsdExportCoverage::Reason::NoVertices
                                                                                : CPsdExportCoverage::Reason::NoTriangles)) return {};
                continue;
            }

            // 跳过无顶点的 mesh
            if (mesh->vertex_counts <= 0 || mesh->vertex_counts > 1000000 ||
                mesh->vertex_counts > std::numeric_limits<int>::max() / 2) {
                if (modelPositions) { control->Fail(QStringLiteral("纹理采样的网格顶点数量无效")); return {}; }
                continue;
            }

            // 检查纹理索引
            int texNo = mesh->texture_nos;
            if (texNo < 0 || texNo >= m_atlases.size()) {
                if (modelPositions) { control->Fail(QStringLiteral("纹理采样的图集索引无效")); return {}; }
                qWarning("CTextureExtractor: mesh %d (%s) 纹理索引 %d 越界",
                         i, mesh->ids.toUtf8().constData(), texNo);
                continue;
            }

            const QImage& atlas = m_atlases[texNo];
            if (modelPositions && atlas.isNull()) { control->Fail(QStringLiteral("纹理采样缺少图集")); return {}; }

            // 提取 UV 数据（归一化 [0,1]）
            int uvBegin = mesh->uv_sources_begin_indices;
            int uvCount = mesh->vertex_counts * 2;

            // 提取三角形索引
            int idxBegin = mesh->position_index_sources_begin_indices;
            int idxCount = mesh->position_index_sources_counts;
            const int uvSize = static_cast<int>(table->uvs->uvs.size());
            const int indexSize = static_cast<int>(table->position_indices->indices.size());
            if (uvBegin < 0 || uvCount < 0 || uvBegin > uvSize || uvCount > uvSize - uvBegin ||
                idxBegin < 0 || idxCount < 0 || idxCount % 3 != 0 || idxBegin > indexSize || idxCount > indexSize - idxBegin) {
                if (modelPositions) { control->Fail(QStringLiteral("纹理采样的网格数据范围无效")); return {}; }
                continue;
            }

            // 准备 UV 数据指针
            const auto authorUvs = materials.value(mesh->ids).uvs;
            if (authorUvs.size() != uvCount) { control->Fail(QStringLiteral("纹理提取缺少有效 UV")); return {}; }
            const float* uvPtr = authorUvs.constData();
            QVector<quint16> overrideIndices;
            const quint16* idxPtr = idxCount ? &table->position_indices->indices[idxBegin] : nullptr;
            if (indexOverrides) {
                const auto overrideIt = indexOverrides->constFind(mesh->ids);
                if (control && !control->Checkpoint()) return {};
                if (modelPositions && overrideIt != indexOverrides->cend() && !validIndices(overrideIt.value(), mesh->vertex_counts)) {
                    control->Fail(QStringLiteral("纹理采样的替换三角索引无效")); return {};
                }
                if (overrideIt != indexOverrides->cend() &&
                    validIndices(overrideIt.value(), mesh->vertex_counts)) {
                    if (control && !control->Checkpoint()) return {};
                    overrideIndices.reserve(overrideIt.value().size());
                    for (qsizetype index = 0; index < overrideIt.value().size(); ++index) {
                        if (control && index % 1024 == 0 && !control->Checkpoint()) return {};
                        overrideIndices.append(overrideIt.value()[index]);
                    }
                    idxPtr = overrideIndices.constData();
                    idxCount = overrideIndices.size();
                }
            }
            if (!idxCount) continue;
            bool indicesValid = true;
            for (int t = 0; t < idxCount; ++t) {
                if (control && t % 1024 == 0 && !control->Checkpoint()) return {};
                if (idxPtr[t] >= mesh->vertex_counts) {
                    indicesValid = false;
                    break;
                }
            }
            if (!indicesValid) {
                if (modelPositions) { control->Fail(QStringLiteral("纹理采样的三角索引越界")); return {}; }
                continue;
            }

            QRect uvPixelRect;
            QImage meshImage;
            QRect canvasRect;
            auto emptyReason = CPsdExportCoverage::Reason::None;
            if (modelPositions) {
                const auto found = modelPositions->constFind(mesh->ids);
                if (found == modelPositions->cend() || found->size() != uvCount) {
                    if (control) control->Fail(QStringLiteral("纹理导出缺少完整的模型坐标：%1").arg(mesh->ids));
                    return {};
                }
                meshImage = RasterizeCanvasMesh(atlas, uvPtr, found->constData(), mesh->vertex_counts,
                                                idxPtr, idxCount, table->canvas_info, canvasRect, pixels, samples, control, emptyReason);
                if (!control->Checkpoint() || (meshImage.isNull() && emptyReason == CPsdExportCoverage::Reason::None)) return {};
            } else {
                meshImage = RasterizeMesh(atlas, uvPtr, mesh->vertex_counts,
                                           idxPtr, idxCount, uvPixelRect, pixels, samples, control);
                if (!control->Checkpoint()) return {};
                if (meshImage.isNull()) continue;
            }
            if (!modelPositions && keyformBegin >= 0 && keyformBegin < table->art_mesh_keyforms.size()) {
                int posBegin = table->art_mesh_keyforms[keyformBegin]->keyform_position_sources_begin_indices;
                const int positionSize = static_cast<int>(table->keyform_positions->xys.size());
                if (posBegin >= 0 && posBegin <= positionSize && uvCount <= positionSize - posBegin) {
                    const float* posPtr = &table->keyform_positions->xys[posBegin];
                    canvasRect = ComputeCanvasRect(posPtr, mesh->vertex_counts, table->canvas_info, control);
                    if (control && !control->Checkpoint()) return {};
                }
            }
            if (canvasRect.isEmpty()) canvasRect = uvPixelRect;

            if (!appendLayer(meshImage, canvasRect, emptyReason)) return {};
        }

        if (control && !control->Report(CExportControl::Phase::Extracting, meshCount, meshCount)) return {};
        return results;
    } catch (const std::exception& exception) {
        if (control) control->Fail(QStringLiteral("提取纹理失败：%1").arg(QString::fromUtf8(exception.what())));
        return {};
    }
}

QImage CTextureExtractor::RasterizeCanvasMesh(const QImage& atlas, const float* uvs, const float* positions, int vertexCount,
                                               const quint16* indices, int indexCount, const CCanvas_info* canvas,
                                               QRect& bounds, qint64& pixels, qint64& samples, CExportControl* control,
                                               CPsdExportCoverage::Reason& emptyReason)
{
    const auto fail = [&](const QString& reason) { control->Fail(reason); return QImage(); };
    bounds = ComputeCanvasRect(positions, vertexCount, canvas, control, &emptyReason);
    if (!control->Checkpoint()) return {};
    if (bounds.isEmpty()) return {};
    if (indexCount < 3 || indexCount > 3000000 || indexCount % 3)
        return fail(QStringLiteral("纹理采样的三角索引数量无效"));
    const qint64 count = qint64(bounds.width()) * bounds.height();
    if (count > MaximumMaterialPixels || count > MaximumTotalMaterialPixels - pixels)
        return fail(QStringLiteral("纹理采样的画布像素总量超出限制"));

    QVector<QPointF> vertices;
    vertices.reserve(vertexCount);
    for (int index = 0; index < vertexCount; ++index) {
        if (index % 1024 == 0 && !control->Checkpoint()) return {};
        QPointF point;
        if (!std::isfinite(uvs[index * 2]) || !std::isfinite(uvs[index * 2 + 1]) ||
            !CCanvasGeometry::ModelToPixels(canvas, QPointF(positions[index * 2], positions[index * 2 + 1]), point))
            return fail(QStringLiteral("纹理采样的顶点或 UV 坐标无效"));
        vertices.append(point - QPointF(bounds.topLeft()));
    }
    struct Triangle { int a, b, c; QRect scan; double determinant; };
    QVector<Triangle> triangles;
    triangles.reserve(indexCount / 3);
    for (int index = 0; index < indexCount; index += 3) {
        if (index % 1023 == 0 && !control->Checkpoint()) return {};
        const int a = indices[index], b = indices[index + 1],
                  c = indices[index + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
            return fail(QStringLiteral("纹理采样的三角索引越界"));
        const QPointF pa = vertices[a], pb = vertices[b], pc = vertices[c];
        if (CUvGeometry::Orientation(pa, pb, pc) == 0) continue;
        const double determinant = cross(pb - pa, pc - pa);
        if (determinant == 0.0 || !std::isfinite(determinant))
            return fail(QStringLiteral("纹理采样的三角形无法稳定求值"));
        const int left = std::max(0, int(std::floor(std::min({pa.x(), pb.x(), pc.x()}))));
        const int top = std::max(0, int(std::floor(std::min({pa.y(), pb.y(), pc.y()}))));
        const int right = std::min(bounds.width(), int(std::ceil(std::max({pa.x(), pb.x(), pc.x()}))));
        const int bottom = std::min(bounds.height(), int(std::ceil(std::max({pa.y(), pb.y(), pc.y()}))));
        if (right <= left || bottom <= top) continue;
        const qint64 scans = qint64(right - left) * (bottom - top);
        if (scans > MaximumRasterSamples - samples)
            return fail(QStringLiteral("纹理采样的三角形扫描总量超出限制"));
        samples += scans;
        triangles.append({a, b, c, QRect(left, top, right - left, bottom - top), determinant});
    }
    pixels += count;
    QImage output(bounds.size(), QImage::Format_ARGB32);
    if (output.isNull()) return fail(QStringLiteral("无法分配纹理采样图像"));
    output.fill(Qt::transparent);
    for (const auto& triangle : triangles) {
        const QPointF a = vertices[triangle.a], b = vertices[triangle.b], c = vertices[triangle.c];
        const QPointF uvA(uvs[triangle.a * 2], uvs[triangle.a * 2 + 1]);
        const QPointF uvB(uvs[triangle.b * 2], uvs[triangle.b * 2 + 1]);
        const QPointF uvC(uvs[triangle.c * 2], uvs[triangle.c * 2 + 1]);
        for (int y = triangle.scan.top(); y <= triangle.scan.bottom(); ++y) {
            if (!control->Report(CExportControl::Phase::Extracting, y - triangle.scan.top(), triangle.scan.height(),
                                 QStringLiteral("采样画布三角形"))) return {};
            QRgb* row = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = triangle.scan.left(); x <= triangle.scan.right(); ++x) {
                if ((x - triangle.scan.left()) % 4096 == 0 && !control->Checkpoint()) return {};
                const QPointF point(x + 0.5, y + 0.5);
                if (!CUvGeometry::ContainsTriangle(point, a, b, c)) continue;
                const double wb = cross(point - a, c - a) / triangle.determinant;
                const double wc = cross(b - a, point - a) / triangle.determinant;
                const QPointF uv = uvA + wb * (uvB - uvA) + wc * (uvC - uvA);
                if (!std::isfinite(uv.x()) || !std::isfinite(uv.y()))
                    return fail(QStringLiteral("纹理采样的插值坐标无效"));
                // 重叠区域按三角索引顺序覆盖，透明像素仍保留直通 RGBA。
                row[x] = sampleBilinear(atlas, uv.x(), uv.y());
            }
        }
    }
    return control->Checkpoint() ? output : QImage();
}

QImage CTextureExtractor::RasterizeMesh(const QImage& atlas,
                                         const float* uvData, int vertexCount,
                                         const quint16* indexData, int indexCount,
                                         QRect& outUvRect, qint64& pixels, qint64& samples, CExportControl* control)
{
    outUvRect = QRect();
    const int atlasW = atlas.width(), atlasH = atlas.height();
    if (atlas.isNull() || !uvData || !indexData || vertexCount <= 0 || vertexCount > 1000000 ||
        indexCount < 3 || indexCount > 3000000 || indexCount % 3 != 0) return {};
    double minPx = std::numeric_limits<double>::max(), minPy = minPx;
    double maxPx = -minPx, maxPy = -minPx;
    for (int vertex = 0; vertex < vertexCount; ++vertex) {
        if (control && vertex % 1024 == 0 && !control->Checkpoint()) return {};
        const double u = uvData[vertex * 2], v = uvData[vertex * 2 + 1];
        if (!std::isfinite(u) || !std::isfinite(v)) return {};
        const double x = u * atlasW, y = v * atlasH;
        minPx = std::min(minPx, x);
        minPy = std::min(minPy, y);
        maxPx = std::max(maxPx, x);
        maxPy = std::max(maxPy, y);
    }
    const double leftValue = minPx >= 0 ? std::max(0.0, std::floor(minPx) - 1) : std::floor(minPx) - 1;
    const double topValue = minPy >= 0 ? std::max(0.0, std::floor(minPy) - 1) : std::floor(minPy) - 1;
    const double rightValue = maxPx <= atlasW ? std::min(double(atlasW), std::ceil(maxPx) + 1) : std::ceil(maxPx) + 1;
    const double bottomValue = maxPy <= atlasH ? std::min(double(atlasH), std::ceil(maxPy) + 1) : std::ceil(maxPy) + 1;
    const double width = rightValue - leftValue, height = bottomValue - topValue;
    if (leftValue < std::numeric_limits<int>::min() || topValue < std::numeric_limits<int>::min() ||
        rightValue > std::numeric_limits<int>::max() || bottomValue > std::numeric_limits<int>::max() ||
        width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
        width * height > MaximumMaterialPixels || width * height > MaximumTotalMaterialPixels - pixels) {
        if (control) control->Fail(QStringLiteral("纹理提取的完整 UV 区域超出像素或坐标限制"));
        return {};
    }
    const int left = int(leftValue), top = int(topValue), right = int(rightValue), bottom = int(bottomValue);
    for (int triangle = 0; triangle < indexCount; triangle += 3) {
        if (control && triangle % 1023 == 0 && !control->Checkpoint()) return {};
        const int a = indexData[triangle], b = indexData[triangle + 1],
                  c = indexData[triangle + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        const QPointF pa(double(uvData[a * 2]) * atlasW, double(uvData[a * 2 + 1]) * atlasH);
        const QPointF pb(double(uvData[b * 2]) * atlasW, double(uvData[b * 2 + 1]) * atlasH);
        const QPointF pc(double(uvData[c * 2]) * atlasW, double(uvData[c * 2 + 1]) * atlasH);
        if (CUvGeometry::Orientation(pa, pb, pc) == 0) continue;
        const double firstX = std::max(0.0, std::floor(std::min({pa.x(), pb.x(), pc.x()}) - left));
        const double firstY = std::max(0.0, std::floor(std::min({pa.y(), pb.y(), pc.y()}) - top));
        const double lastX = std::min(width - 1, std::ceil(std::max({pa.x(), pb.x(), pc.x()}) - left));
        const double lastY = std::min(height - 1, std::ceil(std::max({pa.y(), pb.y(), pc.y()}) - top));
        if (lastX < firstX || lastY < firstY) continue;
        const qint64 scans = qint64(lastX - firstX + 1) * qint64(lastY - firstY + 1);
        if (scans > MaximumRasterSamples - samples) {
            if (control) control->Fail(QStringLiteral("纹理裁片的三角形扫描总量超出限制"));
            return {};
        }
        samples += scans;
    }
    pixels += qint64(width) * qint64(height);
    QImage output(right - left, bottom - top, QImage::Format_ARGB32);
    if (output.isNull()) {
        if (control) control->Fail(QStringLiteral("无法分配纹理裁片图像"));
        return {};
    }
    output.fill(Qt::transparent);
    outUvRect = QRect(left, top, right - left, bottom - top);
    for (int triangle = 0; triangle < indexCount; triangle += 3) {
        if (control && !control->Checkpoint()) return {};
        const int i0 = indexData[triangle], i1 = indexData[triangle + 1],
            i2 = indexData[triangle + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
        const double x0 = double(uvData[i0 * 2]) * atlasW;
        const double y0 = double(uvData[i0 * 2 + 1]) * atlasH;
        const double x1 = double(uvData[i1 * 2]) * atlasW;
        const double y1 = double(uvData[i1 * 2 + 1]) * atlasH;
        const double x2 = double(uvData[i2 * 2]) * atlasW;
        const double y2 = double(uvData[i2 * 2 + 1]) * atlasH;
        if (!RasterizeTriangle(output, atlas, x0, y0, x1, y1, x2, y2, left, top, control)) return {};
    }
    return output;
}

QRect CTextureExtractor::ComputeCanvasRect(const float* positions, int vertexCount,
                                            const CCanvas_info* canvasInfo, CExportControl* control, CPsdExportCoverage::Reason* emptyReason)
{
    if (emptyReason) *emptyReason = CPsdExportCoverage::Reason::None;
    const auto invalid = [&] {
        if (emptyReason && control) control->Fail(QStringLiteral("纹理采样的画布坐标无效或超出范围"));
        return QRect();
    };
    if (!positions || !canvasInfo || vertexCount <= 0 || vertexCount > 1000000) return invalid();
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = -minX, maxY = -minX;
    for (int vertex = 0; vertex < vertexCount; ++vertex) {
        if (control && vertex % 1024 == 0 && !control->Checkpoint()) return {};
        QPointF pixels;
        if (!CCanvasGeometry::ModelToPixels(canvasInfo, QPointF(positions[vertex * 2], positions[vertex * 2 + 1]), pixels)) return invalid();
        minX = std::min(minX, pixels.x());
        minY = std::min(minY, pixels.y());
        maxX = std::max(maxX, pixels.x());
        maxY = std::max(maxY, pixels.y());
    }
    const double left = std::floor(minX), top = std::floor(minY);
    const double right = std::ceil(maxX), bottom = std::ceil(maxY);
    const double minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
    if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
        right - left > maximum || bottom - top > maximum) return invalid();
    if (emptyReason && (minX == maxX || minY == maxY)) {
        *emptyReason = CPsdExportCoverage::Reason::DegenerateBounds;
        return {};
    }
    if (right <= left || bottom <= top) return invalid();
    return QRect(static_cast<int>(left), static_cast<int>(top),
                 static_cast<int>(right - left), static_cast<int>(bottom - top));
}

bool CTextureExtractor::RasterizeTriangle(QImage& output, const QImage& atlas,
                                           double x0, double y0, double x1, double y1,
                                           double x2, double y2, int atlasLeft, int atlasTop, CExportControl* control)
{
    const double left = std::max(0.0, std::floor(std::min({x0, x1, x2}) - atlasLeft));
    const double top = std::max(0.0, std::floor(std::min({y0, y1, y2}) - atlasTop));
    const double right = std::min(double(output.width() - 1), std::ceil(std::max({x0, x1, x2}) - atlasLeft));
    const double bottom = std::min(double(output.height() - 1), std::ceil(std::max({y0, y1, y2}) - atlasTop));
    if (right < left || bottom < top) return true;
    const QPointF a(x0, y0), b(x1, y1), c(x2, y2);
    if (CUvGeometry::Orientation(a, b, c) == 0) return true;
    const int minX = static_cast<int>(left), minY = static_cast<int>(top);
    const int maxX = static_cast<int>(right), maxY = static_cast<int>(bottom);
    for (int py = minY; py <= maxY; ++py) {
        if (control && !control->Report(CExportControl::Phase::Extracting, py - minY, maxY - minY + 1,
                                        QStringLiteral("光栅化纹理三角形"))) return false;
        QRgb* outputRow = reinterpret_cast<QRgb*>(output.scanLine(py));
        for (int px = minX; px <= maxX; ++px) {
            if (control && (px - minX) % 4096 == 0 && !control->Checkpoint()) return false;
            const QPointF point(double(px) + atlasLeft + 0.5, double(py) + atlasTop + 0.5);
            if (!CUvGeometry::ContainsTriangle(point, a, b, c)) continue;
            // UV 裁片只改变原点，直接采样可避免大坐标重心插值抵消。
            const qint64 sampleX = qint64(px) + atlasLeft, sampleY = qint64(py) + atlasTop;
            const QRgb pixel = reinterpret_cast<const QRgb*>(atlas.constScanLine(repeatCoordinate(sampleY, atlas.height())))
                [repeatCoordinate(sampleX, atlas.width())];
            if (qAlpha(pixel) > 0 || qAlpha(outputRow[px]) == 0) outputRow[px] = pixel;
        }
    }
    return !control || control->Checkpoint();
}
