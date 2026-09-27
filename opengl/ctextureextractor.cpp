#include "ctextureextractor.h"
#include "cuvgeometry.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmeshtopology.h"
#include "live2dstruct/cdrawableflags.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include "live2dstruct/ctextureassets.h"
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
                        if (vertex < 0 || vertex >= vertexCount) return fail(QStringLiteral("CMO3 素材的三角索引越界：%1").arg(label));
                    }
                    indexSlices.emplace(slice, true);
                }
            }
            const Slice uvSlice{atlasIndex, mesh->uv_sources_begin_indices, vertexCount};
            auto foundBounds = uvBounds.find(uvSlice);
            if (foundBounds == uvBounds.end()) {
                if (!consume(vertexCount)) return fail(QStringLiteral("CMO3 素材的顶点和索引检查总量超过限制"));
                double minX = std::numeric_limits<double>::max(), minY = minX, maxX = -minX, maxY = -minX;
                for (int vertex = 0; vertex < vertexCount; ++vertex) {
                    const double u = table->uvs->uvs[mesh->uv_sources_begin_indices + vertex * 2];
                    const double v = table->uvs->uvs[mesh->uv_sources_begin_indices + vertex * 2 + 1];
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
            candidate.append({mesh->ids, index, atlasIndex, {}, rect, atlas.size(), mesh->is_visible});
            materialImages.append(foundImage->second);
        }
        for (auto& plan : images) {
            const auto& atlas = m_atlases[plan.atlas]; const auto& rect = plan.rect;
            QImage image(rect.size(), QImage::Format_ARGB32);
            if (image.isNull()) return fail(QStringLiteral("无法分配 CMO3 素材图像"));
            const int first = int(std::clamp(-qint64(rect.x()), qint64(0), qint64(rect.width())));
            const int last = int(std::clamp(qint64(atlas.width()) - rect.x(), qint64(0), qint64(rect.width())));
            for (int y = 0; y < rect.height(); ++y) {
                const int sourceY = int(std::clamp(qint64(rect.y()) + y, qint64(0), qint64(atlas.height() - 1)));
                const auto* source = reinterpret_cast<const QRgb*>(atlas.constScanLine(sourceY));
                auto* target = reinterpret_cast<QRgb*>(image.scanLine(y));
                std::fill(target, target + first, source[0]);
                if (last > first) std::memcpy(target + first, source + qint64(rect.x()) + first, size_t(last - first) * sizeof(QRgb));
                std::fill(target + last, target + rect.width(), source[atlas.width() - 1]);
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
                                                      const QHash<QString, QVector<float>>* modelPositions)
{
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

        for (int i = 0; i < meshCount; ++i) {
            CArt_meshes* mesh = table->art_meshes[i];
            if (control && !control->Report(CExportControl::Phase::Extracting, i, meshCount, mesh->ids)) return {};

            // 跳过隐藏图层（如果不包括的话）
            if (!includeHidden && !mesh->is_visible) {
                continue;
            }

            // 跳过无顶点的 mesh
            if (mesh->vertex_counts <= 0 || mesh->vertex_counts > 1000000 ||
                mesh->vertex_counts > std::numeric_limits<int>::max() / 2) {
                continue;
            }

            // 检查纹理索引
            int texNo = mesh->texture_nos;
            if (texNo < 0 || texNo >= m_atlases.size()) {
                qWarning("CTextureExtractor: mesh %d (%s) 纹理索引 %d 越界",
                         i, mesh->ids.toUtf8().constData(), texNo);
                continue;
            }

            const QImage& atlas = m_atlases[texNo];

            // 提取 UV 数据（归一化 [0,1]）
            int uvBegin = mesh->uv_sources_begin_indices;
            int uvCount = mesh->vertex_counts * 2;

            // 提取三角形索引
            int idxBegin = mesh->position_index_sources_begin_indices;
            int idxCount = mesh->position_index_sources_counts;
            const int uvSize = static_cast<int>(table->uvs->uvs.size());
            const int indexSize = static_cast<int>(table->position_indices->indices.size());
            if (uvBegin < 0 || uvCount < 0 || uvBegin > uvSize || uvCount > uvSize - uvBegin ||
                idxBegin < 0 || idxCount < 3 || idxCount % 3 != 0 || idxBegin > indexSize || idxCount > indexSize - idxBegin) {
                continue;
            }

            // 准备 UV 数据指针
            const float* uvPtr = &table->uvs->uvs[uvBegin];
            QVector<short> overrideIndices;
            const short* idxPtr = &table->position_indices->indices[idxBegin];
            if (indexOverrides) {
                const auto overrideIt = indexOverrides->constFind(mesh->ids);
                if (control && !control->Checkpoint()) return {};
                if (overrideIt != indexOverrides->cend() &&
                    CMeshTopology::IsValid(overrideIt.value(), mesh->vertex_counts)) {
                    if (control && !control->Checkpoint()) return {};
                    overrideIndices.reserve(overrideIt.value().size());
                    for (qsizetype index = 0; index < overrideIt.value().size(); ++index) {
                        if (control && index % 1024 == 0 && !control->Checkpoint()) return {};
                        overrideIndices.append(static_cast<short>(overrideIt.value()[index]));
                    }
                    idxPtr = overrideIndices.constData();
                    idxCount = overrideIndices.size();
                }
            }
            bool indicesValid = true;
            for (int t = 0; t < idxCount; ++t) {
                if (control && t % 1024 == 0 && !control->Checkpoint()) return {};
                if (idxPtr[t] < 0 || idxPtr[t] >= mesh->vertex_counts) {
                    indicesValid = false;
                    break;
                }
            }
            if (!indicesValid) continue;

            // 通过 UV 三角形光栅化从 atlas 提取原始纹理
            QRect uvPixelRect;
            QImage meshImage = RasterizeMesh(atlas, uvPtr, mesh->vertex_counts,
                                             idxPtr, idxCount, uvPixelRect, control);
            if (control && !control->Checkpoint()) return {};

            if (meshImage.isNull() || meshImage.width() == 0 || meshImage.height() == 0) {
                continue;
            }
            QRect canvasRect;
            int keyformBegin = mesh->keyform_sources_begin_indices;
            if (modelPositions) {
                const auto found = modelPositions->constFind(mesh->ids);
                if (found == modelPositions->cend() || found->size() != uvCount) {
                    if (control) control->Fail(QStringLiteral("纹理导出缺少完整的模型坐标：%1").arg(mesh->ids));
                    return {};
                }
                canvasRect = ComputeCanvasRect(found->constData(), mesh->vertex_counts, table->canvas_info, control);
                if (canvasRect.isEmpty()) {
                    if (control && control->Checkpoint()) control->Fail(QStringLiteral("当前姿态的网格没有有效画布边界：%1").arg(mesh->ids));
                    return {};
                }
            } else if (keyformBegin >= 0 && keyformBegin < table->art_mesh_keyforms.size()) {
                int posBegin = table->art_mesh_keyforms[keyformBegin]->keyform_position_sources_begin_indices;
                const int positionSize = static_cast<int>(table->keyform_positions->xys.size());
                if (posBegin >= 0 && posBegin <= positionSize && uvCount <= positionSize - posBegin) {
                    const float* posPtr = &table->keyform_positions->xys[posBegin];
                    canvasRect = ComputeCanvasRect(posPtr, mesh->vertex_counts, table->canvas_info, control);
                    if (control && !control->Checkpoint()) return {};
                }
            } else {
                // 回退：使用 UV 像素区域
                canvasRect = uvPixelRect;
            }
            if (canvasRect.isEmpty()) canvasRect = uvPixelRect;

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
                    if (control && m % 1024 == 0 && !control->Checkpoint()) return {};
                    int maskIdx = maskBegin + m;
                    maskIndices.append(table->drawable_masks[maskIdx]->art_mesh_sources_indices);
                }
            }

            // 构建结果
            ExtractedLayer layer;
            layer.image = meshImage;
            layer.canvasRect = canvasRect;
            layer.name = mesh->ids;
            layer.meshIndex = i;
            layer.textureNo = texNo;
            layer.drawableFlags = mesh->drawable_flags;
            layer.opacity = opacity;
            layer.isVisible = mesh->is_visible;
            layer.isEnabled = mesh->is_enabled;
            layer.parentPartIndex = mesh->parent_part_indices;
            layer.maskIndices = maskIndices;

            results.append(layer);
        }

        if (control && !control->Report(CExportControl::Phase::Extracting, meshCount, meshCount)) return {};
        return results;
    } catch (const std::exception& exception) {
        if (control) control->Fail(QStringLiteral("提取纹理失败：%1").arg(QString::fromUtf8(exception.what())));
        return {};
    }
}

QImage CTextureExtractor::RasterizeMesh(const QImage& atlas,
                                         const float* uvData, int vertexCount,
                                         const short* indexData, int indexCount,
                                         QRect& outUvRect, CExportControl* control)
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
    if (maxPx < 0.0 || maxPy < 0.0 || minPx > atlasW || minPy > atlasH) return {};
    const int left = static_cast<int>(std::max(0.0, std::floor(std::clamp(minPx, 0.0, double(atlasW))) - 1.0));
    const int top = static_cast<int>(std::max(0.0, std::floor(std::clamp(minPy, 0.0, double(atlasH))) - 1.0));
    const int right = static_cast<int>(std::min(double(atlasW), std::ceil(std::clamp(maxPx, 0.0, double(atlasW))) + 1.0));
    const int bottom = static_cast<int>(std::min(double(atlasH), std::ceil(std::clamp(maxPy, 0.0, double(atlasH))) + 1.0));
    if (right <= left || bottom <= top) return {};
    QImage output(right - left, bottom - top, QImage::Format_ARGB32);
    if (output.isNull()) {
        if (control) control->Fail(QStringLiteral("无法分配纹理裁片图像"));
        return {};
    }
    output.fill(Qt::transparent);
    outUvRect = QRect(left, top, right - left, bottom - top);
    for (int triangle = 0; triangle < indexCount; triangle += 3) {
        if (control && !control->Checkpoint()) return {};
        const int i0 = indexData[triangle], i1 = indexData[triangle + 1], i2 = indexData[triangle + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
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
                                            const CCanvas_info* canvasInfo, CExportControl* control)
{
    if (!positions || !canvasInfo || vertexCount <= 0 || vertexCount > 1000000) return {};
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = -minX, maxY = -minX;
    for (int vertex = 0; vertex < vertexCount; ++vertex) {
        if (control && vertex % 1024 == 0 && !control->Checkpoint()) return {};
        QPointF pixels;
        if (!CCanvasGeometry::ModelToPixels(canvasInfo, QPointF(positions[vertex * 2], positions[vertex * 2 + 1]), pixels)) return {};
        minX = std::min(minX, pixels.x());
        minY = std::min(minY, pixels.y());
        maxX = std::max(maxX, pixels.x());
        maxY = std::max(maxY, pixels.y());
    }
    const double left = std::floor(minX), top = std::floor(minY);
    const double right = std::ceil(maxX), bottom = std::ceil(maxY);
    const double minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
    if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
        right <= left || bottom <= top || right - left > maximum || bottom - top > maximum) return {};
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
            if (sampleX < 0 || sampleY < 0 || sampleX >= atlas.width() || sampleY >= atlas.height()) continue;
            const QRgb pixel = reinterpret_cast<const QRgb*>(atlas.constScanLine(static_cast<int>(sampleY)))[sampleX];
            if (qAlpha(pixel) > 0 || qAlpha(outputRow[px]) == 0) outputRow[px] = pixel;
        }
    }
    return !control || control->Checkpoint();
}
